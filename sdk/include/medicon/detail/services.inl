// Included inside medicon::script_v1 by medicon.hpp. Public users include medicon.hpp only.
namespace detail {
inline bool extended() { return api().size >= sizeof(MediconHostApi); }
inline MediconFrameInfo frame() {
    MediconFrameInfo f{}; f.size = sizeof(f); f.scale = 1;
    auto& h = api();
    if (extended() && h.frame_info && h.frame_info(h.context,&f)) return f;
    const auto s = h.render_size(h.context); f.width = s.x; f.height = s.y; return f;
}
inline MediconCallSite site(std::source_location location) { return {str(location.file_name()),location.line(),location.column()}; }
template<class Callback> uint64_t schedule_timer(Module*,double,double,Callback&&);
}
namespace feature {
/// Effective per-host/per-device capabilities, never a sandbox or permission boundary.
enum class Capability : uint64_t {
    Glass = MEDICON_FEATURE_GLASS, Refraction = MEDICON_FEATURE_REFRACTION, UI = MEDICON_FEATURE_UI,
    Animation = MEDICON_FEATURE_ANIMATION, Notifications = MEDICON_FEATURE_NOTIFICATIONS, Storage = MEDICON_FEATURE_STORAGE,
    Timers = MEDICON_FEATURE_TIMERS, Theme = MEDICON_FEATURE_THEME, Input = MEDICON_FEATURE_INPUT, Sound = MEDICON_FEATURE_SOUND,
    GlassRadius = MEDICON_FEATURE_GLASS_RADIUS, GlassOpacity = MEDICON_FEATURE_GLASS_OPACITY,
    GlassBlur = 1ull << 15, GlassTint = 1ull << 16, GlassQuality = 1ull << 17
};
/// @param capability Effective support to test. @return false when absent. Callback thread only.
inline bool available(Capability capability) { return (detail::api().features & static_cast<uint64_t>(capability)) != 0; }
}
namespace clock {
/// @return The clamped host tick delta, in seconds. Callback thread only.
inline float delta_seconds() { return detail::frame().delta_seconds; }
/// @return Monotonic host seconds (not a wall-clock timestamp). Callback thread only.
inline double now_seconds() { return detail::frame().now_seconds; }
}
namespace theme {
enum class Token : uint32_t { TextPrimary, TextSecondary, Accent, Surface, Outline };
/// @param token Existing Medicon theme token. @return Copied RGBA color. Callback thread only.
inline Color color(Token token) {
    auto& h = detail::api(); MediconColor c{1,1,1,1};
    if (detail::extended() && h.theme_color) h.theme_color(h.context,static_cast<uint32_t>(token),&c);
    return {c.r,c.g,c.b,c.a};
}
/// @return The host's effective reduced-motion preference, shared with its UI.
inline bool reduced_motion() { return (detail::frame().flags & MEDICON_FRAME_REDUCED_MOTION) != 0; }
/// @return The shared reduced-transparency preference.
inline bool reduced_transparency() { return (detail::frame().flags & MEDICON_FRAME_REDUCED_TRANSPARENCY) != 0; }
/// @return The host's high-contrast preference.
inline bool high_contrast() { return (detail::frame().flags & MEDICON_FRAME_HIGH_CONTRAST) != 0; }
}
namespace detail {
inline float ease(float p, Easing e) {
    p = std::clamp(p,0.F,1.F);
    switch(e) {
    case Easing::OutCubic: return 1-(1-p)*(1-p)*(1-p);
    case Easing::OutExpo: return p >= 1 ? 1 : p <= 0 ? 0 : 1-std::exp2(-10*p);
    case Easing::InOutCubic: return p < .5F ? 4*p*p*p : 1-std::pow(-2*p+2,3.F)*.5F;
    default: return p;
    }
}
template<class T> T interpolate(T a, T b, float p) {
    if constexpr (std::is_floating_point_v<T>) return a+(b-a)*p;
    else if constexpr (std::same_as<T,Vec2>) return {a.x+(b.x-a.x)*p,a.y+(b.y-a.y)*p};
    else { static_assert(std::same_as<T,Color>,"Tween<T> supports float, double, Vec2, Color");
        return {a.r+(b.r-a.r)*p,a.g+(b.g-a.g)*p,a.b+(b.b-a.b)*p,a.a+(b.a-a.a)*p}; }
}
}
/** @brief Clock-driven value with continuous interruption. No callback or GPU resource is retained.
 * @tparam T float, double, Vec2, or Color. Access in host callbacks; disabled modules do not execute it. */
