#include <medicon/scripts/ScriptRuntime.hpp>
#include "ScriptProcess.hpp"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace medicon::scripts {
namespace {
std::string_view view(MediconString value) {
    if (value.size > 16384 || (value.size && !value.data)) throw std::invalid_argument("SDK text exceeds 16 KiB");
    return value.size ? std::string_view(value.data, value.size) : std::string_view{};
}
bool valid_color(MediconColor c) {
    return std::ranges::all_of(std::array{c.r,c.g,c.b,c.a}, [](float v) { return std::isfinite(v) && v >= 0 && v <= 1; });
}
bool valid_point(MediconPoint p) { return std::isfinite(p.x) && std::isfinite(p.y); }
}
class ScriptPluginLoader::Library {
public:
    detail::Handle file;
    HMODULE module{};
    MediconScriptEntry entry{};
    ~Library() { if (module) FreeLibrary(module); }
};
std::shared_ptr<ScriptPluginLoader::Library> ScriptPluginLoader::load(const BuildResult& build, const ScriptTrustManager& trust, uint64_t features) {
    if (!build.success || !trust.approved(build)) throw std::runtime_error("Native code has not been approved for this source and DLL hash");
    auto library = std::make_shared<Library>();
    library->file.value = CreateFileW(build.binary.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (library->file.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot lock shadow DLL against replacement");
    if (sha256_file(build.binary) != build.binary_hash) throw std::runtime_error("DLL changed after approval; rebuild and approve again");
    const auto manifest = inspect_plugin(build.binary);
    if (std::memcmp(&manifest, &build.manifest, sizeof(manifest)) != 0) throw std::runtime_error("DLL metadata changed after build");
    const auto required = manifest.required_features | build.source.required_features;
    if ((required & features) != required) throw std::runtime_error("Required MCBE feature is unavailable on this host");
    library->module = LoadLibraryExW(build.binary.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library->module) throw std::runtime_error("Cannot load approved script DLL (Win32 " + std::to_string(GetLastError()) + ")");
    library->entry = reinterpret_cast<MediconScriptEntry>(reinterpret_cast<void*>(GetProcAddress(library->module, "medicon_script_entry")));
    if (!library->entry) throw std::runtime_error("Approved DLL entry is missing");
    return library;
}

ScriptModuleAdapter::ScriptModuleAdapter(const BuildResult& b, std::shared_ptr<ScriptPluginLoader::Library> library, HostServices& services)
    : medicon::Module(b.manifest.id, b.manifest.name, b.source.description.empty() ? "C++ script: " + b.source.key : b.source.description, medicon::Category::Scripts),
      library_(std::move(library)), services_(services), source_(b.source.entry) {
    static std::atomic_uint64_t generation{0}; generation_=++generation; telemetry_.generation=generation_;
    host_ = {sizeof(host_), MEDICON_SCRIPT_ABI_VERSION, services_.features, this, host_log, host_chat, define_setting,
        read_setting, setting_range, local_player, render_size, fill_circle, text};
    host_.frame_info=frame_info; host_.draw_shape=draw_shape; host_.glass=glass; host_.ui_begin=ui_begin; host_.ui_end=ui_end;
    host_.ui_widget=ui_widget; host_.theme_color=theme_color; host_.input=input; host_.notify=notify; host_.play_sound=play_sound;
    host_.storage_get=storage_get; host_.storage_set=storage_set; host_.timer_add=timer_add; host_.timer_cancel=timer_cancel;
    const bool legacy=b.manifest.api_version == MEDICON_SCRIPT_LEGACY_API_VERSION;
    if (legacy) host_.size=MEDICON_HOST_V1_SIZE;
    plugin_.size = legacy ? MEDICON_PLUGIN_V1_SIZE : sizeof(plugin_);
    if (!library_->entry(MEDICON_SCRIPT_ABI_VERSION, &plugin_) || plugin_.size < MEDICON_PLUGIN_V1_SIZE || plugin_.size > sizeof(plugin_) ||
        plugin_.abi_version != MEDICON_SCRIPT_ABI_VERSION || !plugin_.create || !plugin_.destroy || !plugin_.enable ||
        !plugin_.disable || !plugin_.tick || !plugin_.render || !plugin_.event) throw std::runtime_error("Invalid plugin function table");
    object_ = plugin_.create(&host_);
    constructing_ = false;
    if (!object_) throw std::runtime_error("Script constructor failed; see script-manager.log");
}
ScriptModuleAdapter::~ScriptModuleAdapter() { drain(); }
void ScriptModuleAdapter::drain() noexcept {
    std::lock_guard lock(callbacks_);
    if (retired_) return;
    set_enabled(false);
    cleanup();
    retired_ = true;
    if (object_) { plugin_.destroy(object_); object_ = nullptr; }
}
void ScriptModuleAdapter::failed(std::string_view callback) noexcept {
    faulted_ = true;
    try { fault_ = std::string(callback) + " failed; script disabled (see log)"; if (services_.log) services_.log(2, name() + ": " + fault_); } catch (...) {}
    if (enabled()) set_enabled(false);
}
void ScriptModuleAdapter::on_enable() noexcept {
    std::lock_guard lock(callbacks_); refresh_features();
    if (retired_ || !object_) { set_enabled(false); return; }
    faulted_ = false; fault_.clear();
    commands_=panels_=0; world_present_=screen_seen_=false;
    const auto start=std::chrono::steady_clock::now();
    if (!plugin_.enable(object_)) failed("on_enable");
    measured(start);
}
void ScriptModuleAdapter::on_disable() noexcept {
    std::lock_guard lock(callbacks_); refresh_features();
    cleanup();
    if (!retired_ && object_ && !plugin_.disable(object_)) failed("on_disable");
}
void ScriptModuleAdapter::on_tick(float delta) noexcept {
    std::lock_guard lock(callbacks_); refresh_features();
    if (retired_ || !object_) return;
    commands_=panels_=0; delta_=std::clamp(delta,0.F,.1F);
    const auto start=std::chrono::steady_clock::now();
    try { lifecycle_events(); if (enabled()) timers_tick(); } catch (...) { failed("timer/event host dispatch"); }
    if (enabled() && !plugin_.tick(object_, delta_)) failed("on_tick/TickEvent");
    measured(start);
}
void ScriptModuleAdapter::on_render() noexcept {
    std::lock_guard lock(callbacks_); refresh_features();
    if (retired_ || !object_) return;
    commands_=panels_=0;
    const auto start=std::chrono::steady_clock::now();
    rendering_ = true;
    const auto success = plugin_.render(object_);
    rendering_ = false;
    if (!success) failed("on_render/RenderEvent");
    measured(start);
}
void ScriptModuleAdapter::key_event(int key, bool down, bool repeat) noexcept {
    std::lock_guard lock(callbacks_); refresh_features();
    if (!enabled() || retired_ || !object_) return;
    const MediconScriptKeyEvent event{key, down ? 1u : 0u, repeat ? 1u : 0u};
    if (!plugin_.event(object_, MEDICON_EVENT_KEY, &event)) failed("KeyEvent");
}
void ScriptModuleAdapter::transfer_settings(const medicon::Module& old, medicon::Module& next) {
    next.set_keybind(old.keybind());
    for (auto& setting : next.settings()) for (const auto& previous : old.settings()) {
        if (previous.id != setting.id || previous.value.index() != setting.value.index()) continue;
        auto value = previous.value;
        if (auto* f = std::get_if<float>(&value)) { if (!std::isfinite(*f)) continue; *f = std::clamp(*f, setting.minimum, setting.maximum); }
        if (auto* i = std::get_if<int>(&value)) *i = static_cast<int>(std::clamp(static_cast<double>(*i), static_cast<double>(setting.minimum), static_cast<double>(setting.maximum)));
        setting.value = std::move(value);
    }
}

void MEDICON_SCRIPT_CALL ScriptModuleAdapter::host_log(void* context, uint32_t severity, MediconString message) noexcept {
    try { auto& self = *static_cast<ScriptModuleAdapter*>(context); if (self.services_.log) self.services_.log(severity, self.id() + ": " + std::string(view(message))); } catch (...) {}
}
int32_t MEDICON_SCRIPT_CALL ScriptModuleAdapter::host_chat(void* context, MediconString message) noexcept {
    try { auto& self = *static_cast<ScriptModuleAdapter*>(context); return (self.host_.features & MEDICON_FEATURE_CHAT) && self.services_.chat && self.services_.chat(view(message)); } catch (...) { return 0; }
}
int32_t MEDICON_SCRIPT_CALL ScriptModuleAdapter::define_setting(void* context, const MediconSettingDescriptor* d) noexcept {
    try {
        auto& self = *static_cast<ScriptModuleAdapter*>(context);
        if (!self.constructing_ || !d || self.settings().size() >= 128 || !valid_id(view(d->id)) || view(d->label).empty()) return 0;
        medicon::Setting::Value value;
        switch (d->initial.type) {
        case MEDICON_BOOL: value = d->initial.integer != 0; break;
        case MEDICON_INT: value = static_cast<int>(d->initial.integer); break;
        case MEDICON_FLOAT: value = d->initial.number; break;
        case MEDICON_COLOR: { const auto c = d->initial.color; value = medicon::Color{c.r,c.g,c.b,c.a}; break; }
        case MEDICON_STRING: value = std::string(view(d->initial.text)); break;
        default: return 0;
        }
        self.add_setting({std::string(view(d->id)), std::string(view(d->label)), "", std::move(value), d->minimum, d->maximum, d->step});
        return 1;
    } catch (...) { return 0; }
}
int32_t MEDICON_SCRIPT_CALL ScriptModuleAdapter::read_setting(void* context, MediconString id, MediconValue* out) noexcept {
    try {
        if (!out) return 0;
        auto& self = *static_cast<ScriptModuleAdapter*>(context);
        for (const auto& setting : self.settings()) if (setting.id == view(id)) {
            *out = {};
            std::visit([&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::same_as<T, bool>) { out->type = MEDICON_BOOL; out->integer = value; }
                else if constexpr (std::same_as<T, int>) { out->type = MEDICON_INT; out->integer = value; }
                else if constexpr (std::same_as<T, float>) { out->type = MEDICON_FLOAT; out->number = value; }
                else if constexpr (std::same_as<T, medicon::Color>) { out->type = MEDICON_COLOR; out->color = {value.r,value.g,value.b,value.a}; }
                else if constexpr (std::same_as<T, std::string>) { out->type = MEDICON_STRING; out->text = {value.data(), static_cast<uint32_t>(value.size())}; }
            }, setting.value);
            return out->type != 0;
        }
    } catch (...) {}
    return 0;
}
int32_t MEDICON_SCRIPT_CALL ScriptModuleAdapter::setting_range(void* context, MediconString id, float minimum, float maximum, float step) noexcept {
    try {
        auto& self = *static_cast<ScriptModuleAdapter*>(context);
        if (!self.constructing_ || !std::isfinite(minimum) || !std::isfinite(maximum) || !std::isfinite(step) || minimum > maximum || step <= 0) return 0;
        for (auto& setting : self.settings()) if (setting.id == view(id)) {
            const bool integer = std::holds_alternative<int>(setting.value);
            if (!integer && !std::holds_alternative<float>(setting.value)) return 0;
            const double value = integer ? static_cast<double>(setting.get<int>()) : setting.get<float>();
            if (value < minimum || value > maximum || (integer && (minimum < std::numeric_limits<int>::min() || maximum > static_cast<double>(std::numeric_limits<int>::max())))) return 0;
            setting.minimum = minimum; setting.maximum = maximum; setting.step = step; return 1;
        }
    } catch (...) {}
    return 0;
}
int32_t MEDICON_SCRIPT_CALL ScriptModuleAdapter::local_player(void* context, MediconPlayerSnapshot* out) noexcept {
    try { auto& self = *static_cast<ScriptModuleAdapter*>(context); if (!out || !(self.host_.features & MEDICON_FEATURE_PLAYER) || !self.services_.player) return 0;
        if (const auto p = self.services_.player()) { *out = *p; return 1; } } catch (...) {}
    return 0;
}
MediconPoint MEDICON_SCRIPT_CALL ScriptModuleAdapter::render_size(void* context) noexcept {
    try { auto& self = *static_cast<ScriptModuleAdapter*>(context); if (self.rendering_ && self.services_.render_size) return self.services_.render_size(); } catch (...) {}
    return {};
}
void MEDICON_SCRIPT_CALL ScriptModuleAdapter::fill_circle(void* context, MediconPoint p, float radius, MediconColor c) noexcept {
    try { auto& self = *static_cast<ScriptModuleAdapter*>(context); if (self.rendering_ && self.services_.circle && valid_point(p) && valid_color(c) && std::isfinite(radius) && radius > 0 && radius <= 32768 && self.command("fill_circle")) self.services_.circle(p, radius, c); } catch (...) {}
}
void MEDICON_SCRIPT_CALL ScriptModuleAdapter::text(void* context, MediconPoint p, MediconString message, MediconColor c, float size) noexcept {
    try { auto& self = *static_cast<ScriptModuleAdapter*>(context); if (self.rendering_ && self.services_.text && valid_point(p) && valid_color(c) && std::isfinite(size) && size >= 1 && size <= 256 && self.command("text")) self.services_.text(p, view(message), c, size); } catch (...) {}
}
} // namespace medicon::scripts
