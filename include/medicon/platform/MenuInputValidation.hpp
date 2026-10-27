#pragma once

#ifdef _WIN32
#include <Windows.h>

namespace medicon::platform {

// Only position/cursor updates may reuse the state published by Present. Keep
// the default conservative: clicks, keys, wheels, focus and undecoded raw input
// must validate the current screen before taking an input-capture snapshot.
[[nodiscard]] inline bool menu_input_requires_screen_validation(
    UINT message, const RAWINPUT* decoded = nullptr) noexcept {
    if (message == WM_MOUSEMOVE || message == WM_NCMOUSEMOVE || message == WM_SETCURSOR) return false;
    if (message == WM_INPUT && decoded && decoded->header.dwType == RIM_TYPEMOUSE &&
        decoded->data.mouse.usButtonFlags == 0) return false;
    return true;
}

} // namespace medicon::platform
#endif
