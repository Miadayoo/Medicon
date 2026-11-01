#pragma once
/** @file medicon.hpp
 * @brief Medicon Script SDK 0.1 (C++20). Include this header in scripts only.
 * @details All APIs run on the serialized host callback thread, except rendering,
 * which is valid only in on_render/RenderEvent. Never retain a RenderContext or
 * call this SDK from a user-created thread. The C ABI owns no STL objects.
 * @example HelloWorld.cpp
 */
#include <medicon/abi.h>
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

namespace medicon { inline namespace script_v1 {
/// Script classification metadata. ClickGUI displays every script in Scripts.
enum class Category { Visual, Movement, Player, Utility, Client };
/// Normalized RGBA channels in [0,1]. Example: Color{1,0,0,1}.
struct Color { float r = 1, g = 1, b = 1, a = 1; };
/// A screen position in physical pixels; origin is the top-left corner.
using Vec2 = MediconPoint;
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
/// @return Public API version (100 = 0.1.0); usable on any thread.
inline constexpr uint32_t api_version = MEDICON_SCRIPT_API_VERSION;
/// @return C ABI version; usable on any thread.
inline constexpr uint32_t abi_version = MEDICON_SCRIPT_ABI_VERSION;
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

/// Drawing facade. Borrow only within on_render or a RenderEvent callback.
class RenderContext {
public:
    /** @return Screen center in pixels. Render callback only.
     * @code auto c = context.center(); @endcode */
    Vec2 center() const { auto& h = detail::api(); auto p = h.render_size(h.context); return {p.x * .5F, p.y * .5F}; }
    /** @brief Draw a filled circle. @param position Center in pixels.
     * @param radius Positive radius in pixels. @param color Normalized RGBA.
     * @code context.fill_circle(context.center(), 4, Color{1,0,0,1}); @endcode */
    void fill_circle(Vec2 position, float radius, Color color = {}) const {
        auto& h = detail::api(); h.fill_circle(h.context, position, radius, {color.r,color.g,color.b,color.a});
    }
    /** @brief Draw UTF-8 text. @param position Top-left pixel position.
     * @param message Text to draw. @param color Normalized RGBA. @param size Font size in pixels.
     * @code context.text({10,10}, "HUD", Color{}, 18); @endcode Render callback only. */
    void text(Vec2 position, std::string_view message, Color color = {}, float size = 16) const {
        auto& h = detail::api(); h.text(h.context, position, detail::str(message), {color.r,color.g,color.b,color.a}, size);
    }
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
            else { static_assert(std::same_as<Event, KeyEvent>, "Module::listen: unsupported event"); return MEDICON_EVENT_KEY; }
        }();
        listeners_.push_back({kind, [fn = std::forward<Callback>(callback)](const void* e) mutable { std::invoke(fn, *static_cast<const Event*>(e)); }});
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
        if (!host || host->size != sizeof(MediconHostApi) || host->abi_version != MEDICON_SCRIPT_ABI_VERSION ||
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
    if (abi != MEDICON_SCRIPT_ABI_VERSION || !out || out->size != sizeof(MediconPluginApi)) return 0; \
    using F = medicon::script_v1::detail::Factory<Type>; \
    *out = {sizeof(MediconPluginApi), MEDICON_SCRIPT_ABI_VERSION, F::create, F::destroy, F::enable, F::disable, F::tick, F::render, F::event}; return 1; \
}
