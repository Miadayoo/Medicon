#pragma once

#include <cstdint>

namespace medicon::platform {

// One notch. High-resolution devices send fractions of this in a single message.
inline constexpr int wheel_delta_unit = 120;
// A real wheel or trackpad message stays within a few notches. Relative mouse
// motion stuffed into WM_MOUSEWHEEL is commonly thousands of units per packet.
inline constexpr int max_genuine_wheel_delta = 20 * wheel_delta_unit;

inline constexpr unsigned short raw_mouse_wheel_flag = 0x0400;   // RI_MOUSE_WHEEL
inline constexpr unsigned short raw_mouse_hwheel_flag = 0x0800;  // RI_MOUSE_HWHEEL

struct RawMouseWheel {
    bool vertical = false;
    bool horizontal = false;
    std::int16_t vertical_delta = 0;
    std::int16_t horizontal_delta = 0;
};

// Movement-only raw packets must not become ClickGUI scrolling. Only the wheel
// button flags are forwarded, and a zero delta is not a scroll.
inline RawMouseWheel wheel_from_raw_mouse(unsigned short button_flags, unsigned short button_data) noexcept {
    RawMouseWheel wheel;
    const auto delta = static_cast<std::int16_t>(button_data);
    if ((button_flags & raw_mouse_wheel_flag) != 0 && delta != 0) {
        wheel.vertical = true;
        wheel.vertical_delta = delta;
    }
    if ((button_flags & raw_mouse_hwheel_flag) != 0 && delta != 0) {
        wheel.horizontal = true;
        wheel.horizontal_delta = delta;
    }
    return wheel;
}

inline bool genuine_legacy_wheel_delta(std::int16_t delta) noexcept {
    return delta != 0 && delta >= -max_genuine_wheel_delta && delta <= max_genuine_wheel_delta;
}

} // namespace medicon::platform

