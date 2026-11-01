#include <medicon/scripts/ScriptManager.hpp>
#include <windows.h>
#include <algorithm>
#include <set>
#include <regex>
#include <fstream>

namespace medicon::scripts {
namespace {
std::string process_version() {
    wchar_t path[32768]{};
    if (!GetModuleFileNameW(nullptr, path, 32768)) return "unavailable";
    if (_wcsicmp(fs::path(path).filename().c_str(), L"Minecraft.Windows.exe") != 0) return "preview / non-Minecraft host";
    DWORD ignored{}; const auto size = GetFileVersionInfoSizeW(path, &ignored);
    std::vector<unsigned char> buffer(size);
    if (!size || !GetFileVersionInfoW(path, 0, size, buffer.data())) return "unknown Minecraft version";
    VS_FIXEDFILEINFO* info{}; UINT length{};
    if (!VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void**>(&info), &length) || length < sizeof(*info)) return "unknown Minecraft version";
    return std::to_string(HIWORD(info->dwFileVersionMS)) + "." + std::to_string(LOWORD(info->dwFileVersionMS)) + "." +
        std::to_string(HIWORD(info->dwFileVersionLS)) + "." + std::to_string(LOWORD(info->dwFileVersionLS));
}
}

ScriptManager::ScriptManager(ModuleManager& modules, fs::path config, fs::path compiler)
    : modules_(modules), config_(std::move(config)), paths_(config_.path().parent_path()), compiler_(std::move(compiler)),
      trust_(paths_.root / "script-trust.json") {
    services_.log = [this](uint32_t severity, std::string_view message) { audit(severity, message); };
    services_.storage=std::make_shared<ScriptStorage>(paths_.root/"Storage",[this](std::string_view message){audit(2,message);});
}
ScriptManager::~ScriptManager() { stop(); }
void ScriptManager::start() {
    if (worker_.joinable()) return;
    stopping_ = false;
    { std::lock_guard lock(mutex_); status_.detecting = true; }
    worker_ = std::thread([this] { worker(); });
}
void ScriptManager::stop() {
    stopping_ = true; wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::vector<std::string> keys;
    for (const auto& [key, _] : active_) keys.push_back(key);
    for (const auto& key : keys) remove_active(key, false);
    services_.storage->stop();
    std::deque<std::pair<uint32_t, std::string>> logs;
    { std::lock_guard lock(mutex_); logs.swap(audit_); ready_.clear(); requests_.clear(); pending_.clear(); views_.clear(); }
    for (const auto& [_, message] : logs) {
        try { append_audit(paths_, "shutdown", message); } catch (...) { /* Stop must still unload on a full disk. */ }
    }
}
void ScriptManager::audit(uint32_t severity, std::string_view message) {
    { std::lock_guard lock(mutex_);
        if (audit_.size() >= 1024) audit_.pop_front();
        audit_.emplace_back(severity, std::string(message.substr(0, 16384)));
    }
    wake_.notify_one();
}
void ScriptManager::set_mcbe_version(std::string version) { std::lock_guard lock(mutex_); status_.mcbe_version = std::move(version); }
ScriptStatus ScriptManager::snapshot() const {
    std::lock_guard lock(mutex_);
    auto result = status_; result.features = services_.features;
    for (const auto& [_, view] : views_) result.scripts.push_back(view);
    return result;
}
void ScriptManager::request_build(std::string key, bool force) {
    {
        std::lock_guard lock(mutex_);
        if (requests_.size() >= 256) { status_.error = "Script request queue is full; wait for the current operation to finish."; return; }
        if (std::ranges::any_of(requests_, [&](const auto& r) { return r.kind == Request::build && (r.key.empty() || r.key == key); })) return;
        for (const auto& [source, view] : views_) if ((key.empty() || key == source) && view.progress.running()) return;
        for (auto& [source, view] : views_) if ((key.empty() || key == source) && !view.source.empty()) {
            view.status = "Build queued";
            view.progress = {BuildStage::queued, "Waiting for the build worker..."};
            view.approval_required = false; view.diagnostics.clear(); view.log.clear(); ++view.generation;
            pending_.erase(source);
        }
        status_.build_status = key.empty() ? "Build / Reload all queued..." : "Build queued: " + key;
        requests_.push_back({Request::build, std::move(key), {}, force});
    }
    wake_.notify_one();
}
void ScriptManager::request_approval(std::string key, std::string binary_hash) {
    {
        std::lock_guard lock(mutex_);
        const auto found = views_.find(key);
        if (requests_.size() >= 256 || found == views_.end() || !found->second.approval_required ||
            found->second.binary_hash != binary_hash || found->second.progress.running()) return;
        found->second.status = "Load queued";
        found->second.progress.stage = BuildStage::loading;
        found->second.progress.message = "Waiting to verify approval and load the DLL...";
        requests_.push_back({Request::approve, std::move(key), std::move(binary_hash)});
    }
    wake_.notify_one();
}
void ScriptManager::request_revoke(std::string key) {
    { std::lock_guard lock(mutex_); if (requests_.size() < 256) requests_.push_back({Request::revoke, std::move(key), {}}); }
    wake_.notify_one();
}
void ScriptManager::request_create(std::string name,std::string template_name) {
    { std::lock_guard lock(mutex_); if(requests_.size()<256) requests_.push_back({Request::create,std::move(name),std::move(template_name)}); }
    wake_.notify_one();
}
void ScriptManager::set_result(const BuildResult& b, uint64_t generation, bool trusted) {
    std::lock_guard lock(mutex_);
    auto& view = views_[b.source.key];
    if (view.generation != generation) return;
    view.source = b.source.entry; view.log = b.log; view.diagnostics = b.diagnostics;
    view.approval_required = b.success && !trusted;
    view.binary_hash = b.binary_hash; view.fingerprint = b.source.fingerprint;
    if (b.success) {
        view.id = b.manifest.id; view.name = b.manifest.name;
        view.status = trusted ? "Ready to reload" : "Approval required before native code can run";
        view.progress.stage = trusted ? BuildStage::loading : BuildStage::complete;
        view.progress.message = b.cached ? "Build succeeded (verified cache); " : "Build succeeded; ";
        view.progress.message += trusted ? "waiting to reload..." : "choose Approve & Load to register the module.";
        pending_[b.source.key] = b;
        if (trusted) ready_.push_back({b, generation});
    } else {
        view.status = view.active ? "Build failed; previous version remains active" : "Build failed";
        view.progress.stage = BuildStage::failed;
        pending_.erase(b.source.key);
    }
}

