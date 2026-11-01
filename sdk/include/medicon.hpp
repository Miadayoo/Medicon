#pragma once
/** @file medicon.hpp
 * @brief Medicon Script SDK 0.4 (C++20). Include this header in scripts only.
 * @details All APIs run on the serialized host callback thread, except rendering,
 * which is valid only in on_render/RenderEvent. Never retain a RenderContext or
 * call this SDK from a user-created thread. The C ABI owns no STL objects.
 * @example HelloWorld.cpp
 */
#include <medicon/abi.h>
#include <medicon/types.hpp>
#include <algorithm>
#include <cmath>
#include <concepts>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <chrono>
#include <cstring>
#include <source_location>

namespace medicon { inline namespace script_v1 {
/// Script classification metadata. ClickGUI displays every script in Scripts.
enum class Category { Visual, Movement, Player, Utility, Client, Misc = Utility };
/// An owned read-only snapshot; contains no game pointers. health < 0 means unknown.
using PlayerSnapshot = MediconPlayerSnapshot;
/// Published once per enabled host tick. delta_seconds is elapsed host time.
using TickEvent = MediconTickEvent;
/// A copied Win32-compatible key code and press/repeat flags (no native handles).
using KeyEvent = MediconScriptKeyEvent;
class Module;

namespace detail {
template<class T> struct Factory;
template<class Fn> int32_t invoke(Module*, Fn&&) noexcept;
inline thread_local const MediconHostApi* host = nullptr;
inline MediconString str(std::string_view s) { return {s.data(), static_cast<uint32_t>(s.size())}; }
struct HostScope {
    const MediconHostApi* previous;
    explicit HostScope(const MediconHostApi* h) : previous(std::exchange(host, h)) {}
    ~HostScope() { host = previous; }
};
inline const MediconHostApi& api() {
    if (!host) throw std::logic_error("Medicon SDK is only available in host callbacks");
    return *host;
}
template<class T> constexpr uint32_t value_type() {
    if constexpr (std::same_as<T, bool>) return MEDICON_BOOL;
    else if constexpr (std::same_as<T, int>) return MEDICON_INT;
    else if constexpr (std::same_as<T, float>) return MEDICON_FLOAT;
    else if constexpr (std::same_as<T, Color>) return MEDICON_COLOR;
    else if constexpr (std::same_as<T, std::string>) return MEDICON_STRING;
    else { static_assert(!sizeof(T), "setting<T>: supported types are bool, int, float, Color, std::string"); }
}
template<class T> MediconValue encode(const T& v) {
    MediconValue r{}; r.type = value_type<T>();
    if constexpr (std::same_as<T, bool> || std::same_as<T, int>) r.integer = v;
    else if constexpr (std::same_as<T, float>) r.number = v;
    else if constexpr (std::same_as<T, Color>) r.color = {v.r,v.g,v.b,v.a};
    else r.text = str(v);
    return r;
}
template<class T> T decode(const MediconValue& v) {
    if (v.type != value_type<T>()) throw std::runtime_error("Setting type changed");
    if constexpr (std::same_as<T, bool>) return v.integer != 0;
    else if constexpr (std::same_as<T, int>) return v.integer;
    else if constexpr (std::same_as<T, float>) return v.number;
    else if constexpr (std::same_as<T, Color>) return {v.color.r,v.color.g,v.color.b,v.color.a};
    else return std::string(v.text.data, v.text.size);
}
} // namespace detail

namespace sdk {
struct Version {
    uint32_t value;
    constexpr operator uint32_t() const { return value; }
    constexpr uint32_t operator()() const { return value; }
};
/// @return Public API version (400 = 0.4.0); usable on any thread.
inline constexpr Version api_version{MEDICON_SCRIPT_API_VERSION};
/// @return C ABI version; usable on any thread.
inline constexpr Version abi_version{MEDICON_SCRIPT_ABI_VERSION};
}
namespace features {
/// Feature names used by is_available and medicon.toml required_features.
enum class Feature : uint64_t { Chat = MEDICON_FEATURE_CHAT, Render = MEDICON_FEATURE_RENDER, LocalPlayer = MEDICON_FEATURE_PLAYER };
/** @brief Check an optional capability in a callback.
 * @param feature Capability to query. @return true if the current host supports it.
 * @code if (features::is_available(features::Feature::Chat)) chat::print("Ready"); @endcode */
inline bool is_available(Feature feature) { return (detail::api().features & static_cast<uint64_t>(feature)) != 0; }
/// @param name "chat", "render", or "local_player". @return false for unknown names.
inline bool is_available(std::string_view name) {
    return name == "chat" ? is_available(Feature::Chat) : name == "render" ? is_available(Feature::Render) :
        name == "local_player" && is_available(Feature::LocalPlayer);
}
}
namespace log {
/// @brief Queue an informational audit entry. @param message UTF-8 text (max 16 KiB). Callback thread only.
inline void info(std::string_view message) { auto& h = detail::api(); h.log(h.context, 0, detail::str(message)); }
/// @brief Queue a warning audit entry. @param message UTF-8 text. Callback thread only.
inline void warn(std::string_view message) { auto& h = detail::api(); h.log(h.context, 1, detail::str(message)); }
/// @brief Queue an error audit entry. @param message UTF-8 text. Callback thread only.
inline void error(std::string_view message) { auto& h = detail::api(); h.log(h.context, 2, detail::str(message)); }
}
namespace chat {
/** @brief Queue local chat feedback through the host's native game-thread bridge.
 * @param message UTF-8 text. @return false when chat is unavailable or the queue is full.
 * @code chat::print("Hello, Medicon!"); @endcode Callback thread only. */
inline bool print(std::string_view message) { auto& h = detail::api(); return h.chat(h.context, detail::str(message)) != 0; }
}
namespace game {
/** @brief Copy the most recent local player snapshot in a callback.
 * @return An owned snapshot, or nullopt outside a world/without a supported profile.
 * @code if (auto p = game::local_player()) log::info(std::to_string(p->health)); @endcode */
inline std::optional<PlayerSnapshot> local_player() {
    PlayerSnapshot p{}; auto& h = detail::api();
    if (!h.local_player(h.context, &p)) return std::nullopt;
    return p;
}
}

#include <medicon/detail/services.inl>

/// Drawing facade. Borrow only within on_render or a RenderEvent callback.
class RenderContext {
public:
    /// @return A context using host-scaled logical DIPs. Existing methods remain physical by default.
    RenderContext logical() const { RenderContext c; c.scale_ = std::max(.01F, detail::frame().scale); return c; }
    /// @return Display extent in this context's units. Valid only in a render callback.
    Size viewport_size() const { const auto f = detail::frame(); return {f.width / scale_, f.height / scale_}; }
    /** @return Screen center in pixels. Render callback only.
     * @code auto c = context.center(); @endcode */
    Vec2 center() const { auto& h = detail::api(); auto p = h.render_size(h.context); return {p.x * .5F / scale_, p.y * .5F / scale_}; }
    /** @brief Draw a filled circle. @param position Center in pixels.
     * @param radius Positive radius in pixels. @param color Normalized RGBA.
     * @code context.fill_circle(context.center(), 4, Color{1,0,0,1}); @endcode */
    void fill_circle(Vec2 position, float radius, Color color = {}) const {
        auto& h = detail::api(); h.fill_circle(h.context, {position.x*scale_,position.y*scale_}, radius*scale_, {color.r,color.g,color.b,color.a});
    }
    /** @brief Draw UTF-8 text. @param position Top-left pixel position.
     * @param message Text to draw. @param color Normalized RGBA. @param size Font size in pixels.
     * @code context.text({10,10}, "HUD", Color{}, 18); @endcode Render callback only. */
    void text(Vec2 position, std::string_view message, Color color = {}, float size = 16) const {
        auto& h = detail::api(); h.text(h.context, {position.x*scale_,position.y*scale_}, detail::str(message), {color.r,color.g,color.b,color.a}, size*scale_);
    }
    /** @brief Submit a copied pane to Medicon's existing Liquid Glass. No new GPU path.
     * @param rect Bounds in context units. @param style Value descriptor. @param source Automatic diagnostic location.
     * @return true for native optics, false for fallback/rejection. Check capabilities for unsupported style controls. */
    bool glass(Rect rect, const Glass& style = Glass::regular(), std::source_location source = std::source_location::current()) const {
        auto& h = detail::api(); if (!detail::extended() || !h.glass) return false;
        MediconRect r{rect.x*scale_,rect.y*scale_,rect.width*scale_,rect.height*scale_};
        auto d = style.descriptor(); d.radius *= scale_; const auto site = detail::site(source);
        return h.glass(h.context, &r, &d, &site) == MEDICON_RESULT_OK;
    }
    /// @brief Filled rounded rectangle. @param rect Bounds. @param radius Corner radius. @param color Normalized RGBA.
    bool fill_rounded_rect(Rect rect, float radius, Color color) const { return shape(1,rect,{},radius,color,1); }
    /// @brief Rounded outline. @param thickness Positive width. Other arguments use context units.
    bool stroke_rounded_rect(Rect rect, float radius, Color color, float thickness = 1) const { return shape(2,rect,{},radius,color,thickness); }
    /// @brief Line segment. @param from Start. @param to End. @param color RGBA. @param thickness Width.
    bool line(Vec2 from, Vec2 to, Color color, float thickness = 1) const { return shape(3,{from.x,from.y,0,0},to,0,color,thickness); }
private:
    bool shape(uint32_t kind, Rect r, Vec2 end, float radius, Color c, float width) const {
        auto& h = detail::api(); if (!detail::extended() || !h.draw_shape) return false;
        MediconDrawCommand d{sizeof(d),kind,{r.x*scale_,r.y*scale_,r.width*scale_,r.height*scale_},
            {end.x*scale_,end.y*scale_},{c.r,c.g,c.b,c.a},radius*scale_,width*scale_};
        return h.draw_shape(h.context,&d) > 0;
    }
    float scale_ = 1;
};
/// Published after on_render; context is borrowed until this callback returns.
struct RenderEvent { RenderContext& context; };

/// A typed reference to a host-owned, automatically persisted ClickGUI setting.
template<class T> class Setting {
public:
    /** @return The current value copied from ClickGUI/config. Callback thread only.
     * @code float r = radius.get(); @endcode */
    T get() const {
        MediconValue value{};
        if (!host_->read_setting(host_->context, detail::str(id_), &value)) throw std::runtime_error("Setting::get: " + id_);
        return detail::decode<T>(value);
    }
    /** @brief Set numeric slider bounds during construction only.
     * @param minimum Lower bound. @param maximum Upper bound. @param step Positive increment.
     * @return This setting for chaining. @code setting<float>("r","Radius",4).range(1,20,.5F); @endcode */
    Setting& range(float minimum, float maximum, float step = 1) requires (std::same_as<T, float> || std::same_as<T, int>) {
        if (!host_->setting_range(host_->context, detail::str(id_), minimum, maximum, step))
            throw std::invalid_argument("Setting::range: invalid bounds or lifecycle for " + id_);
        return *this;
    }
private:
    friend class Module;
    Setting(const MediconHostApi* host, std::string id) : host_(host), id_(std::move(id)) {}
    const MediconHostApi* host_;
    std::string id_;
};

/// Base class for one script module. Override callbacks as needed; exceptions disable only this script.
class Module {
public:
    /// Called by the SDK factory with a live host; never instantiate a script on another thread.
    Module() : host_(&detail::api()) {}
    virtual ~Module() = default;
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
    /// Runs once per OFF -> ON after settings restore. Register scoped listeners here.
    virtual void on_enable() {}
    /// Runs once per ON -> OFF; listeners are then removed automatically.
    virtual void on_disable() {}
    /// Runs on each enabled host tick. Use listen<TickEvent> for delta_seconds.
    virtual void on_tick() {}
    /// @param context Borrowed drawing facade, valid only until return.
    virtual void on_render(RenderContext& context) { (void)context; }
    /// @brief Interactive callback, enabled modules only while ClickGUI is visible. @param context Frame-scoped DIP UI.
    virtual void on_ui(UIContext& context) { (void)context; }
    /** @brief One-shot host timer, cancelled on disable/unload.
     * @param delay Nonnegative duration. @param callback Callback on the serialized host tick. @return Timer ID or 0. */
    template<class Rep, class Period, class Callback> uint64_t after(std::chrono::duration<Rep,Period> delay, Callback&& callback) {
        return detail::schedule_timer(this, std::chrono::duration<double>(delay).count(), 0, std::forward<Callback>(callback));
    }
    /** @brief Repeating timer; missed intervals are coalesced. @param interval Positive duration.
     * @param callback Host tick callback. @return Timer ID or 0; all are removed on disable. */
    template<class Rep, class Period, class Callback> uint64_t every(std::chrono::duration<Rep,Period> interval, Callback&& callback) {
        const double seconds = std::chrono::duration<double>(interval).count();
        if (!std::isfinite(seconds) || seconds <= 0) return 0;
        return detail::schedule_timer(this, seconds, seconds, std::forward<Callback>(callback));
    }
    /// @brief Cancel a timer owned by this module. @param id Returned by after/every. Callback thread only.
    void cancel_timer(uint64_t id) { auto& h = detail::api(); if (detail::extended() && h.timer_cancel) h.timer_cancel(h.context,id); }

