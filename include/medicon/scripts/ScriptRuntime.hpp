#pragma once
#include <medicon/core/Module.hpp>
#include <medicon/scripts/ScriptServices.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <array>
#include <set>
#include <medicon/scripts/ScriptStorage.hpp>

namespace medicon::scripts {
struct HostServices {
    uint64_t features = 0;
    std::function<void(uint32_t, std::string_view)> log;
    std::function<bool(std::string_view)> chat;
    std::function<std::optional<MediconPlayerSnapshot>()> player;
    std::function<MediconPoint()> render_size;
    std::function<void(MediconPoint, float, MediconColor)> circle;
    std::function<void(MediconPoint, std::string_view, MediconColor, float)> text;
    std::function<MediconFrameInfo()> frame;
    std::function<MediconPoint()> cursor;
    std::function<bool()> input_focused;
    std::function<double()> clock;
    std::function<uint64_t()> world_identity;
    std::function<uint64_t()> capabilities;
    std::function<int32_t(const MediconDrawCommand&)> shape;
    std::function<int32_t(std::string_view,uint64_t,const MediconRect&,const MediconGlassStyle&)> glass;
    std::function<uint32_t(std::string_view,uint64_t,std::string_view,const MediconRect&,const MediconGlassStyle&)> ui_begin;
    std::function<void(uint32_t)> ui_end;
    std::function<int32_t()> ui_status;
    std::function<int32_t(uint32_t,const MediconWidget&)> ui_widget;
    std::function<void()> ui_finish;
    std::function<MediconColor(uint32_t)> theme_color;
    std::function<bool(std::string_view,uint64_t,std::string_view,std::string_view,uint32_t)> notify;
    std::function<bool(uint32_t)> sound;
    std::function<void(std::string_view,uint64_t)> cleanup;
    std::shared_ptr<ScriptStorage> storage;
    std::array<bool,256> keys{};
};
struct ScriptTelemetry {
    uint64_t generation=0, callbacks=0, draw_commands=0, glass_panels=0, rejected=0;
    double total_us=0, max_us=0;
};

class ScriptPluginLoader {
public:
    class Library;
    // Both an exact source+binary approval and offline ABI inspection are
    // required, BEFORE loading. The file stays locked against replacement.
    static std::shared_ptr<Library> load(const BuildResult& build, const ScriptTrustManager& trust, uint64_t features);
};

class ScriptModuleAdapter final : public medicon::Module {
public:
    ScriptModuleAdapter(const BuildResult& build, std::shared_ptr<ScriptPluginLoader::Library> library, HostServices& services);
    ~ScriptModuleAdapter() override;
    bool healthy() const noexcept { return !faulted_; }
    const std::string& fault() const noexcept { return fault_; }
    // Stops admission, waits for callback lock, disables, then destroys the
    // plugin object. The Library cannot unload while any callback is running.
    void drain() noexcept;
    void key_event(int key, bool down, bool repeat) noexcept;
    void ui() noexcept;
    const std::vector<ScriptDiagnostic>& runtime_diagnostics() const noexcept { return diagnostics_; }
    const ScriptTelemetry& telemetry() const noexcept { return telemetry_; }
    static void transfer_settings(const medicon::Module& old, medicon::Module& next);
private:
    void on_enable() noexcept override;
    void on_disable() noexcept override;
    void on_tick(float delta) noexcept override;
    void on_render() noexcept override;
    void failed(std::string_view callback) noexcept;
    void refresh_features() noexcept { host_.features = services_.features; }
    double now() const;
    bool command(std::string_view api);
    void diagnostic(std::string code,std::string message,const MediconCallSite* site=nullptr);
    bool glass_style(const MediconGlassStyle&,const MediconCallSite*);
    void timers_tick();
    void lifecycle_events();
    void cleanup() noexcept;
    void measured(std::chrono::steady_clock::time_point start) noexcept;