void ScriptManager::worker() {
    try {
        paths_.initialize(); services_.storage->start(); set_mcbe_version(process_version());
        append_audit(paths_, "start", "Script manager started; native code requires exact source and binary approval");
        std::string error;
        auto toolchain = ScriptToolchain::detect(compiler_, stopping_, error);
        {
            std::lock_guard lock(mutex_); status_.detecting = false; status_.error = error;
            status_.setup_url = "https://visualstudio.microsoft.com/visual-cpp-build-tools/";
            if (toolchain) { status_.toolchain = utf8(toolchain->compiler); status_.version = toolchain->version; status_.license = toolchain->license; status_.setup_url = toolchain->setup_url; }
        }
        struct Watched {
            std::string observed, built;
            std::chrono::steady_clock::time_point changed{};
            std::vector<fs::path> dependencies;
            bool forced = false;
        };
        std::map<std::string, Watched> watched;
        std::optional<ScriptCompiler> compiler;
        std::string identity;
        if (toolchain) { identity = toolchain->identity(); compiler.emplace(paths_, std::move(*toolchain)); }
        std::string database_signature;
        while (!stopping_) {
            std::deque<Request> requests; std::deque<std::pair<uint32_t, std::string>> logs;
            { std::lock_guard lock(mutex_); requests.swap(requests_); logs.swap(audit_); }
            bool build_requested = false;
            for (const auto& [severity, message] : logs) append_audit(paths_, severity == 2 ? "error" : severity == 1 ? "warning" : "info", message);
            for (const auto& request : requests) {
                try {
                    if (request.kind == Request::build) {
                        build_requested = true;
                        if (!compiler) {
                            {
                                std::lock_guard lock(mutex_); status_.detecting = true;
                                for (auto& [key, view] : views_) if ((request.key.empty() || request.key == key) && !view.source.empty())
                                    view.progress = {BuildStage::detecting_toolchain, "Looking for a C++ compiler..."};
                            }
                            auto retry = ScriptToolchain::detect(compiler_, stopping_, error);
                            { std::lock_guard lock(mutex_); status_.detecting = false; status_.error = error; }
                            if (retry) {
                                identity = retry->identity();
                                { std::lock_guard lock(mutex_); status_.error.clear(); status_.toolchain = utf8(retry->compiler); status_.version = retry->version; status_.license = retry->license; status_.setup_url = retry->setup_url; }
                                compiler.emplace(paths_, std::move(*retry));
                            }
                        }
                        for (auto& [key, watch] : watched) if (request.key.empty() || request.key == key) { watch.built.clear(); watch.forced = request.force; watch.changed = {}; }
                    } else if(request.kind==Request::create) {
                        if(!std::regex_match(request.key,std::regex("[A-Za-z][A-Za-z0-9_-]{0,47}"))) throw std::runtime_error("Script name must start with a letter and contain up to 48 letters, digits, underscores or hyphens");
                        const std::set<std::string> templates{"HelloWorld","Settings","GlassHUD","GlassQuickPanel","AnimatedToast","Events","MultiFile"};
                        if(!templates.contains(request.hash)) throw std::runtime_error("Unknown script template");
                        // A prefix also avoids reserved Windows device names such as CON.
                        const auto destination=paths_.sources/("Script_"+request.key+(request.hash=="MultiFile"?"":".cpp"));
                        if(!ScriptPaths::contains(paths_.sources,destination)||fs::exists(destination)) throw std::runtime_error("Script already exists or destination is invalid");
                        const auto replace_id=[&](const fs::path& file) {
                            std::ifstream in(file,std::ios::binary); std::string text((std::istreambuf_iterator<char>(in)),{});
                            if(!in.eof()&&!in) throw std::runtime_error("Cannot read script template");
                            return std::regex_replace(text,std::regex("MEDICON_MODULE\\(([^,]+),\\s*\"[^\"]+\",\\s*\"[^\"]+\""),
                                "MEDICON_MODULE($1,\"script_"+request.key+"\",\""+request.key+"\"");
                        };
                        if(request.hash=="MultiFile") {
                            if(!fs::create_directory(destination)) throw std::runtime_error("Cannot create script project");
                            fs::copy(paths_.sdk/"examples"/"MultiFile",destination,fs::copy_options::recursive);
                            atomic_write(destination/"main.cpp",replace_id(destination/"main.cpp"));
                            const auto manifest=destination/"medicon.toml";
                            if(fs::exists(manifest)) {
                                std::ifstream in(manifest,std::ios::binary);const std::string contents((std::istreambuf_iterator<char>(in)),{});
                                in.close(); // Windows replacement requires the source read handle to be closed.
                                atomic_write(manifest,std::regex_replace(contents,std::regex("(^|\n)[ \\t]*id[ \\t]*=[ \\t]*\"[^\"]*\""),"$1id = \"script_"+request.key+"\""));
                            }
                        } else {
                            const auto text=replace_id(paths_.sdk/"examples"/(request.hash+".cpp"));
                            const auto handle=CreateFileW(destination.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
                            if(handle==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create script file (it may already exist)");
                            DWORD written=0; const bool ok=WriteFile(handle,text.data(),static_cast<DWORD>(text.size()),&written,nullptr)&&written==text.size();
                            CloseHandle(handle); if(!ok) { DeleteFileW(destination.c_str()); throw std::runtime_error("Cannot write script template"); }
                        }
                        append_audit(paths_,"create",utf8(destination));
                    } else if (request.kind == Request::revoke) {
                        trust_.revoke(request.key); append_audit(paths_, "revoke", request.key);
                        std::lock_guard lock(mutex_); auto& view = views_[request.key]; view.status = "Approval revoked; module unloaded";
                        view.progress = {};
                        view.approval_required = pending_.contains(request.key); ++view.generation;
                        BuildResult b; b.source.key = request.key; ready_.push_back({b, view.generation, true});
                    } else {
                        BuildResult b; uint64_t generation{};
                        { std::lock_guard lock(mutex_); const auto found = pending_.find(request.key);
                            if (found == pending_.end() || found->second.binary_hash != request.hash || !views_[request.key].approval_required) continue;
                            b = found->second; generation = views_[request.key].generation;
                        }
                        if (!compiler || ScriptDiscovery::fingerprint(b.source, paths_, identity) != b.source.fingerprint || sha256_file(b.binary) != b.binary_hash)
                            throw std::runtime_error("Approval is stale: the source or DLL changed; rebuild before approving");
                        trust_.approve(b); append_audit(paths_, "approve", request.key + " source=" + b.source.fingerprint + " dll=" + b.binary_hash);
                        set_result(b, generation, true);
                    }
                } catch (const std::exception& e) {
                    audit(2, e.what()); std::lock_guard lock(mutex_); status_.error = e.what();
                    for (auto& [key, view] : views_) if ((request.key.empty() || request.key == key) && view.progress.running()) {
                        view.status = "Script operation failed"; view.progress.stage = BuildStage::failed; view.progress.message = e.what();
                    }
                }
            }
            std::vector<ScriptDiagnostic> diagnostics;
            const auto sources = ScriptDiscovery::scan(paths_, diagnostics);
            {
                std::lock_guard lock(mutex_);
                if (!diagnostics.empty()) {
                    auto& view = views_["[discovery]"]; view.key = "[discovery]"; view.name = "Source discovery"; view.status = "Some paths could not be scanned"; view.diagnostics = diagnostics;
                } else views_.erase("[discovery]");
            }
            std::set<std::string> present;
            std::string signature;
            for (auto source : sources) {
                if (stopping_) break;
                present.insert(source.key);
                for (const auto& path : source.sources) signature += utf8(path) + "\n";
                auto& watch = watched[source.key];
                for (const auto& path : watch.dependencies) source.inputs.push_back(path);
                try {
                    source.fingerprint = ScriptDiscovery::fingerprint(source, paths_, identity);
                    auto now = std::chrono::steady_clock::now();
                    if (source.fingerprint != watch.observed) {
                        watch.observed = source.fingerprint; watch.changed = now;
                        std::lock_guard lock(mutex_); auto& view = views_[source.key];
                        view.key = source.key; view.source = source.entry; view.approval_required = false; ++view.generation;
                        view.status = compiler ? "Changes detected; waiting 500 ms" : "Toolchain required";
                        view.progress = compiler ? BuildProgress{BuildStage::queued, "Source saved; waiting 500 ms before building..."} :
                            BuildProgress{BuildStage::failed, "Build blocked: C++ compiler not found. Open Toolchain setup."};
                        view.log.clear(); view.diagnostics.clear();
                        pending_.erase(source.key);
                    }
                    if (!compiler) {
                        std::lock_guard lock(mutex_); auto& view = views_[source.key];
                        view.status = "Build blocked: C++ compiler not found";
                        view.progress = {BuildStage::failed, "Install C++ Build Tools (x64 + Windows SDK), then press Build / Reload to retry."};
                        continue;
                    }
                    if (watch.observed == watch.built || now - watch.changed < std::chrono::milliseconds(500)) continue;
                    uint64_t generation{};
                    { std::lock_guard lock(mutex_); auto& view = views_[source.key]; view.status = "Building in worker process"; generation = view.generation; }
                    append_audit(paths_, "build_start", source.key);
                    const auto result = compiler->build(source, stopping_, std::exchange(watch.forced, false), [&](const BuildProgress& progress) {
                        std::lock_guard lock(mutex_); auto& view = views_[source.key];
                        if (view.generation != generation) return;
                        view.progress = progress;
                        if (!progress.log.empty()) view.log = progress.log;
                        if (progress.running()) view.status = progress.message;
                    });
                    if(result.success) services_.storage->preload(result.manifest.id);
                    watch.built = watch.observed;
                    if (result.success) { watch.dependencies = result.dependencies; watch.observed = watch.built = result.source.fingerprint; }
                    append_audit(paths_, result.success ? "build_success" : "build_failed", source.key + (result.cached ? " (cached)" : ""));
                    set_result(result, generation, result.success && trust_.approved(result));
                } catch (const std::exception& e) {
                    std::lock_guard lock(mutex_); auto& view = views_[source.key]; view.key = source.key; view.source = source.entry;
                    view.status = "Source/dependency error; previous version retained";
                    view.progress = {BuildStage::failed, e.what()}; view.approval_required = false;
                    pending_.erase(source.key);
                    view.diagnostics = {{source.entry, 0, 0, "error", "INPUT", e.what(), "Restore the missing file or fix its path, then save."}};
                }
            }
            if (compiler && signature != database_signature) { compiler->write_compile_database(sources); database_signature = signature; }
            for (auto it = watched.begin(); it != watched.end();) {
                if (present.contains(it->first)) { ++it; continue; }
                // A malformed directory must not accidentally remove a working module.
                const bool scan_failed = std::ranges::any_of(diagnostics, [&](const auto& d) { return utf8(d.file.filename()) == it->first; });
                if (scan_failed) {
                    std::lock_guard lock(mutex_); auto& view = views_[it->first];
                    if (view.progress.stage != BuildStage::failed || view.approval_required) ++view.generation;
                    view.status = "Source discovery failed; previous version retained";
                    view.progress = {BuildStage::failed, "Fix the source discovery diagnostics, then retry Build / Reload."};
                    view.approval_required = false; pending_.erase(it->first);
                    ++it; continue;
                }
                { std::lock_guard lock(mutex_); auto& view = views_[it->first]; ++view.generation;
                    BuildResult b; b.source.key = it->first; ready_.push_back({b, view.generation, true}); view.status = "Source removed"; view.id.clear(); pending_.erase(it->first);
                    view.progress = {}; view.approval_required = false;
                    view.source.clear();
                }
                it = watched.erase(it);
            }
            std::unique_lock lock(mutex_);
            if (requests_.empty() && (build_requested || !present.empty()))
                status_.build_status = present.empty() ? "No buildable C++ scripts found. Save a .cpp in Scripts and retry." : "";
            wake_.wait_for(lock, std::chrono::milliseconds(200), [this] { return stopping_ || !requests_.empty() || !audit_.empty(); });
        }
        std::deque<std::pair<uint32_t, std::string>> logs;
        { std::lock_guard lock(mutex_); logs.swap(audit_); }
        for (const auto& [_, message] : logs) append_audit(paths_, "shutdown", message);
        append_audit(paths_, "stop", "Build worker stopped");
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex_); status_.detecting = false; status_.error = e.what();
        status_.build_status = "Script worker stopped: " + std::string(e.what());
        for (auto& [_, view] : views_) if (view.progress.running()) view.progress = {BuildStage::failed, e.what()};
    }
}

void ScriptManager::remove_active(const std::string& key, bool save) {
    const auto found = active_.find(key); if (found == active_.end()) return;
    if (save && save_) save_();
    if (auto* module = modules_.find(found->second)) {
        if (before_remove_) before_remove_(*module);
        auto previous = modules_.extract(module->id());
        if (auto* adapter = dynamic_cast<ScriptModuleAdapter*>(previous.get())) adapter->drain();
    }
    active_.erase(found);
    std::lock_guard lock(mutex_); views_[key].active = false;
}
void ScriptManager::pump() {
    if(services_.capabilities) services_.features=(services_.features&(MEDICON_FEATURE_CHAT|MEDICON_FEATURE_PLAYER))|services_.capabilities();
    services_.features|=MEDICON_FEATURE_ANIMATION|MEDICON_FEATURE_TIMERS|MEDICON_FEATURE_STORAGE;
    if(services_.frame && !(services_.frame().flags&MEDICON_FRAME_UI_VISIBLE)) services_.keys.fill(false);
    std::deque<Ready> ready;
    { std::lock_guard lock(mutex_); ready.swap(ready_); }
    for (auto& item : ready) {
        { std::lock_guard lock(mutex_); const auto found = views_.find(item.build.source.key);
            if (found == views_.end() || found->second.generation != item.generation) continue;
        }
        if (item.remove) { remove_active(item.build.source.key); continue; }
        std::string message;
        const auto ok = ScriptReloadCoordinator::replace(*this, item.build, message);
        audit(ok ? 0 : 2, item.build.source.key + ": " + message);
        std::lock_guard lock(mutex_); auto& view = views_[item.build.source.key];
        view.status = message; view.active = active_.contains(item.build.source.key); view.approval_required = false;
        view.progress.stage = ok ? BuildStage::complete : BuildStage::failed; view.progress.message = message;
        if (!ok) view.diagnostics.push_back({item.build.source.entry, 0, 0, "error", "LOAD", message, "Review the log; use Build / Reload to retry."});
    }
    for (const auto& [key, id] : active_) if (const auto* adapter = dynamic_cast<const ScriptModuleAdapter*>(modules_.find(id))) {
        std::lock_guard lock(mutex_); auto& view=views_[key]; view.telemetry=adapter->telemetry();
        if(!adapter->healthy()) view.status=adapter->fault();
        for(const auto& d : adapter->runtime_diagnostics()) if(view.diagnostics.size()<256 && std::ranges::none_of(view.diagnostics,[&](const auto& old){return old.code==d.code&&old.message==d.message;})) view.diagnostics.push_back(d);
    }
}
void ScriptManager::render_ui() { for(const auto& [_,id] : active_) if(auto* adapter=dynamic_cast<ScriptModuleAdapter*>(modules_.find(id))) adapter->ui(); }
void ScriptManager::key_event(int key, bool down, bool repeat) {
    for (const auto& [_, id] : active_) if (auto* module = dynamic_cast<ScriptModuleAdapter*>(modules_.find(id))) module->key_event(key, down, repeat);
}
medicon::Module* ScriptManager::module(std::string_view key) noexcept {
    for (const auto& [source, id] : active_) if (source == key) return modules_.find(id);
    return nullptr;
}
void ScriptManager::settings_edited(bool changed, bool editing) {
    settings_dirty_ = settings_dirty_ || changed;
    if (settings_dirty_ && !editing && save_) { save_(); settings_dirty_ = false; }
}

bool ScriptReloadCoordinator::replace(ScriptManager& m, const BuildResult& b, std::string& status) {
    std::unique_ptr<medicon::Module> previous;
    bool was_enabled = false;
    try {
        const auto active = m.active_.find(b.source.key);
        auto* old = active != m.active_.end() ? m.modules_.find(active->second) : nullptr;
        if (const auto* collision = m.modules_.find(b.manifest.id); collision && collision != old)
            throw std::runtime_error("Duplicate module ID: " + std::string(b.manifest.id));
        {
            std::lock_guard lock(m.mutex_);
            for (const auto& [key, view] : m.views_) if (key != b.source.key && view.id == b.manifest.id)
                throw std::runtime_error("Duplicate script ID in " + key + ": " + b.manifest.id);
        }
        auto library = ScriptPluginLoader::load(b, m.trust_, m.services_.features);
        auto candidate = std::make_unique<ScriptModuleAdapter>(b, std::move(library), m.services_);
        const auto id = candidate->id();
        auto next_active = m.active_; next_active[b.source.key] = id;
        bool enable = false;
        if (old && old->id() == candidate->id()) { ScriptModuleAdapter::transfer_settings(*old, *candidate); enable = old->enabled(); }
        else { std::string error; if (!m.config_.load_module(*candidate, error, &enable)) throw std::runtime_error("Script config: " + error); }
        m.modules_.all().reserve(m.modules_.all().size() + 1);
        if (old) {
            was_enabled = old->enabled();
            if (m.before_remove_) m.before_remove_(*old);
            previous = m.modules_.extract(old->id());
        }
        auto* installed = candidate.get(); m.modules_.adopt(std::move(candidate)); installed->set_enabled(enable);
        if (!installed->healthy()) {
            auto failed = m.modules_.extract(id); failed.reset();
            status = "Script enable failed; no previous DLL was active";
            if (previous) {
                auto* rollback = previous.get(); m.modules_.adopt(std::move(previous)); rollback->set_enabled(was_enabled);
                status = rollback->enabled() == was_enabled ? "Reload failed; previous DLL restored" : "Reload failed; previous DLL restored but could not be enabled";
            }
            return false;
        }
        m.active_.swap(next_active);
        previous.reset(); // all callbacks have returned; scoped listeners already disconnected.
        status = b.cached ? "Loaded from verified cache" : "Loaded successfully";
        try { if (m.save_) m.save_(); } catch (const std::exception& e) { status += std::string("; settings save failed: ") + e.what(); }
        return true;
    } catch (const std::exception& e) {
        if (previous) { auto* rollback = previous.get(); m.modules_.adopt(std::move(previous)); rollback->set_enabled(was_enabled); }
        status = std::string(e.what()) + "; previous version retained";
        return false;
    }
}
} // namespace medicon::scripts