template<class T> class Tween {
public:
    /// @param initial Initial owned value. Construction performs no host calls.
    explicit Tween(T initial = {}) : from_(initial), target_(initial) {}
    /// @return Value sampled against the monotonic host clock. Reduced motion snaps to the target.
    T value() const {
        if (duration_ <= 0 || theme::reduced_motion()) return target_;
        const auto p = static_cast<float>((clock::now_seconds()-start_) / duration_);
        return detail::interpolate(from_,target_,detail::ease(p,easing_));
    }
    /** @param target New destination. @param duration Transition time. @param easing Interpolation curve.
     * @code opacity.to(1.F, std::chrono::milliseconds(260), Easing::OutCubic); @endcode */
    template<class Rep,class Period> void to(T target,std::chrono::duration<Rep,Period> duration,Easing easing = Easing::OutCubic) {
        from_ = value(); target_ = target; start_ = clock::now_seconds();
        duration_ = std::chrono::duration<double>(duration).count(); easing_ = easing;
        if (!std::isfinite(duration_) || duration_ < 0 || theme::reduced_motion() || !feature::available(feature::Capability::Animation)) duration_ = 0;
    }
private:
    T from_,target_;
    double start_ = 0, duration_ = 0;
    Easing easing_ = Easing::OutCubic;
};
namespace input {
/// @return Logical-DIP cursor in on_ui, or {-1,-1} when input is not available to this UI.
inline Vec2 cursor_position() {
    auto& h = detail::api(); MediconPoint p{-1,-1}; if (detail::extended() && h.input) h.input(h.context,-1,&p); return p;
}
/// @param key Stable key enum. @return Held state only in a permitted on_ui callback.
inline bool key_down(Key key) { auto& h = detail::api(); return detail::extended() && h.input && h.input(h.context,static_cast<int>(key),nullptr) > 0; }
}
namespace notification {
enum class Level : uint32_t { Info, Success, Warning, Error };
/// @brief Submit to existing Medicon Toasts. @param title Title. @param body Message. @param level Severity.
/// @return true if queued; false for host preferences, limits, or unavailable service. Callback thread only.
inline bool show(std::string_view title,std::string_view body,Level level = Level::Info) {
    auto& h = detail::api(); return detail::extended() && h.notify && h.notify(h.context,detail::str(title),detail::str(body),static_cast<uint32_t>(level)) > 0;
}
}
namespace sound {
enum class Builtin : uint32_t { Click, Success, Error };
/// @param sound Built-in sound. Currently only Success maps to the existing chime; others diagnose unsupported.
/// @return true if accepted by the host's sound preferences/rate limit. Callback thread only.
inline bool play_ui(Builtin sound) { auto& h = detail::api(); return detail::extended() && h.play_sound && h.play_sound(h.context,static_cast<uint32_t>(sound)) > 0; }
}
namespace storage {
/// @param key Module-private logical key, never a filesystem path. @return Copied UTF-8 value, or nullopt.
inline std::optional<std::string> get(std::string_view key) {
    auto& h = detail::api(); if (!detail::extended() || !h.storage_get) return std::nullopt;
    const auto size = h.storage_get(h.context,detail::str(key),nullptr,0);
    if (size < 0 || size > 256*1024) return std::nullopt;
    std::string value(static_cast<size_t>(size),'\0');
    if (h.storage_get(h.context,detail::str(key),value.data(),static_cast<uint32_t>(value.size())) != size) return std::nullopt;
    return value;
}
/// @param key Private key. @param value UTF-8 value. @return true if queued for atomic persistence within 256 KiB quota.
inline bool set(std::string_view key,std::string_view value) { auto& h = detail::api(); return detail::extended() && h.storage_set && h.storage_set(h.context,detail::str(key),detail::str(value)) > 0; }
}
class UIContext;
/** @brief Movable frame-scoped UI panel. Never store it beyond on_ui.
 * Background uses the existing Medicon glass; widgets and focus use ImGui. */
class Panel {
public:
    ~Panel() { end(); }
    Panel(const Panel&) = delete; Panel& operator=(const Panel&) = delete;
    Panel(Panel&& other) noexcept : host_(other.host_),token_(std::exchange(other.token_,0)) {}
    Panel& operator=(Panel&& other) noexcept { if (this != &other) { end(); host_=other.host_;token_=std::exchange(other.token_,0); } return *this; }
    /// @param text UTF-8 content. Valid only in this on_ui scope.
    void label(std::string_view text) { widget(1,{},text,0); }
    /// @param id Stable widget ID. @param text Label. @return true once for a click or keyboard activation.
    bool button(std::string_view id,std::string_view text) { return widget(2,id,text,0) > 0; }
    /// @param id Stable widget ID. @param value Finite progress in [0,1].
    void progress(std::string_view id,float value) { widget(3,id,{},value); }
private:
    friend class UIContext;
    Panel(const MediconHostApi* host,uint32_t token) : host_(host),token_(token) {}
    int32_t widget(uint32_t kind,std::string_view id,std::string_view text,float value) {
        if (!token_ || detail::host != host_) return 0;
        MediconWidget w{sizeof(w),kind,detail::str(id),detail::str(text),value}; return host_->ui_widget(host_->context,token_,&w);
    }
    void end() noexcept { if (token_ && detail::host == host_) host_->ui_end(host_->context,token_); token_ = 0; }
    const MediconHostApi* host_; uint32_t token_;
};
/// Immediate-mode logical-DIP UI, provided only while the enabled module may interact with ClickGUI.
class UIContext {
public:
    /// @return Logical display size, using the host's current scale.
    Size viewport_size() const { auto f = detail::frame(); return {f.width/std::max(.01F,f.scale),f.height/std::max(.01F,f.scale)}; }
    /** @param id Stable panel ID. @param rect Logical-DIP bounds. @param style Glass descriptor.
     * @param source Automatic diagnostic source. @return RAII panel, valid only until this on_ui returns. */
    Panel glass_panel(std::string_view id,Rect rect,const Glass& style = Glass::regular(),std::source_location source = std::source_location::current()) {
        auto& h = detail::api(); if (!detail::extended() || !h.ui_begin || !h.ui_end || !h.ui_widget) return Panel(&h,0);
        MediconRect r{rect.x,rect.y,rect.width,rect.height}; auto d = style.descriptor(); const auto callsite = detail::site(source);
        return Panel(&h,h.ui_begin(h.context,detail::str(id),&r,&d,&callsite));
    }
};