    /** @brief Declare a setting during construction. IDs must be unique and stable.
     * @tparam T bool, int, float, Color, or std::string. @param id Persistence key.
     * @param label ClickGUI label. @param initial Default value. @return A typed setting handle.
     * @code Setting<float> radius = setting<float>("radius","Radius",4).range(1,20,.5F); @endcode */
    template<class T> Setting<T> setting(std::string_view id, std::string_view label, T initial) {
        MediconSettingDescriptor d{detail::str(id), detail::str(label), detail::encode(initial), -1000000, 1000000,
            std::same_as<T, float> ? .1F : 1.F};
        if (!host_->define_setting(host_->context, &d)) throw std::invalid_argument("Module::setting: invalid or duplicate ID " + std::string(id));
        return Setting<T>{host_, std::string(id)};
    }
    /** @brief Subscribe until disable/destruction. Call from on_enable.
     * @tparam Event TickEvent, RenderEvent, or KeyEvent. @param callback Callable taking Event const&.
     * @code listen<TickEvent>([](const TickEvent& e) { (void)e.delta_seconds; }); @endcode */
    template<class Event, class Callback> void listen(Callback&& callback) {
        constexpr uint32_t kind = [] {
            if constexpr (std::same_as<Event, TickEvent>) return MEDICON_EVENT_TICK;
            else if constexpr (std::same_as<Event, RenderEvent>) return MEDICON_EVENT_RENDER;
            else if constexpr (std::same_as<Event, GameJoined>) return MEDICON_EVENT_GAME_JOINED;
            else if constexpr (std::same_as<Event, GameLeft>) return MEDICON_EVENT_GAME_LEFT;
            else if constexpr (std::same_as<Event, ScreenResized>) return MEDICON_EVENT_SCREEN_RESIZED;
            else { static_assert(std::same_as<Event, KeyEvent> || std::same_as<Event, KeyPressed>, "Module::listen: unsupported event"); return MEDICON_EVENT_KEY; }
        }();
        listeners_.push_back({kind, [fn = std::forward<Callback>(callback)](const void* e) mutable {
            if constexpr (std::same_as<Event, KeyPressed>) { const auto& k = *static_cast<const MediconScriptKeyEvent*>(e); if (k.down) std::invoke(fn, KeyPressed{static_cast<Key>(k.key),k.repeat != 0}); }
            else if constexpr (std::same_as<Event, GameJoined> || std::same_as<Event, GameLeft>) std::invoke(fn, Event{static_cast<const MediconWorldEvent*>(e)->world_identity});
            else if constexpr (std::same_as<Event, ScreenResized>) { const auto& s = *static_cast<const MediconScreenEvent*>(e); std::invoke(fn, ScreenResized{s.width,s.height,s.scale}); }
            else std::invoke(fn, *static_cast<const Event*>(e));
        }});
    }
private:
    template<class T> friend struct detail::Factory;
    template<class Fn> friend int32_t detail::invoke(Module*, Fn&&) noexcept;
    // Implementation hooks never expose the host's opaque context to the public API.
    const MediconHostApi* sdk_host() const { return host_; }
    void sdk_clear_listeners() { listeners_.clear(); }
    void sdk_dispatch(uint32_t kind, const void* event) {
        const auto snapshot = listeners_;
        for (const auto& listener : snapshot) if (listener.kind == kind) listener.callback(event);
    }
private:
    struct Listener { uint32_t kind; std::function<void(const void*)> callback; };
    const MediconHostApi* host_;
    std::vector<Listener> listeners_;
};

namespace detail {
template<class Callback> uint64_t schedule_timer(Module* owner, double delay, double interval, Callback&& callback) {
    struct Handler {
        Module* owner; std::function<void()> callback;
        static int32_t MEDICON_SCRIPT_CALL fire(void* p) noexcept { auto& h = *static_cast<Handler*>(p); return invoke(h.owner,[&] { h.callback(); }); }
        static void MEDICON_SCRIPT_CALL destroy(void* p) noexcept { delete static_cast<Handler*>(p); }
    };
    auto& h = api(); if (!extended() || !h.timer_add) return 0;
    auto* handler = new Handler{owner,std::forward<Callback>(callback)};
    const auto id = h.timer_add(h.context,delay,interval,Handler::fire,Handler::destroy,handler);
    if (!id) delete handler;
    return id;
}
template<class Fn> int32_t invoke(Module* m, Fn&& fn) noexcept {
    HostScope scope(m->sdk_host());
    try { fn(); return 1; }
    catch (const std::exception& e) { log::error(e.what()); }
    catch (...) { log::error("Unhandled script exception"); }
    m->sdk_clear_listeners();
    return 0;
}
template<class T> struct Factory {
    static void* MEDICON_SCRIPT_CALL create(const MediconHostApi* host) noexcept {
        if (!host || host->size < MEDICON_HOST_V1_SIZE || host->abi_version != MEDICON_SCRIPT_ABI_VERSION ||
            !host->log || !host->chat || !host->define_setting || !host->read_setting || !host->setting_range ||
            !host->local_player || !host->render_size || !host->fill_circle || !host->text) return nullptr;
        HostScope scope(host);
        try { return new T; } catch (const std::exception& e) { log::error(e.what()); } catch (...) { log::error("Script construction failed"); }
        return nullptr;
    }
    static void MEDICON_SCRIPT_CALL destroy(void* p) noexcept {
        auto* m = static_cast<T*>(p); HostScope scope(m->sdk_host()); delete m;
    }
    static int32_t MEDICON_SCRIPT_CALL enable(void* p) noexcept { Module* m = static_cast<T*>(p); return invoke(m, [&] { m->on_enable(); }); }
    static int32_t MEDICON_SCRIPT_CALL disable(void* p) noexcept {
        Module* m = static_cast<T*>(p); auto r = invoke(m, [&] { m->on_disable(); }); m->sdk_clear_listeners(); return r;
    }
    static int32_t MEDICON_SCRIPT_CALL tick(void* p, float dt) noexcept {
        Module* m = static_cast<T*>(p); return invoke(m, [&] { m->on_tick(); TickEvent e{dt}; m->sdk_dispatch(MEDICON_EVENT_TICK, &e); });
    }
    static int32_t MEDICON_SCRIPT_CALL render(void* p) noexcept {
        Module* m = static_cast<T*>(p); return invoke(m, [&] { RenderContext c; m->on_render(c); RenderEvent e{c}; m->sdk_dispatch(MEDICON_EVENT_RENDER, &e); });
    }
    static int32_t MEDICON_SCRIPT_CALL event(void* p, uint32_t kind, const void* e) noexcept {
        auto* m = static_cast<T*>(p); return invoke(m, [&] { m->sdk_dispatch(kind, e); });
    }
    static int32_t MEDICON_SCRIPT_CALL ui(void* p) noexcept {
        Module* m = static_cast<T*>(p); return invoke(m,[&] { UIContext context; m->on_ui(context); });
    }
};
template<size_t I, size_t N> consteval MediconScriptManifest manifest(const char (&id)[I], const char (&name)[N], Category category) {
    static_assert(I > 1 && I <= 64, "MEDICON_MODULE: ID must be 1..63 bytes");
    static_assert(N > 1 && N <= 96, "MEDICON_MODULE: name must be 1..95 bytes");
    MediconScriptManifest m{}; m.magic = MEDICON_SCRIPT_MAGIC; m.size = sizeof(m);
    m.abi_version = sdk::abi_version; m.api_version = sdk::api_version; m.category = static_cast<uint32_t>(category);
    for (size_t i = 0; i < I; ++i) m.id[i] = id[i];
    for (size_t i = 0; i < N; ++i) m.name[i] = name[i];
    return m;
}
} // namespace detail
}} // namespace medicon::script_v1

