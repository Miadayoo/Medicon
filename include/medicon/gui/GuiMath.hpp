#pragma once

// Backend-independent GUI math: scaling, easing, animation state and slider
// value mapping. No ImGui dependency so it can be unit tested directly.

#include "medicon/gui/Theme.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace medicon::gui {

// --- Scaling (spec 3.1) -----------------------------------------------------

inline float auto_scale(float client_w, float client_h, float dpi) {
    const float res_scale = std::min(client_w / theme::metrics::reference_width,
                                     client_h / theme::metrics::reference_height);
    const float dpi_scale = dpi / 96.0F;
    return std::max(res_scale, dpi_scale);
}

inline float clamp_scale(float scale) {
    return std::clamp(scale, theme::metrics::min_scale, theme::metrics::max_scale);
}

inline float ui_scale(float client_w, float client_h, float dpi, float gui_scale_setting) {
    return clamp_scale(auto_scale(client_w, client_h, dpi) * gui_scale_setting);
}

// Edge coordinate in physical px for an rpx offset.
inline float px(float rpx, float scale) { return std::round(rpx * scale); }
// Line thickness: never thinner than one physical pixel.
inline float line_px(float rpx, float scale) { return std::max(1.0F, std::round(rpx * scale)); }

// --- Easing and animation (spec 3.5) ------------------------------------------

inline float ease_out_cubic(float t) {
    t = std::clamp(t, 0.0F, 1.0F);
    const float inverse = 1.0F - t;
    return 1.0F - inverse * inverse * inverse;
}

// Overshoots by about 10 % at c1 = 1.7 and 4 % at c1 = 1.0, then settles at 1.
// Not clamped: x in [0, 1].
inline float ease_out_back(float x, float c1) {
    const float u = x - 1.0F;
    return 1.0F + (c1 + 1.0F) * u * u * u + c1 * u * u;
}

inline float clamp_dt(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) return 0.0F;
    return static_cast<float>(std::min(seconds, static_cast<double>(theme::timing::max_dt)));
}

// Single progress value for open/close animations. Reversing mid-way never
// resets progress, so the displayed value is continuous.
struct Reversible {
    float p = 0.0F;

    void step(bool forward, float dt, float open_seconds, float close_seconds) {
        if (forward) p += open_seconds > 0.0F ? dt / open_seconds : 1.0F;
        else p -= close_seconds > 0.0F ? dt / close_seconds : 1.0F;
        p = std::clamp(p, 0.0F, 1.0F);
    }
    void snap(bool forward) { p = forward ? 1.0F : 0.0F; }
    float value() const { return ease_out_cubic(p); }
    bool settled(bool forward) const { return forward ? p >= 1.0F : p <= 0.0F; }
};

// Linear tween toward 0 or 1 (hover/enabled colours).
inline float tween(float current, bool target, float dt, float seconds) {
    const float delta = seconds > 0.0F ? dt / seconds : 1.0F;
    return target ? std::min(1.0F, current + delta) : std::max(0.0F, current - delta);
}

// Exponential follow: x += (target - x) * (1 - exp(-dt / tau)).
inline float follow(float current, float target, float dt, float tau) {
    if (tau <= 0.0F) return target;
    const float next = current + (target - current) * (1.0F - std::exp(-dt / tau));
    return std::abs(target - next) < 0.01F ? target : next;
}

// Damped oscillator, solved exactly each step so the motion does not depend on
// the frame rate. A damping ratio below 1 (required) overshoots and rings.
struct Spring {
    float value = 0.0F;
    float velocity = 0.0F;

    void step(float target, float stiffness, float damping_ratio, float dt) {
        const float omega = std::sqrt(stiffness);
        const float decay = damping_ratio * omega;
        const float ringing = omega * std::sqrt(1.0F - damping_ratio * damping_ratio);
        const float offset = value - target;
        const float fade = std::exp(-decay * dt);
        const float c = std::cos(ringing * dt);
        const float s = std::sin(ringing * dt);
        value = target + fade * (offset * c + (velocity + decay * offset) / ringing * s);
        velocity = fade * (velocity * c - (decay * velocity + stiffness * offset) / ringing * s);
    }
};

// --- Slider (spec 4.5.2) ------------------------------------------------------

inline int float_precision(float step) {
    if (!(step > 0.0F) || !std::isfinite(step)) return 2;
    return std::max(0, -static_cast<int>(std::floor(std::log10(static_cast<double>(step)) + 1e-6)));
}

inline double round_to_digits(double value, int digits) {
    const double factor = std::pow(10.0, digits);
    return std::round(value * factor) / factor;
}

// Maps a cursor position on a track to a stepped, clamped value.
inline double slider_value(float mouse_x, float track_left, float track_right, double minimum, double maximum,
                           double step, bool integer, int digits) {
    const float span = track_right - track_left;
    const double t = span > 0.0F ? std::clamp(static_cast<double>((mouse_x - track_left) / span), 0.0, 1.0) : 0.0;
    double value = minimum + t * (maximum - minimum);
    if (step > 0.0) value = minimum + std::round((value - minimum) / step) * step;
    value = std::clamp(value, minimum, maximum);
    if (integer) {
        value = std::round(value);
        // Rounding can step outside fractional bounds; stay on an in-range integer.
        if (value > maximum) value = std::floor(maximum);
        if (value < minimum) value = std::ceil(minimum);
    } else {
        value = std::clamp(round_to_digits(value, digits), minimum, maximum);
    }
    return value;
}

inline float slider_fraction(double value, double minimum, double maximum) {
    if (!(maximum > minimum)) return 0.0F;
    return static_cast<float>(std::clamp((value - minimum) / (maximum - minimum), 0.0, 1.0));
}

inline std::string format_number(double value, int digits) {
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.*f", std::max(0, digits), value);
    std::string text = buffer;
    if (text == "-0" || text.rfind("-0.", 0) == 0) {
        // Avoid "-0.0" after rounding tiny negatives.
        bool all_zero = true;
        for (char c : text) if (c >= '1' && c <= '9') all_zero = false;
        if (all_zero) text.erase(0, 1);
    }
    return text;
}

// --- Panel placement (spec 4.4 / 4.7) ----------------------------------------

struct PanelPoint {
    float x = 0.0F;
    float y = 0.0F;
};

// Display-time clamp in physical px: the whole header and keep_visible_x rpx
// horizontally must stay inside the client area. Never written back to config.
inline PanelPoint clamp_panel_px(PanelPoint position, float panel_w_px, float header_h_px, float keep_px,
                                 float client_w, float client_h) {
    const float min_x = -(panel_w_px - keep_px);
    const float max_x = client_w - keep_px;
    position.x = std::clamp(position.x, std::min(min_x, max_x), std::max(min_x, max_x));
    position.y = std::clamp(position.y, 0.0F, std::max(0.0F, client_h - header_h_px));
    return position;
}

// Initial layout for a category without saved state (rpx). Wraps to the next
// row when the panel would cross the right edge of the client area.
inline PanelPoint initial_panel_position(int index, float client_w_rpx) {
    using namespace theme::metrics;
    PanelPoint point{start_x, start_y};
    for (int i = 0; i < index; ++i) {
        point.x += panel_width + panel_gap;
        if (point.x + panel_width > client_w_rpx) {
            point.x = start_x;
            point.y += panel_wrap_step_y;
        }
    }
    return point;
}

} // namespace medicon::gui