    static void MEDICON_SCRIPT_CALL host_log(void*, uint32_t, MediconString) noexcept;
    static int32_t MEDICON_SCRIPT_CALL host_chat(void*, MediconString) noexcept;
    static int32_t MEDICON_SCRIPT_CALL define_setting(void*, const MediconSettingDescriptor*) noexcept;
    static int32_t MEDICON_SCRIPT_CALL read_setting(void*, MediconString, MediconValue*) noexcept;
    static int32_t MEDICON_SCRIPT_CALL setting_range(void*, MediconString, float, float, float) noexcept;
    static int32_t MEDICON_SCRIPT_CALL local_player(void*, MediconPlayerSnapshot*) noexcept;
    static MediconPoint MEDICON_SCRIPT_CALL render_size(void*) noexcept;
    static void MEDICON_SCRIPT_CALL fill_circle(void*, MediconPoint, float, MediconColor) noexcept;
    static void MEDICON_SCRIPT_CALL text(void*, MediconPoint, MediconString, MediconColor, float) noexcept;
    static int32_t MEDICON_SCRIPT_CALL frame_info(void*,MediconFrameInfo*) noexcept;
    static int32_t MEDICON_SCRIPT_CALL draw_shape(void*,const MediconDrawCommand*) noexcept;
    static int32_t MEDICON_SCRIPT_CALL glass(void*,const MediconRect*,const MediconGlassStyle*,const MediconCallSite*) noexcept;
    static uint32_t MEDICON_SCRIPT_CALL ui_begin(void*,MediconString,const MediconRect*,const MediconGlassStyle*,const MediconCallSite*) noexcept;
    static void MEDICON_SCRIPT_CALL ui_end(void*,uint32_t) noexcept;
    static int32_t MEDICON_SCRIPT_CALL ui_widget(void*,uint32_t,const MediconWidget*) noexcept;
    static int32_t MEDICON_SCRIPT_CALL theme_color(void*,uint32_t,MediconColor*) noexcept;
    static int32_t MEDICON_SCRIPT_CALL input(void*,int32_t,MediconPoint*) noexcept;
    static int32_t MEDICON_SCRIPT_CALL notify(void*,MediconString,MediconString,uint32_t) noexcept;
    static int32_t MEDICON_SCRIPT_CALL play_sound(void*,uint32_t) noexcept;
    static int32_t MEDICON_SCRIPT_CALL storage_get(void*,MediconString,char*,uint32_t) noexcept;
    static int32_t MEDICON_SCRIPT_CALL storage_set(void*,MediconString,MediconString) noexcept;
    static uint64_t MEDICON_SCRIPT_CALL timer_add(void*,double,double,MediconTimerCallback,MediconTimerDestroy,void*) noexcept;
    static void MEDICON_SCRIPT_CALL timer_cancel(void*,uint64_t) noexcept;

    std::shared_ptr<ScriptPluginLoader::Library> library_;
    HostServices& services_;
    MediconHostApi host_{};
    MediconPluginApi plugin_{};
    void* object_ = nullptr;
    std::recursive_mutex callbacks_;
    bool constructing_ = true, retired_ = false, rendering_ = false, faulted_ = false;
    bool ui_active_=false, world_present_=false, screen_seen_=false;
    uint64_t generation_=0, previous_world_=0, next_timer_=1;
    uint32_t commands_=0, panels_=0;
    float delta_=0;
    MediconScreenEvent previous_screen_{};
    fs::path source_;
    ScriptTelemetry telemetry_;
    std::vector<ScriptDiagnostic> diagnostics_;
    std::set<std::string> reported_;
    struct Timer {
        double due=0,interval=0;
        MediconTimerCallback callback=nullptr;
        MediconTimerDestroy destroy=nullptr;
        void* data=nullptr;
        ~Timer() { if(destroy) destroy(data); }
    };
    std::map<uint64_t,std::shared_ptr<Timer>> timers_;
    std::string fault_;
};

class ScriptReloadCoordinator {
public:
    // Under the host's frame lock. The old DLL stays mapped until the new
    // module has constructed/restored/enabled successfully, permitting rollback.
    static bool replace(class ScriptManager& manager, const BuildResult& build, std::string& status);
};
} // namespace medicon::scripts