/** @brief Register exactly one Module per script (one .cpp only in multi-file modules).
 * @param Type Module subclass. @param Id Stable unique literal ID.
 * @param Name ClickGUI display name literal. @param CategoryValue Classification metadata.
 * @code MEDICON_MODULE(HelloWorld,"hello","Hello World",medicon::Category::Utility); @endcode */
#define MEDICON_MODULE(Type, Id, Name, CategoryValue) \
extern "C" MEDICON_SCRIPT_EXPORT const MediconScriptManifest medicon_script_manifest = medicon::script_v1::detail::manifest(Id, Name, CategoryValue); \
extern "C" MEDICON_SCRIPT_EXPORT int32_t MEDICON_SCRIPT_CALL medicon_script_entry(uint32_t abi, MediconPluginApi* out) noexcept { \
    if (abi != MEDICON_SCRIPT_ABI_VERSION || !out || out->size < MEDICON_PLUGIN_V1_SIZE) return 0; \
    using F = medicon::script_v1::detail::Factory<Type>; \
    const auto capacity = out->size; const MediconPluginApi table{sizeof(MediconPluginApi), MEDICON_SCRIPT_ABI_VERSION, F::create, F::destroy, F::enable, F::disable, F::tick, F::render, F::event, F::ui}; \
    std::memcpy(out, &table, std::min<size_t>(capacity,sizeof(table))); out->size = static_cast<uint32_t>(std::min<size_t>(capacity,sizeof(table))); return 1; \
}
