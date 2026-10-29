#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

namespace medicon::platform {

// Default game movement remains available in ClickGUI, unless a text field or
// keybind editor owns the keyboard. Social and the title menu remain modal.
[[nodiscard]] constexpr bool clickgui_movement_key(int key) noexcept {
    switch (key) {
    case 'W': case 'A': case 'S': case 'D':
    case 0x20: // Space
    case 0x10: case 0xA0: case 0xA1: // Shift, left/right Shift
    case 0x11: case 0xA2: case 0xA3: // Ctrl, left/right Ctrl
        return true;
    default:
        return false;
    }
}

// Remember the owner of a complete press, including repeats/up after a screen
// closes. An up for a down already delivered to the game must still reach it.
template<std::size_t Count> class InputPressOwnership {
public:
    bool consume(std::size_t key, bool down, bool blocked) noexcept {
        if (key >= Count) return blocked;
        const bool owned = owned_[key];
        if (!down) {
            const bool game_held = game_down_[key];
            owned_[key] = game_down_[key] = false;
            return !game_held && (blocked || owned);
        }
        if (blocked || owned) {
            if (!game_down_[key]) owned_[key] = true;
            return true;
        }
        game_down_[key] = true;
        return false;
    }

    [[nodiscard]] bool active() const noexcept {
        return std::any_of(owned_.begin(), owned_.end(), [](bool owned) { return owned; });
    }
    void reset() noexcept { owned_.fill(false); game_down_.fill(false); }

private:
    std::array<bool, Count> owned_{};
    std::array<bool, Count> game_down_{};
};

} // namespace medicon::platform

#ifdef _WIN32
#include <Windows.h>

namespace medicon::platform {

// GetRawInputData/GetRawInputBuffer can be used before the game's WndProc.
// Keep a valid stationary relative packet; zero absolute coordinates would
// otherwise turn a swallowed tablet/absolute packet into a camera jump.
inline void suppress_raw_mouse(RAWINPUT& raw) noexcept {
    if (raw.header.dwType == RIM_TYPEMOUSE) raw.data.mouse = {};
}

[[nodiscard]] constexpr unsigned mouse_button_mask(int key) noexcept {
    switch (key) {
    case VK_LBUTTON: return 1U;
    case VK_RBUTTON: return 2U;
    case VK_MBUTTON: return 4U;
    case VK_XBUTTON1: return 8U;
    case VK_XBUTTON2: return 16U;
    default: return 0;
    }
}

} // namespace medicon::platform
#endif
