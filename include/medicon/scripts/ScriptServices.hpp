#pragma once

#include <medicon/abi.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace medicon::scripts {
namespace fs = std::filesystem;

struct ScriptPaths {
    fs::path root, sources, sdk, cache, logs;
    explicit ScriptPaths(fs::path base);
    void initialize() const;
    static bool contains(const fs::path& root, const fs::path& path);
};

struct ScriptDiagnostic {
    fs::path file;
    int line = 0, column = 0;
    std::string severity = "error", code, message, hint;
};
std::vector<ScriptDiagnostic> parse_diagnostics(std::string_view output);
std::string sha256_file(const fs::path& path);
std::string sha256(std::string_view bytes);
bool valid_id(std::string_view value);
std::string utf8(const fs::path& path);
fs::path path_from_utf8(std::string_view text);

struct ScriptSource {
    std::string key;
    fs::path entry, directory;
    std::vector<fs::path> sources, inputs;
    std::string description;
    std::string declared_id;
    uint64_t required_features = 0;
    std::string fingerprint;
};

class ScriptDiscovery {
public:
    // Root .cpp files are independent; each immediate child directory is one module.
    static std::vector<ScriptSource> scan(const ScriptPaths& paths, std::vector<ScriptDiagnostic>& diagnostics);
    static std::string fingerprint(const ScriptSource& source, const ScriptPaths& paths,
                                   std::string_view toolchain);
};

enum class ToolchainKind { msvc, llvm_mingw };
struct ScriptToolchain {
    ToolchainKind kind = ToolchainKind::msvc;
    fs::path compiler;
    fs::path environment_batch;
    std::vector<wchar_t> environment;
    std::string version, license, setup_url;
    std::string identity() const;
    static std::optional<ScriptToolchain> detect(const fs::path& preferred, std::atomic_bool& stop,
                                               std::string& diagnostic);
};

struct BuildResult {
    ScriptSource source;
    fs::path binary, log;
    std::string binary_hash;
    MediconScriptManifest manifest{};
    std::vector<ScriptDiagnostic> diagnostics;
    std::vector<fs::path> dependencies;
    bool success = false, cached = false;
};

enum class BuildStage { idle, queued, detecting_toolchain, preparing, compiling, linking, verifying, loading, complete, failed };
struct BuildProgress {
    BuildStage stage = BuildStage::idle;
    std::string message;
    // Completed pipeline steps, not an estimate of elapsed compile time.
    size_t completed_steps = 0, total_steps = 0;
    fs::path log{};
    bool running() const noexcept { return stage != BuildStage::idle && stage != BuildStage::complete && stage != BuildStage::failed; }
};
using BuildProgressHandler = std::function<void(const BuildProgress&)>;

class ScriptCompiler {
public:
    ScriptCompiler(ScriptPaths paths, ScriptToolchain toolchain);
    BuildResult build(ScriptSource source, std::atomic_bool& stop, bool force = false,
                      const BuildProgressHandler& progress = {}) const;
    void write_compile_database(const std::vector<ScriptSource>& sources) const;
    const ScriptToolchain& toolchain() const noexcept { return toolchain_; }
private:
    BuildResult build_verified(ScriptSource source, std::atomic_bool& stop, bool force, unsigned dependency_pass,
                               const BuildProgressHandler& progress) const;
    ScriptPaths paths_;
    ScriptToolchain toolchain_;
};

// File parsing only: never maps a DLL or executes its DllMain/entry point.
MediconScriptManifest inspect_plugin(const fs::path& path);

class ScriptTrustManager {
public:
    explicit ScriptTrustManager(fs::path path);
    bool approved(const BuildResult& build) const;
    void approve(const BuildResult& build);
    void revoke(std::string_view key);
private:
    fs::path path_;
};

// Does not interpret or execute the message. The worker is the only writer.
void append_audit(const ScriptPaths& paths, std::string_view event, std::string_view detail);
void atomic_write(const fs::path& path, std::string_view content);

} // namespace medicon::scripts
