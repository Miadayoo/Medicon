#include <medicon/scripts/ScriptServices.hpp>
#include "ScriptProcess.hpp"
#include "ScriptInspectionError.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>
#include <set>
#include <cstring>

namespace medicon::scripts {
namespace {
using Json = nlohmann::json;
std::string read_text(const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream || fs::file_size(file) > 16 * 1024 * 1024) throw std::runtime_error("Cannot read compiler metadata");
    return {std::istreambuf_iterator<char>(stream), {}};
}
std::vector<std::wstring> flags(const ScriptToolchain& t, const ScriptPaths& p, const ScriptSource& s) {
    if (t.kind == ToolchainKind::msvc) {
        std::vector<std::wstring> result{L"/nologo", L"/std:c++20", L"/MT", L"/EHsc", L"/utf-8", L"/W4", L"/permissive-", L"/diagnostics:column", L"/FC", L"/I" + (p.sdk / "include").wstring(), L"/I" + s.directory.wstring()};
        for (size_t i = 0; i < t.environment.size() && t.environment[i];) {
            const std::wstring entry(t.environment.data() + i); i += entry.size() + 1;
            if (entry.size() < 8 || _wcsnicmp(entry.c_str(), L"INCLUDE=", 8) != 0) continue;
            size_t start = 8;
            while (start < entry.size()) { const auto end = entry.find(L';', start); const auto part = entry.substr(start, end - start);
                if (!part.empty()) result.push_back(L"/I" + part); if (end == std::wstring::npos) break; start = end + 1; }
        }
        return result;
    }
    return {L"-std=c++20", L"-O2", L"-Wall", L"-Wextra", L"-fno-color-diagnostics", L"-static", L"-I" + (p.sdk / "include").wstring(), L"-I" + s.directory.wstring()};
}
std::string compiler_output(std::string text, ToolchainKind kind) {
    if (kind != ToolchainKind::msvc || text.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0)) return text;
    const int count = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(count, 0); MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), wide.data(), count);
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), count, nullptr, 0, nullptr, nullptr);
    std::string result(bytes, 0); WideCharToMultiByte(CP_UTF8, 0, wide.data(), count, result.data(), bytes, nullptr, nullptr); return result;
}
std::vector<wchar_t> clean_environment(const std::vector<wchar_t>& supplied, bool msvc) {
    std::vector<std::wstring> entries;
    auto* inherited = supplied.empty() ? GetEnvironmentStringsW() : nullptr;
    const auto* p = supplied.empty() ? inherited : supplied.data();
    if (!p) throw std::runtime_error("Cannot capture compiler environment");
    for (; *p; p += std::wcslen(p) + 1) {
        const std::wstring entry(p); const auto equals = entry.find(L'=', entry[0] == L'=' ? 1 : 0);
        const auto key = entry.substr(0, equals);
        if (_wcsicmp(key.c_str(), L"CL") && _wcsicmp(key.c_str(), L"_CL_") && _wcsicmp(key.c_str(), L"LINK") &&
            _wcsicmp(key.c_str(), L"_LINK_") && (!msvc || _wcsicmp(key.c_str(), L"VSLANG"))) entries.push_back(entry);
    }
    if (inherited) FreeEnvironmentStringsW(inherited);
    if (msvc) entries.push_back(L"VSLANG=1033");
    std::ranges::sort(entries, [](const auto& a, const auto& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    std::vector<wchar_t> result;
    for (const auto& entry : entries) { result.insert(result.end(), entry.begin(), entry.end()); result.push_back(0); }
    result.push_back(0); return result;
}
Json arguments_json(const fs::path& compiler, const std::vector<std::wstring>& args) {
    Json result = Json::array(); result.push_back(utf8(compiler));
    for (const auto& arg : args) result.push_back(utf8(fs::path(arg)));
    return result;
}
void add_dependency(ScriptSource& source, std::vector<fs::path>& dependencies, const fs::path& p, const ScriptPaths& paths) {
    if (!fs::is_regular_file(p)) return;
    if (ScriptPaths::contains(paths.sdk / "include", p)) return;
    if ((GetFileAttributesW(p.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0) throw std::runtime_error("Linked dependency is not supported: " + utf8(p));
    dependencies.push_back(fs::weakly_canonical(p));
    source.inputs.push_back(fs::weakly_canonical(p));
}
void parse_depfile(const fs::path& path, ScriptSource& source, std::vector<fs::path>& deps, const ScriptPaths& paths) {
    auto text = read_text(path);
    // Make-style escaping: spaces/#/colon/backslash, including Windows drive letters.
    const auto start = text.find(": "); if (start == std::string::npos) return;
    std::string token;
    const auto flush = [&] { if (!token.empty()) { add_dependency(source, deps, path_from_utf8(token), paths); token.clear(); } };
    for (size_t i = start + 2; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\\' && i + 1 < text.size()) {
            const char next = text[i + 1];
            if (next == '\n') { ++i; continue; }
            if (next == '\r' && i + 2 < text.size() && text[i + 2] == '\n') { i += 2; continue; }
            if (next == ' ' || next == '#' || next == '\\' || next == ':') { token += next; ++i; continue; }
        }
        if (std::isspace(static_cast<unsigned char>(c))) flush(); else token += c;
    }
    flush();
}
} // namespace

std::string ScriptToolchain::identity() const {
    std::string result = utf8(compiler) + "\n" + version + "\n" + sha256_file(compiler) + "\nC++20/static-CRT/v1";
    for (size_t i = 0; i < environment.size() && environment[i];) {
        const std::wstring entry(environment.data() + i); i += entry.size() + 1;
        if (_wcsnicmp(entry.c_str(), L"INCLUDE=", 8) == 0 || _wcsnicmp(entry.c_str(), L"LIB=", 4) == 0)
            result += "\n" + utf8(fs::path(entry));
    }
    return result;
}
std::optional<ScriptToolchain> ScriptToolchain::detect(const fs::path& preferred, std::atomic_bool& stop, std::string& diagnostic) {
    std::vector<std::pair<fs::path, fs::path>> candidates;
    if (!preferred.empty()) candidates.emplace_back(fs::absolute(preferred), fs::path{});
    else {
        wchar_t override_path[32768]{};
        const auto count = GetEnvironmentVariableW(L"MEDICON_SCRIPT_CXX", override_path, 32768);
        if (count && count < 32768) candidates.emplace_back(override_path, fs::path{});
        else {
            if (auto cl = detail::search(L"cl.exe"); !cl.empty()) candidates.emplace_back(cl, fs::path{});
            wchar_t program_files[32768]{};
            const auto n = GetEnvironmentVariableW(L"ProgramFiles(x86)", program_files, 32768);
            const auto vswhere = fs::path(program_files) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
            if (n && n < 32768 && fs::is_regular_file(vswhere)) {
                const auto locate = [&](bool prerelease) {
                    std::vector<std::wstring> args{L"-latest", L"-products", L"*", L"-requires", L"Microsoft.VisualStudio.Component.VC.Tools.x86.x64", L"-property", L"installationPath", L"-utf8"};
                    if (prerelease) args.push_back(L"-prerelease");
                    auto path = detail::run(vswhere, args, {}, stop, 10).output;
                    while (!path.empty() && std::isspace(static_cast<unsigned char>(path.back()))) path.pop_back();
                    if (path.starts_with("\xEF\xBB\xBF")) path.erase(0, 3);
                    return path;
                };
                auto path = locate(false);
                // vswhere excludes Visual Studio Insiders / Preview unless explicitly requested.
                if (path.empty() && !stop) path = locate(true);
                if (!path.empty()) {
                    const auto installation = path_from_utf8(path);
                    const auto tools = installation / "VC" / "Tools" / "MSVC";
                    std::vector<fs::path> versions;
                    if (fs::is_directory(tools)) for (const auto& v : fs::directory_iterator(tools)) if (v.is_directory()) versions.push_back(v.path());
                    std::ranges::sort(versions, std::greater<>{});
                    if (!versions.empty()) {
                        auto selected = versions.front();
                        const auto auxiliary = installation / "VC" / "Auxiliary" / "Build";
                        std::ifstream defaults(auxiliary / "Microsoft.VCToolsVersion.default.txt"); std::string version;
                        if (std::getline(defaults, version)) {
                            while (!version.empty() && std::isspace(static_cast<unsigned char>(version.back()))) version.pop_back();
                            const auto candidate = tools / path_from_utf8(version);
                            // Match the headers and libraries selected by vcvars64, even when a newer preview toolset is installed.
                            if (ScriptPaths::contains(tools, candidate) && fs::is_regular_file(candidate / "bin" / "Hostx64" / "x64" / "cl.exe")) selected = candidate;
                        }
                        candidates.emplace_back(selected / "bin" / "Hostx64" / "x64" / "cl.exe", auxiliary / "vcvars64.bat");
                    }
                }
            }
            if (auto clang = detail::search(L"clang++.exe"); !clang.empty()) candidates.emplace_back(clang, fs::path{});
        }
    }
    for (const auto& [compiler, batch] : candidates) {
        if (!fs::is_regular_file(compiler)) continue;
        try {
            ScriptToolchain t; t.compiler = fs::weakly_canonical(compiler); t.environment_batch = batch;
            if (compiler.filename() == L"cl.exe") {
                if (t.environment_batch.empty()) {
                    for (auto parent = t.compiler.parent_path(); !parent.empty() && parent != parent.parent_path(); parent = parent.parent_path()) {
                        const auto possible = parent / "Auxiliary" / "Build" / "vcvars64.bat";
                        if (fs::is_regular_file(possible)) { t.environment_batch = possible; break; }
                    }
                }
                t.kind = ToolchainKind::msvc; t.license = "Microsoft Visual Studio Build Tools license (user-installed)";
                t.setup_url = "https://visualstudio.microsoft.com/visual-cpp-build-tools/";
                if (!t.environment_batch.empty()) {
                    // Only a Microsoft installation path enters cmd; source paths never enter shell commands.
                    if (t.environment_batch.wstring().find_first_of(L"%\r\n\"") != std::wstring::npos) throw std::runtime_error("Unsupported VS installation path");
                    wchar_t system[MAX_PATH]{}; GetSystemDirectoryW(system, MAX_PATH);
                    const auto command = L"\"call \"" + t.environment_batch.wstring() + L"\" >nul && set\"";
                    const auto env = detail::run(fs::path(system) / "cmd.exe", {L"/d", L"/s", L"/u", L"/c"}, {}, stop, 20, {}, command);
                    if (env.exit_code) throw std::runtime_error("vcvars64 environment setup failed");
                    std::wstring output(env.output.size() / sizeof(wchar_t), 0); std::memcpy(output.data(), env.output.data(), output.size() * sizeof(wchar_t));
                    std::wistringstream lines(output); std::wstring line;
                    while (std::getline(lines, line)) {
                        if (!line.empty() && line.back() == L'\r') line.pop_back();
                        if (line.find(L'=') == std::wstring::npos) continue;
                        t.environment.insert(t.environment.end(), line.begin(), line.end()); t.environment.push_back(0);
                    }
                    t.environment.push_back(0);
                }
                t.environment = clean_environment(t.environment, true);
                const auto version = detail::run(t.compiler, {L"/Bv"}, {}, stop, 10, t.environment);
                if (version.output.find("x64") == std::string::npos) throw std::runtime_error("MSVC x64 compiler is required");
                t.version = compiler_output(version.output, t.kind).substr(0, 2048);
            } else {
                t.kind = ToolchainKind::llvm_mingw; t.license = "LLVM Apache-2.0 WITH LLVM-exception; mingw-w64 notices in toolchain distribution";
                t.environment = clean_environment({}, false);
                t.setup_url = "https://github.com/mstorsjo/llvm-mingw";
                const auto version = detail::run(t.compiler, {L"--version"}, {}, stop, 10, t.environment);
                if (version.exit_code || (version.output.find("x86_64-w64") == std::string::npos && version.output.find("x86_64-w64-windows-gnu") == std::string::npos))
                    throw std::runtime_error("Use MSVC cl.exe or Windows x64 LLVM-MinGW clang++.exe");
                t.version = version.output.substr(0, 2048);
            }
            diagnostic.clear(); return t;
        } catch (const std::exception& e) { diagnostic = e.what(); }
    }
    if (diagnostic.empty()) diagnostic = "C++20 toolchain missing. Install Visual Studio C++ Build Tools (x64 + Windows SDK), or set MEDICON_SCRIPT_CXX to LLVM-MinGW clang++.exe.";
    return std::nullopt;
}

ScriptCompiler::ScriptCompiler(ScriptPaths paths, ScriptToolchain toolchain) : paths_(std::move(paths)), toolchain_(std::move(toolchain)) {}
void ScriptCompiler::write_compile_database(const std::vector<ScriptSource>& sources) const {
    Json commands = Json::array();
    for (const auto& source : sources) for (const auto& file : source.sources) {
        auto args = flags(toolchain_, paths_, source); args.push_back(toolchain_.kind == ToolchainKind::msvc ? L"/c" : L"-c"); args.push_back(file.wstring());
        commands.push_back({{"directory", utf8(source.directory)}, {"file", utf8(file)}, {"arguments", arguments_json(toolchain_.compiler, args)}});
    }
    atomic_write(paths_.sdk / "compile_commands.json", commands.dump(2));
}

BuildResult ScriptCompiler::build(ScriptSource source, std::atomic_bool& stop, bool force, const BuildProgressHandler& progress) const {
    return build_verified(std::move(source), stop, force, 0, progress);
}
BuildResult ScriptCompiler::build_verified(ScriptSource source, std::atomic_bool& stop, bool force, unsigned dependency_pass,
                                           const BuildProgressHandler& progress) const {
    BuildResult b; b.source = std::move(source);
    std::string transcript,stage="preparing";
    const size_t total_steps = b.source.sources.size() + 4;
    size_t completed_steps = 0;
    const auto report = [&](BuildStage next, std::string message, size_t completed) {
        completed_steps = completed;
        if (progress) progress({next, std::move(message), completed, total_steps, b.log});
    };
    const auto failure=[&](std::string code,std::string message,std::string hint) {
        b.diagnostics.push_back({b.source.entry,0,0,"error",std::move(code),std::move(message),std::move(hint)});
        if(!b.log.empty()) {
            const auto& d=b.diagnostics.back();
            transcript+="\n[Medicon / "+stage+"] "+d.code+": "+d.message+"\nHint: "+d.hint+"\n";
            try { atomic_write(b.log,transcript); } catch(...) { /* Keep the original diagnostic if logging fails. */ }
        }
    };
    try {
        report(BuildStage::preparing, dependency_pass ? "Rebuilding with discovered include dependencies (pass " + std::to_string(dependency_pass + 1) + ")" : "Checking source and build cache...", 0);
        const auto identity = toolchain_.identity();
        const auto index_file = paths_.cache / (sha256(b.source.key) + ".json");
        if (fs::exists(index_file)) {
            try {
                const auto index = Json::parse(read_text(index_file));
                auto candidate = b.source; std::vector<fs::path> deps;
                for (const auto& dependency : index.at("dependencies")) add_dependency(candidate, deps, path_from_utf8(dependency.get<std::string>()), paths_);
                const auto hash = ScriptDiscovery::fingerprint(candidate, paths_, identity);
                const auto binary = path_from_utf8(index.at("binary").get<std::string>());
                if (!force && hash == index.value("source", "") && ScriptPaths::contains(paths_.cache, binary) && fs::is_regular_file(binary) && sha256_file(binary) == index.value("hash", "")) {
                    b.source = std::move(candidate); b.source.fingerprint = hash; b.binary = binary; b.binary_hash = index.at("hash");
                    b.manifest = inspect_plugin(binary); b.dependencies = std::move(deps); b.success = b.cached = true;
                    report(BuildStage::complete, "Build succeeded (verified cache)", total_steps); return b;
                }
            } catch (...) { /* Invalid cache is rebuilt, never trusted. */ }
        }
        const auto initial = ScriptDiscovery::fingerprint(b.source, paths_, identity);
        const auto initial_inputs = b.source.inputs;
        static std::atomic_uint64_t sequence{};
        const auto work = paths_.cache / ("work-" + sha256(b.source.key).substr(0, 16) + "-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(sequence++));
        fs::create_directories(work);
        b.log = work / "build.log";
        const auto output = work / "module.dll";
        transcript="[Medicon] Compiler: "+utf8(toolchain_.compiler)+"\nCompiler version:\n"+toolchain_.version+"\nSDK include: "+utf8(paths_.sdk/"include")+"\n";
        for(const auto& header:{fs::path("medicon.hpp"),fs::path("medicon/abi.h")})
            transcript+="SDK header SHA256 ("+utf8(header)+"): "+sha256_file(paths_.sdk/"include"/header)+"\n";
        for (const auto& file : b.source.sources) {
            const auto text = read_text(file);
            transcript += "Source: " + utf8(file) + "\nSource SHA256: " + sha256(text) + "\nSource bytes: " + std::to_string(text.size()) +
                "\nMEDICON_MODULE text: " + (text.find("MEDICON_MODULE") == std::string::npos ? "absent" : "present") + "\n";
        }
        atomic_write(b.log,transcript);
        const auto run_command = [&](const std::vector<std::wstring>& args) {
            transcript += "\n[Medicon / " + stage + "] Command arguments:\n" + arguments_json(toolchain_.compiler, args).dump(2) + "\n";
            atomic_write(b.log, transcript);
            const auto result = detail::run(toolchain_.compiler, args, work, stop, 60, toolchain_.environment);
            transcript += compiler_output(result.output, toolchain_.kind);
            atomic_write(b.log, transcript);
            return result;
        };
        std::vector<fs::path> objects;
        std::vector<fs::path> depfiles;
        for (size_t i = 0; i < b.source.sources.size(); ++i) {
            stage="compiling";
            report(BuildStage::compiling, "Compiling " + std::to_string(i + 1) + "/" + std::to_string(b.source.sources.size()) + ": " + utf8(b.source.sources[i].filename()), i + 1);
            auto args = flags(toolchain_, paths_, b.source);
            const auto object = work / (std::to_string(i) + ".obj");
            const auto dep = work / (std::to_string(i) + (toolchain_.kind == ToolchainKind::msvc ? ".json" : ".d"));
            if (toolchain_.kind == ToolchainKind::msvc) { args.insert(args.end(), {L"/c", b.source.sources[i].wstring(), L"/Fo" + object.wstring(), L"/sourceDependencies", dep.wstring()}); }
            else args.insert(args.end(), {L"-c", b.source.sources[i].wstring(), L"-o", object.wstring(), L"-MMD", L"-MF", dep.wstring()});
            const auto result = run_command(args);
            if (result.exit_code || result.limited) {
                b.diagnostics = parse_diagnostics(transcript);
                if (result.limited) b.diagnostics.push_back({b.source.sources[i], 0, 0, "error", "BUILD_LIMIT", "Compiler cancelled or exceeded time/memory/CPU/output limit", "Reduce the module size and inspect build.log."});
                throw std::runtime_error("C++ compilation failed; previous version is still running");
            }
            objects.push_back(object); depfiles.push_back(dep);
        }
        auto args = flags(toolchain_, paths_, b.source);
        stage="linking";
        report(BuildStage::linking, "Linking module DLL...", b.source.sources.size() + 1);
        for (const auto& object : objects) args.push_back(object.wstring());
        if (toolchain_.kind == ToolchainKind::msvc) args.insert(args.end(), {L"/LD", L"/Fe" + output.wstring(), L"/link", L"/MACHINE:X64", L"/INCREMENTAL:NO"});
        else args.insert(args.end(), {L"-shared", L"-o", output.wstring()});
        const auto result = run_command(args);
        b.diagnostics = parse_diagnostics(transcript);
        if (result.exit_code || result.limited) throw std::runtime_error("DLL linking failed (require exactly one MEDICON_MODULE)");
        stage="verifying inputs";
        report(BuildStage::verifying, "Verifying source and include dependencies...", b.source.sources.size() + 2);
        if (ScriptDiscovery::fingerprint(b.source, paths_, identity) != initial) throw std::runtime_error("Source changed during build; save again to retry");
        for (const auto& dep : depfiles) {
            if (toolchain_.kind == ToolchainKind::msvc) {
                const auto json = Json::parse(read_text(dep));
                for (const auto& include : json.at("Data").at("Includes")) {
                    const auto path = path_from_utf8(include.get<std::string>());
                    // MSVC system headers belong to the selected installation/Windows SDK.
                    if (ScriptPaths::contains(b.source.directory, path) || ScriptPaths::contains(paths_.sdk, path)) add_dependency(b.source, b.dependencies, path, paths_);
                    else if (std::find(b.source.inputs.begin(), b.source.inputs.end(), path) != b.source.inputs.end()) add_dependency(b.source, b.dependencies, path, paths_);
                    else if (utf8(path).find("/Windows Kits/") == std::string::npos && utf8(path).find("/VC/Tools/") == std::string::npos) add_dependency(b.source, b.dependencies, path, paths_);
                }
            } else parse_depfile(dep, b.source, b.dependencies, paths_);
        }
        std::ranges::sort(b.dependencies); b.dependencies.erase(std::unique(b.dependencies.begin(), b.dependencies.end()), b.dependencies.end());
        const bool new_dependency = std::ranges::any_of(b.dependencies, [&](const auto& p) {
            return std::ranges::none_of(initial_inputs, [&](const auto& known) { return fs::weakly_canonical(known) == p; });
        });
        if (new_dependency) {
            if (dependency_pass >= 3) throw std::runtime_error("Include dependencies are unstable; rebuild after inputs stop changing");
            // First discovery of an external include is followed by a complete
            // build with that input hashed BEFORE and AFTER compilation.
            return build_verified(std::move(b.source), stop, true, dependency_pass + 1, progress);
        }
        b.source.fingerprint = ScriptDiscovery::fingerprint(b.source, paths_, identity);
        stage="inspecting DLL";
        report(BuildStage::verifying, "Checking DLL registration and API / ABI...", b.source.sources.size() + 3);
        try {
            b.manifest = inspect_plugin(output);
        } catch (const ScriptInspectionError& inspection) {
            if (toolchain_.kind != ToolchainKind::msvc || inspection.code != "SCRIPT_REGISTRATION" || stop) throw;
            // An object can define the C ABI but omit the compiler's export directives.
            // Require the exact ABI names at link time; undefined registration still fails.
            report(BuildStage::verifying, "Relinking MSVC DLL with explicit script exports...", b.source.sources.size() + 3);
            transcript += "\n[Medicon] " + std::string(inspection.what()) + "\nRetrying MSVC link with explicit script exports.\n";
            args.insert(args.end(), {L"/EXPORT:medicon_script_manifest,DATA", L"/EXPORT:medicon_script_entry"});
            const auto retry = run_command(args);
            b.diagnostics = parse_diagnostics(transcript);
            if (retry.exit_code || retry.limited)
                throw ScriptInspectionError("SCRIPT_REGISTRATION", "Required script registration could not be linked. Exactly one MEDICON_MODULE(...) is required.",
                    "Confirm the saved .cpp contains MEDICON_MODULE outside comments and inactive #if branches. See SDK/examples/HelloWorld.cpp. Build log records source paths, hashes and commands.");
            b.manifest = inspect_plugin(output);
            transcript += "\n[Medicon] Explicit script exports verified successfully.\n";
            atomic_write(b.log, transcript);
        }
        if (!b.source.declared_id.empty() && b.source.declared_id != b.manifest.id) throw std::runtime_error("medicon.toml id differs from MEDICON_MODULE ID");
        stage="publishing DLL";
        report(BuildStage::verifying, "Saving verified DLL and build cache...", b.source.sources.size() + 3);
        b.binary_hash = sha256_file(output);
        const auto shadow = work / (b.binary_hash + ".dll"); fs::rename(output, shadow); b.binary = shadow;
        Json deps = Json::array(); for (const auto& p : b.dependencies) deps.push_back(utf8(p));
        atomic_write(index_file, Json{{"source", b.source.fingerprint}, {"binary", utf8(shadow)}, {"hash", b.binary_hash}, {"dependencies", deps}}.dump(2));
        b.success = true;
    } catch(const ScriptInspectionError& e) {
        failure(e.code,e.what(),e.hint);
    } catch (const std::exception& e) {
        failure("BUILD",e.what(),"Open build.log or SDK/README.md. A failed build never replaces the active DLL.");
    }
    report(b.success ? BuildStage::complete : BuildStage::failed,
        b.success ? "Build succeeded" : "Build failed while " + stage + ": " + b.diagnostics.back().message,
        b.success ? total_steps : completed_steps);
    return b;
}
} // namespace medicon::scripts
