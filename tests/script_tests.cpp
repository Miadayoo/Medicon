#include <medicon/scripts/ScriptManager.hpp>
#include "ScriptProcess.hpp"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <future>
#include <iostream>

namespace {
using namespace medicon::scripts;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void write(const fs::path& path, std::string_view text) { std::ofstream file(path, std::ios::binary); file << text; if (!file) throw std::runtime_error("Test write failed"); }
struct Temp {
    fs::path root = fs::temp_directory_path() / (L"Medicon Script テスト " + std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(GetTickCount64()));
    Temp() { fs::create_directories(root); }
    ~Temp() { std::error_code ignored; fs::remove_all(root, ignored); }
};
template<class Predicate> ScriptStatus wait_for(ScriptManager& m, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(40);
    while (std::chrono::steady_clock::now() < deadline) {
        m.pump(); auto status = m.snapshot();
        if (predicate(status)) return status;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const auto s = m.snapshot(); std::cerr << "Manager: " << s.error << '\n';
    for (const auto& v : s.scripts) { std::cerr << v.key << ": " << v.status << '\n'; for (const auto& d : v.diagnostics) std::cerr << d.message << '\n'; }
    throw std::runtime_error("Timed out waiting for script state");
}
const ScriptView& view(const ScriptStatus& s, std::string_view key) {
    const auto found = std::ranges::find(s.scripts, key, &ScriptView::key);
    if (found == s.scripts.end()) throw std::runtime_error("Script view missing"); return *found;
}
std::string fixture(std::string_view version, bool fail = false) {
    return R"cpp(#include <medicon.hpp>
#include <windows.h>
using namespace medicon;
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        wchar_t marker[32768]{}; GetEnvironmentVariableW(L"MEDICON_SCRIPT_TEST_MARKER", marker, 32768);
        if (marker[0]) { auto h = CreateFileW(marker, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr); if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    }
    return TRUE;
}
class Fixture : public Module {
    Setting<float> radius = setting<float>("radius", "Radius", 4).range(1,20,.5F);
    Setting<bool> fail_tick = setting<bool>("fail_tick", "Fail tick", false);
    void on_tick() override { if (fail_tick.get()) throw std::runtime_error("tick failure"); }
    void on_enable() override {
)cpp" + (fail ? std::string("throw std::runtime_error(\"enable failure\");\n") :
        std::string("chat::print(\"") + std::string(version) + ":\" + std::to_string(radius.get()));\n") + R"cpp(
        listen<TickEvent>([](const TickEvent&) { log::info("tick"); });
    }
    void on_render(RenderContext& context) override { context.fill_circle(context.center(), radius.get()); }
};
MEDICON_MODULE(Fixture,"fixture","Fixture Script",Category::Utility);
)cpp";
}
void services_tests(const fs::path& root) {
    ScriptPaths paths(root); paths.initialize();
    check(fs::exists(paths.sdk / "include" / "medicon.hpp"), "SDK installed from embedded bytes");
    check(!ScriptPaths::contains(paths.sources, paths.sources / ".." / "escape.cpp"), "Traversal refused");
    check(!ScriptPaths::contains(paths.sources, fs::path(paths.sources.wstring() + L"-sibling") / "test.cpp"), "Sibling prefix refused");
    write(paths.sources / "A.cpp", "// source"); write(paths.sources / "helper.hpp", "// header");
    fs::create_directories(paths.sources / "Multi");
    write(paths.sources / "Multi" / "main.cpp", "// main"); write(paths.sources / "Multi" / "helper.cpp", "// helper");
    write(paths.sources / "Multi" / "medicon.toml", "[module]\nid = \"multi\"\nrequired_features = [\"render\"]\n");
    std::vector<ScriptDiagnostic> errors; const auto sources = ScriptDiscovery::scan(paths, errors);
    check(errors.empty() && sources.size() == 2 && sources[0].sources.size() == 1 && sources[1].sources.size() == 2, "Root and directory discovery");
    check(sources[1].required_features == MEDICON_FEATURE_RENDER, "Manifest feature requirements");
    const auto d = parse_diagnostics("C:\\test dir\\a.cpp(12,4): error C2039: unknown API\nC:/a.cpp:8:3: error: no such member\n");
    check(d.size() == 2 && d[0].line == 12 && d[0].column == 4 && d[0].code == "C2039" && d[1].line == 8, "MSVC/clang diagnostics");
    check(sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA256 exact digest");
    std::atomic_bool stop = false; std::string error;
    auto t = ScriptToolchain::detect(path_from_utf8(MEDICON_TEST_CXX), stop, error); check(t.has_value(), error.c_str());
    ScriptCompiler compiler(paths, *t);
    // Compile every published sample through the production compiler service.
    for (const auto& item : fs::directory_iterator(paths.sdk / "examples")) {
        if (item.is_directory()) fs::copy(item.path(), paths.sources / item.path().filename(), fs::copy_options::recursive);
        else if (item.path().extension() == ".cpp") fs::copy_file(item.path(), paths.sources / item.path().filename());
    }
    errors.clear(); auto all = ScriptDiscovery::scan(paths, errors);
    std::vector<ScriptSource> samples;
    for (const auto& s : all) if (s.key != "A.cpp" && s.key != "Multi") samples.push_back(s);
    check(samples.size() == 12, "All twelve published sample modules discovered");
    compiler.write_compile_database(samples);
    for (const auto& sample : samples) {
        std::vector<BuildProgress> progress;
        const auto built = compiler.build(sample, stop, false, [&](const auto& p) {
            progress.push_back(p);
            if (p.stage == BuildStage::compiling) check(fs::is_regular_file(p.log), "Build log is available while compilation is running");
        });
        if (!built.success) for (const auto& diagnostic : built.diagnostics) std::cerr << diagnostic.message << '\n';
        check(built.success, "Published sample compiles and passes offline inspection");
        check(progress.front().stage == BuildStage::preparing && progress.back().stage == BuildStage::complete &&
            progress.back().completed_steps == progress.back().total_steps, "Build progress starts at preparation and finishes with all steps complete");
        check(std::ranges::any_of(progress, [](const auto& p) { return p.stage == BuildStage::compiling; }) &&
            std::ranges::any_of(progress, [](const auto& p) { return p.stage == BuildStage::linking; }) &&
            std::ranges::any_of(progress, [](const auto& p) { return p.stage == BuildStage::verifying; }), "Real compile, link and verification stages are reported");
        if (sample.sources.size() > 1) check(std::ranges::any_of(progress, [](const auto& p) {
            return p.message.starts_with("Compiling 2/2:") && p.total_steps == 6;
        }), "Multi-file progress includes the current file and file count");
        progress.clear();
        const auto cached = compiler.build(sample, stop, false, [&](const auto& p) { progress.push_back(p); });
        check(cached.success && cached.cached && cached.binary_hash == built.binary_hash, "Verified incremental cache");
        check(progress.back().stage == BuildStage::complete && std::ranges::none_of(progress, [](const auto& p) {
            return p.stage == BuildStage::compiling;
        }), "Cache hits finish progress without claiming to compile sources");
    }
    const auto database = nlohmann::json::parse(std::ifstream(paths.sdk / "compile_commands.json"));
    check(database.size() == samples.size()+1 && database[0].contains("arguments"), "Compile database includes both MultiFile units");
    // Compiling/linking succeeds without MEDICON_MODULE; inspection must explain
    // the missing registration and preserve that reason in Build log.
    BuildProgress failed_progress;
    const auto missing=compiler.build(sources.front(),stop,false,[&](const auto& p){failed_progress=p;});
    check(failed_progress.stage==BuildStage::failed && failed_progress.completed_steps<failed_progress.total_steps &&
        failed_progress.message.find("inspecting DLL")!=std::string::npos, "Failed progress identifies the stage and never claims completion");
    check(!missing.success&&std::ranges::any_of(missing.diagnostics,[](const auto& d){
        return d.code=="SCRIPT_REGISTRATION"&&d.message.find("MEDICON_MODULE")!=std::string::npos&&d.hint.find("HelloWorld.cpp")!=std::string::npos;
    }),"Missing registration has an actionable script diagnostic");
    const auto missing_log=[&]{std::ifstream in(missing.log,std::ios::binary);return std::string(std::istreambuf_iterator<char>(in),{});}();
    check(missing_log.find("[Medicon / inspecting DLL] SCRIPT_REGISTRATION")!=std::string::npos,"Build log includes post-link inspection failures");
    check(missing_log.find(utf8(t->compiler))!=std::string::npos&&missing_log.find(sha256_file(paths.sdk/"include"/"medicon.hpp"))!=std::string::npos,
        "Build log identifies the actual compiler and installed SDK header");
    check(missing_log.find("Source: "+utf8(sources.front().entry))!=std::string::npos&&missing_log.find("MEDICON_MODULE text: absent")!=std::string::npos&&
        missing_log.find("Command arguments:")!=std::string::npos,"Build log identifies the saved source and exact compiler arguments");
    // The exact Hello.cpp reported with the PE RVA failure is valid SDK code.
    write(paths.sources/"A.cpp",R"cpp(#include <medicon.hpp>
using namespace medicon;

class Hello : public Module {
    void on_enable() override { chat::print("Hello, Medicon!"); }
};
MEDICON_MODULE(Hello, "my_hello", "My Hello", Category::Utility);
)cpp");
    const auto registered_hello=compiler.build(sources.front(),stop);
    check(registered_hello.success&&std::string(registered_hello.manifest.id)=="my_hello", "Reported Hello.cpp builds through the production compiler and DLL inspector");
    const auto clangd = t->compiler.parent_path() / "clangd.exe";
    if (fs::is_regular_file(clangd)) {
        const auto completion = detail::run(clangd, {L"--check=" + (paths.sources / "HelloWorld.cpp").wstring(),
            L"--compile-commands-dir=" + paths.sdk.wstring(), L"--query-driver=" + t->compiler.wstring(), L"--log=error"}, root, stop, 30, t->environment);
        if (completion.exit_code) std::cerr << completion.output;
        check(completion.exit_code == 0 && !completion.limited, "clangd consumes the generated compile database and public SDK");
    }
    write(root / "external.hpp", "inline constexpr int external_value = 1;\n");
    write(paths.sources / "Dependent.cpp", "#include <medicon.hpp>\n#include \"../external.hpp\"\nclass Dependent : public medicon::Module {};\nMEDICON_MODULE(Dependent,\"dependent\",\"Dependent\",medicon::Category::Utility);\n");
    errors.clear(); all = ScriptDiscovery::scan(paths, errors);
    const auto dependency_source = std::ranges::find(all, std::string("Dependent.cpp"), &ScriptSource::key);
    auto dependency_build = compiler.build(*dependency_source, stop);
    check(dependency_build.success && !dependency_build.dependencies.empty(), "External include dependencies tracked and verified");
    write(root / "external.hpp", "inline constexpr int external_value = 2;\n");
    const auto changed_dependency = compiler.build(*dependency_source, stop);
    check(changed_dependency.success && !changed_dependency.cached && changed_dependency.source.fingerprint != dependency_build.source.fingerprint, "External header change invalidates incremental cache and approval");
    wchar_t self[32768]{}; GetModuleFileNameW(nullptr, self, 32768);
    const auto timeout = detail::run(fs::path(self), {L"--sleep"}, root, stop, 1);
    check(timeout.limited, "Compiler process timeout cancels Job Object");
    const auto batch = root / L"environment テスト.bat"; write(batch, "@set MEDICON_ENV_TEST=ok\r\n");
    wchar_t system[MAX_PATH]{}; GetSystemDirectoryW(system, MAX_PATH);
    const auto env = detail::run(fs::path(system) / "cmd.exe", {L"/d",L"/s",L"/u",L"/c"}, root, stop, 5, {}, L"\"call \"" + batch.wstring() + L"\" >nul && set\"");
    std::wstring environment(env.output.size() / sizeof(wchar_t), 0); std::memcpy(environment.data(), env.output.data(), environment.size() * sizeof(wchar_t));
    check(env.exit_code == 0 && environment.find(L"MEDICON_ENV_TEST=ok") != std::wstring::npos, "Unicode vcvars batch command quoting");
}

void runtime_tests(const fs::path& root) {
    fs::create_directories(root / "Scripts"); write(root / "Scripts" / "Fixture.cpp", fixture("v1"));
    const auto marker = root / "dllmain-marker"; SetEnvironmentVariableW(L"MEDICON_SCRIPT_TEST_MARKER", marker.c_str());
    struct Reset { ~Reset() { SetEnvironmentVariableW(L"MEDICON_SCRIPT_TEST_MARKER", nullptr); } } reset;
    medicon::ModuleManager modules; medicon::ConfigStore config(root / "config.json");
    ScriptManager manager(modules, config.path(), path_from_utf8(MEDICON_TEST_CXX));
    std::vector<std::string> messages;
    manager.host().features = MEDICON_FEATURE_CHAT | MEDICON_FEATURE_RENDER;
    manager.host().chat = [&](std::string_view s) { messages.emplace_back(s); return true; };
    manager.host().render_size = [] { return MediconPoint{1920,1080}; };
    manager.set_save_handler([&] { std::string error; check(config.save(modules, error), error.c_str()); });
    manager.start();
    auto status = wait_for(manager, [](const auto& s) { return !s.scripts.empty() && s.scripts.front().approval_required; });
    check(!fs::exists(marker) && !modules.find("fixture"), "Unapproved DLL never runs DllMain");
    check(view(status,"Fixture.cpp").progress.stage==BuildStage::complete, "Automatic builds publish completed progress before approval");
    const auto first_log=view(status,"Fixture.cpp").log;
    manager.request_build("Fixture.cpp",true);
    const auto queued=manager.snapshot();
    check(view(queued,"Fixture.cpp").progress.running() && !view(queued,"Fixture.cpp").approval_required, "Build click immediately shows progress and invalidates old approval");
    manager.request_build("Fixture.cpp",true);
    check(view(manager.snapshot(),"Fixture.cpp").generation==view(queued,"Fixture.cpp").generation,"Repeated build clicks do not queue duplicate builds");
    status=wait_for(manager,[](const auto& s){return view(s,"Fixture.cpp").approval_required;});
    check(view(status,"Fixture.cpp").log!=first_log && view(status,"Fixture.cpp").progress.stage==BuildStage::complete,
        "Rebuild bypasses cache and completes progress with a fresh build log");
    manager.request_approval("Fixture.cpp", view(status, "Fixture.cpp").binary_hash);
    check(view(manager.snapshot(),"Fixture.cpp").progress.running(),"Approve & Load immediately shows loading progress");
    status = wait_for(manager, [&](const auto&) { return modules.find("fixture") != nullptr; });
    auto* first = modules.find("fixture");
    check(fs::exists(marker) && first->category() == medicon::Category::Scripts && first->name() == "Fixture Script", "Approved module appears in Scripts with its name");
    check(view(status,"Fixture.cpp").progress.stage==BuildStage::complete,"DLL loading finishes progress on the frame thread");
    first->settings()[0].value = 7.F; first->set_keybind(75); first->set_enabled(true);
    check(first->enabled() && messages.back().starts_with("v1:7"), "Setting restored before enable and chat bridged");
    std::string error; check(config.save(modules, error), "Save script settings");

    // Offline ABI mismatch, even with an exact approval, must not execute DllMain.
    std::atomic_bool stop = false; auto tc = ScriptToolchain::detect(path_from_utf8(MEDICON_TEST_CXX), stop, error);
    ScriptPaths paths(root); std::vector<ScriptDiagnostic> errors; auto sources = ScriptDiscovery::scan(paths, errors);
    ScriptCompiler compiler(paths, *tc); auto build = compiler.build(sources.front(), stop);
    auto malformed = build; malformed.binary = root / "wrong-abi.dll";
    std::ifstream binary(build.binary, std::ios::binary); std::string bytes{std::istreambuf_iterator<char>(binary), {}};
    const auto manifest_bytes = std::string(reinterpret_cast<const char*>(&build.manifest), sizeof(build.manifest));
    const auto at = bytes.find(manifest_bytes); check(at != std::string::npos, "Manifest is file-readable");
    uint32_t wrong = 900; std::memcpy(bytes.data() + at + offsetof(MediconScriptManifest, abi_version), &wrong, sizeof(wrong));
    write(malformed.binary, bytes); malformed.binary_hash = sha256_file(malformed.binary);
    ScriptTrustManager separate(root / "test-trust.json"); separate.approve(malformed); fs::remove(marker);
    bool refused = false;
    try { auto dll = ScriptPluginLoader::load(malformed, separate, manager.host().features); } catch (const std::exception&) { refused = true; }
    check(refused && !fs::exists(marker), "ABI mismatch rejected before LoadLibrary");
    auto required = build; required.source.required_features = MEDICON_FEATURE_PLAYER; separate.approve(required); refused = false;
    try { auto dll = ScriptPluginLoader::load(required, separate, manager.host().features); } catch (const std::exception&) { refused = true; }
    check(refused && !fs::exists(marker), "Missing required MCBE feature rejected before DllMain");
    auto tampered = build; tampered.binary = root / "tampered.dll"; fs::copy_file(build.binary, tampered.binary); separate.approve(tampered);
    { std::ofstream changed(tampered.binary, std::ios::binary | std::ios::app); changed << "changed"; } refused = false;
    try { auto dll = ScriptPluginLoader::load(tampered, separate, manager.host().features); } catch (const std::exception&) { refused = true; }
    check(refused && !fs::exists(marker), "Changed DLL hash refused before DllMain");

    write(root / "Scripts" / "Fixture.cpp", "#include <medicon.hpp>\nvoid error() { medicon::chat::nonexistent(); }\n");
    status = wait_for(manager, [](const auto& s) { return !s.scripts.empty() && s.scripts.front().status.starts_with("Build failed"); });
    check(modules.find("fixture") == first && first->enabled(), "Compilation failure retains active version");
    check(view(status,"Fixture.cpp").progress.stage==BuildStage::failed && !view(status,"Fixture.cpp").progress.running(),"Compiler failure ends the progress indicator");
    check(std::ranges::any_of(view(status, "Fixture.cpp").diagnostics, [](const auto& d) { return d.line > 0 && d.column > 0; }), "Real compiler errors include line and column");
    write(root / "Scripts" / "Fixture.cpp", fixture("v2"));
    status = wait_for(manager, [](const auto& s) { return !s.scripts.empty() && s.scripts.front().approval_required; });
    check(modules.find("fixture") == first, "Changed code waits for reapproval while old code runs");
    manager.request_approval("Fixture.cpp", view(status, "Fixture.cpp").binary_hash);
    wait_for(manager, [&](const auto&) { return modules.find("fixture") != first; });
    auto* second = modules.find("fixture");
    check(second && second->enabled() && second->keybind() == 75 && second->settings()[0].get<float>() == 7 && messages.back().starts_with("v2:7"), "Reload preserves module settings, bind and enabled state");
    sources = ScriptDiscovery::scan(paths, errors); auto working = compiler.build(sources.front(), stop);
    write(root / "Scripts" / "Fixture.cpp", fixture("bad", true));
    status = wait_for(manager, [](const auto& s) { return !s.scripts.empty() && s.scripts.front().approval_required; });
    manager.request_approval("Fixture.cpp", view(status, "Fixture.cpp").binary_hash);
    wait_for(manager, [](const auto& s) { return !s.scripts.empty() && s.scripts.front().status.find("previous DLL restored") != std::string::npos; });
    check(modules.find("fixture") == second && second->enabled(), "Failed enable rolls back to the old DLL");
    class Counter final : public medicon::Module {
    public:
        int ticks = 0;
        Counter() : medicon::Module("counter", "Counter", "", medicon::Category::Misc) { set_enabled(true); }
        void on_tick(float) noexcept override { ++ticks; }
    };
    auto& counter = modules.add<Counter>();
    second->settings()[1].value = true; modules.tick(.05F);
    check(!second->enabled() && counter.ticks == 1, "Script exception disables only the failing module");
    second->settings()[1].value = false; second->set_enabled(true);
    manager.stop();
    check(!modules.find("fixture"), "Shutdown unregisters and unloads scripts");
    check(config.save(modules, error), "Save with unloaded scripts");
    const auto persisted = nlohmann::json::parse(std::ifstream(config.path()));
    check(persisted["modules"]["fixture"]["settings"]["radius"] == 7 && persisted["modules"]["fixture"]["enabled"] == true, "Unloaded settings and enabled state survive saves/restarts");
    medicon::ModuleManager restarted;
    separate.approve(working);
    auto restored = std::make_unique<ScriptModuleAdapter>(working, ScriptPluginLoader::load(working, separate, manager.host().features), manager.host());
    auto* adapter = restored.get(); restarted.adopt(std::move(restored));
    check(config.load_module(*adapter, error) && adapter->enabled() && adapter->keybind() == 75 && adapter->settings()[0].get<float>() == 7, "New process module restores persisted settings before enabling");

    // A callback holds the drain lock until its host rendering call has returned.
    std::mutex mutex; std::condition_variable cv; bool entered = false, release = false;
    manager.host().circle = [&](MediconPoint, float, MediconColor) { std::unique_lock lock(mutex); entered = true; cv.notify_all(); cv.wait(lock, [&] { return release; }); };
    auto rendering = std::async(std::launch::async, [&] { restarted.render(); });
    { std::unique_lock lock(mutex); cv.wait(lock, [&] { return entered; }); }
    auto draining = std::async(std::launch::async, [&] { adapter->drain(); });
    const bool waited = draining.wait_for(std::chrono::milliseconds(80)) == std::future_status::timeout;
    { std::lock_guard lock(mutex); release = true; } cv.notify_all(); rendering.get(); draining.get();
    check(waited, "Drain waits for in-flight rendering");
}
void template_tests(const fs::path& root) {
    medicon::ModuleManager modules;ScriptManager manager(modules,root/"config.json",path_from_utf8(MEDICON_TEST_CXX));
    manager.start();manager.request_create("Mine","GlassHUD");manager.request_create("Project","MultiFile");
    auto status=wait_for(manager,[](const auto& s){return s.scripts.size()==2&&std::ranges::all_of(s.scripts,&ScriptView::approval_required);});
    check(fs::is_regular_file(root/"Scripts"/"Script_Mine.cpp")&&fs::is_regular_file(root/"Scripts"/"Script_Project"/"helpers.cpp"),"Worker creates single-file and multi-file templates");
    check(view(status,"Script_Mine.cpp").id=="script_Mine"&&view(status,"Script_Project").id=="script_Project","Template IDs follow the new script name");
    const auto before=sha256_file(root/"Scripts"/"Script_Mine.cpp");
    manager.request_create("Mine","HelloWorld");wait_for(manager,[](const auto& s){return s.error.find("already exists")!=std::string::npos;});
    check(sha256_file(root/"Scripts"/"Script_Mine.cpp")==before,"Create never overwrites an existing script");
    manager.request_create("../escape","HelloWorld");wait_for(manager,[](const auto& s){return s.error.find("must start")!=std::string::npos;});
    check(!fs::exists(root/"escape.cpp"),"Template name cannot traverse out of Scripts");manager.stop();
}
void missing_toolchain_tests(const fs::path& root) {
    fs::create_directories(root/"Scripts"); write(root/"Scripts"/"Hello.cpp",fixture("hello"));
    wchar_t previous[32768]{}; const auto count=GetEnvironmentVariableW(L"MEDICON_SCRIPT_CXX",previous,32768);
    check(count<32768,"Compiler override fits the test environment buffer");
    struct RestoreCompiler { std::wstring previous; ~RestoreCompiler(){SetEnvironmentVariableW(L"MEDICON_SCRIPT_CXX",previous.empty()?nullptr:previous.c_str());} } restore{previous};
    SetEnvironmentVariableW(L"MEDICON_SCRIPT_CXX",(root/"missing-compiler.exe").c_str());
    medicon::ModuleManager modules; ScriptManager manager(modules,root/"config.json");
    manager.request_build();
    check(manager.snapshot().build_status.find("queued")!=std::string::npos,"Build all acknowledges requests before discovery has run");
    manager.start();
    auto status=wait_for(manager,[](const auto& s){return !s.detecting&&!s.scripts.empty()&&s.scripts.front().progress.stage==BuildStage::failed;});
    check(!status.error.empty()&&view(status,"Hello.cpp").status.find("C++ compiler not found")!=std::string::npos,"Missing compiler explains why the script was not built");
    const auto generation=view(status,"Hello.cpp").generation;
    manager.request_build("Hello.cpp");
    status=wait_for(manager,[&](const auto& s){const auto& v=view(s,"Hello.cpp");return !s.detecting&&v.generation>generation&&v.progress.stage==BuildStage::failed;});
    check(!view(status,"Hello.cpp").progress.running()&&!view(status,"Hello.cpp").active,"Failed toolchain retry ends progress without loading a DLL");
    SetEnvironmentVariableW(L"MEDICON_SCRIPT_CXX",path_from_utf8(MEDICON_TEST_CXX).c_str());
    manager.request_build("Hello.cpp");
    status=wait_for(manager,[](const auto& s){return view(s,"Hello.cpp").approval_required;});
    check(status.error.empty()&&!status.detecting&&!status.toolchain.empty()&&view(status,"Hello.cpp").progress.stage==BuildStage::complete,
        "Retry detects an installed compiler, clears the setup error and finishes the build");
    write(root/"Scripts"/"Hello.cpp",std::string(8*1024*1024+1,' ')); manager.request_build("Hello.cpp");
    status=wait_for(manager,[](const auto& s){return view(s,"Hello.cpp").status.starts_with("Source discovery failed");});
    check(!view(status,"Hello.cpp").progress.running()&&!view(status,"Hello.cpp").approval_required,"Discovery errors end queued progress and invalidate stale approval");
    fs::remove(root/"Scripts"/"Hello.cpp");
    wait_for(manager,[](const auto& s){return view(s,"Hello.cpp").status=="Source removed";});
    manager.request_build();
    status=wait_for(manager,[](const auto& s){return s.build_status.find("No buildable C++ scripts")!=std::string::npos;});
    check(!view(status,"Hello.cpp").progress.running(),"Build all does not leave deleted scripts queued forever");
    manager.stop();
    fs::create_directories(root/"empty");
    ScriptManager empty(modules,root/"empty"/"config.json",root/"missing-compiler.exe"); empty.request_build(); empty.start();
    wait_for(empty,[](const auto& s){return s.build_status.find("No buildable C++ scripts")!=std::string::npos;});
    write(empty.paths().sources/"Later.cpp",fixture("later"));
    wait_for(empty,[](const auto& s){return !s.scripts.empty()&&s.build_status.empty();}); empty.stop();
}
void native_toolchain_test(const fs::path& root) {
    wchar_t previous_link[32768]{}; const auto count=GetEnvironmentVariableW(L"_LINK_",previous_link,32768);
    check(count<32768,"Linker override fits the test environment buffer");
    struct RestoreLink {std::wstring value;~RestoreLink(){SetEnvironmentVariableW(L"_LINK_",value.empty()?nullptr:value.c_str());}} restore_link{previous_link};
    // This would make an otherwise valid build fail if inherited linker switches leaked in.
    SetEnvironmentVariableW(L"_LINK_",L"/INCLUDE:medicon_test_unwanted_environment_symbol");
    std::atomic_bool stop=false; std::string error;
    const auto toolchain=ScriptToolchain::detect({},stop,error); check(toolchain.has_value(),error.c_str());
    std::cout<<"Automatically detected compiler: "<<utf8(toolchain->compiler)<<std::endl;
    ScriptPaths paths(root); paths.initialize();
    write(paths.sources/"Hello.cpp",R"cpp(#include <medicon.hpp>
using namespace medicon;
class Hello : public Module {
    void on_enable() override { chat::print("Hello, Medicon!"); }
};
MEDICON_MODULE(Hello, "my_hello", "My Hello", Category::Utility);
)cpp");
    std::vector<ScriptDiagnostic> diagnostics; const auto sources=ScriptDiscovery::scan(paths,diagnostics);
    check(diagnostics.empty()&&sources.size()==1,"Native smoke test source is discoverable");
    ScriptCompiler compiler(paths,*toolchain);
    const auto result=compiler.build(sources.front(),stop,true,[](const auto& p){std::cout<<p.message<<std::endl;});
    if(!result.success) for(const auto& d:result.diagnostics) std::cerr<<d.message<<'\n';
    check(result.success&&std::string(result.manifest.id)=="my_hello","User's Hello module builds and passes DLL inspection with the automatically detected compiler");
    if(toolchain->kind==ToolchainKind::msvc) {
        // Reproduce a DLL with the ABI definitions but no compiler-generated export directives.
        write(paths.sources/"Hello.cpp",R"cpp(#include <medicon.hpp>
#undef MEDICON_SCRIPT_EXPORT
#define MEDICON_SCRIPT_EXPORT
class Hello : public medicon::Module {};
MEDICON_MODULE(Hello, "my_hello", "My Hello", medicon::Category::Utility);
)cpp");
        const auto recovered=compiler.build(sources.front(),stop,true,[](const auto& p){std::cout<<p.message<<std::endl;});
        check(recovered.success&&std::string(recovered.manifest.id)=="my_hello","Explicit MSVC exports recover a registered object without export directives");
        const auto log=[&]{std::ifstream in(recovered.log,std::ios::binary);return std::string(std::istreambuf_iterator<char>(in),{});}();
        check(log.find("DLL has no exports")!=std::string::npos&&log.find("Explicit script exports verified successfully")!=std::string::npos,
            "MSVC recovery reproduces and fixes the reported no-exports condition");
        write(paths.sources/"Hello.cpp","#include <medicon.hpp>\nclass Hello : public medicon::Module {};\n");
        const auto missing=compiler.build(sources.front(),stop,true);
        check(!missing.success&&std::ranges::any_of(missing.diagnostics,[](const auto& d){return d.code=="SCRIPT_REGISTRATION";}),
            "MSVC export recovery still rejects a module without registration");
        std::cout<<"MSVC missing-export recovery and registration rejection passed"<<std::endl;
    }
}
}
int main(int argc, char** argv) {
    const bool native=argc>1&&std::string_view(argv[1])=="--native-toolchain";
    if (argc > 1&&!native) { Sleep(10000); return 0; }
    try { Temp temp; if(native) {native_toolchain_test(temp.root/"native");return 0;} services_tests(temp.root / "services"); runtime_tests(temp.root / "runtime");template_tests(temp.root/"templates");missing_toolchain_tests(temp.root/"missing"); std::cout << "Script integration tests passed\n"; return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
