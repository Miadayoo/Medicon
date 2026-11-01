#pragma once
#include <medicon/core/ConfigStore.hpp>
#include <medicon/core/ModuleManager.hpp>
#include <medicon/scripts/ScriptRuntime.hpp>
#include <condition_variable>
#include <deque>
#include <thread>

namespace medicon::scripts {
struct ScriptView {
    std::string key, id, name, status, binary_hash, fingerprint;
    fs::path source, log;
    std::vector<ScriptDiagnostic> diagnostics;
    BuildProgress progress;
    bool approval_required = false, active = false;
    ScriptTelemetry telemetry;
    uint64_t generation = 0;
};
struct ScriptStatus {
    std::vector<ScriptView> scripts;
    std::string toolchain, version, license, setup_url, error, mcbe_version, build_status;
    uint64_t features = 0;
    bool detecting = true;
};

class ScriptManager {
public:
    ScriptManager(ModuleManager& modules, fs::path config, fs::path compiler = {});
    ~ScriptManager();
    void start();
    void stop();
    // Main/frame thread only, under the same host lock as modules.tick/render.
    void pump();
    void key_event(int key, bool down, bool repeat);
    void record_key_event(int key,bool down) noexcept { if(key>=0&&key<256) services_.keys[static_cast<size_t>(key)]=down; }
    void render_ui();
    HostServices& host() noexcept { return services_; }
    void set_mcbe_version(std::string version);
    void set_before_remove(std::function<void(const medicon::Module&)> callback) { before_remove_ = std::move(callback); }
    void set_save_handler(std::function<void()> callback) { save_ = std::move(callback); }
    ScriptStatus snapshot() const;
    void request_build(std::string key = {}, bool force = false);
    void request_approval(std::string key, std::string binary_hash);
    void request_revoke(std::string key);
    void request_create(std::string name,std::string template_name);
    // Frame-thread access for the management screen's standard setting editor.
    medicon::Module* module(std::string_view key) noexcept;
    void settings_edited(bool changed, bool editing);
    const ScriptPaths& paths() const noexcept { return paths_; }
    void audit(uint32_t severity, std::string_view message);
private:
    friend class ScriptReloadCoordinator;
    struct Request { enum Kind { build, approve, revoke, create } kind; std::string key, hash; bool force = false; };
    struct Ready { BuildResult build; uint64_t generation; bool remove = false; };
    void worker();
    void set_result(const BuildResult& build, uint64_t generation, bool trusted);
    void remove_active(const std::string& key, bool save = true);

    ModuleManager& modules_;
    ConfigStore config_;
    ScriptPaths paths_;
    fs::path compiler_;
    HostServices services_;
    ScriptTrustManager trust_;
    std::function<void(const medicon::Module&)> before_remove_;
    std::function<void()> save_;
    std::map<std::string, std::string> active_; // frame thread only, key -> module ID
    bool settings_dirty_ = false;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::map<std::string, ScriptView> views_;
    std::map<std::string, BuildResult> pending_;
    std::deque<Request> requests_;
    std::deque<Ready> ready_;
    std::deque<std::pair<uint32_t, std::string>> audit_;
    ScriptStatus status_;
    std::atomic_bool stopping_{false};
    std::thread worker_;
};
} // namespace medicon::scripts
