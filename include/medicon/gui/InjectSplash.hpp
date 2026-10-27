#pragma once

#include <imgui.h>

#include <chrono>
#include <optional>

namespace medicon::gui {

class FontManager;

inline constexpr double SLIDE_IN_DURATION = 0.70;
inline constexpr double HOLD_DURATION = 0.90;
inline constexpr double COLLAPSE_DURATION = 0.40;
inline constexpr double INJECT_SPLASH_DURATION = SLIDE_IN_DURATION + HOLD_DURATION + COLLAPSE_DURATION;

// Every animated value is a pure function of elapsed seconds. No phase state or
// accumulated frame delta is used, including after a resize or a stalled frame.
struct InjectSplashFrame {
    bool finished = false;
    float slide = 0.0F;
    float text_scale = 1.0F;
    float text_alpha = 0.0F;
    ImVec2 text_shake{};
    float image_scale = 0.0F;
    float image_alpha = 0.0F;
    float scale = 1.0F;
    float alpha = 1.0F;
};

InjectSplashFrame evaluate_inject_splash(double elapsed) noexcept;
void draw_inject_splash(ImDrawList* list, const FontManager& fonts, ImVec2 display_size, double elapsed);

class InjectSplash {
public:
    // Arm at client startup; count time from the first rendered frame, after
    // font uploads and renderer initialization have finished.
    void start() noexcept;
    bool active() const noexcept;
    void render(ImDrawList* list, const FontManager& fonts, ImVec2 display_size);

private:
    double elapsed() const noexcept;
    bool armed_ = false;
    std::optional<std::chrono::steady_clock::time_point> start_time_;
};

} // namespace medicon::gui
