#pragma once

#include <imgui.h>

#include <cstdint>
#include <mutex>
#include <string>

namespace medicon::gui {

// Japanese IME for Social text inputs (spec 4.8), between two threads:
// - the render thread says when a Social input has the keyboard and where its
//   caret is, takes the confirmed text, and draws the composition itself;
// - the window thread feeds it the game window's WM_IME_* messages.
// While an input is focused the IME is enabled on the window even if the game
// turned it off, and restored afterwards. Never touches game objects.
// Win32 handles and message parameters are passed as integers so this header
// does not bring <Windows.h> (and its macros) into every GUI file.
class ImeBridge {
public:
    using Window = void*;               // HWND
    using WParam = std::uintptr_t;      // WPARAM
    using LParam = std::intptr_t;       // LPARAM / LRESULT

    ImeBridge();
    ImeBridge(const ImeBridge&) = delete;
    ImeBridge& operator=(const ImeBridge&) = delete;

    // --- Render thread ---
    // `caret` is the caret's top in client pixels; candidates open below it.
    void set_active(bool active, ImVec2 caret, float line_height);
    struct Composition {
        std::wstring text; // empty: not composing
        int cursor = 0;    // UTF-16 index inside text
    };
    Composition composition() const;
    // Text the IME confirmed since the last call.
    std::wstring take_result();

    // --- Window thread ---
    // Handles the message while an input is focused (and the bridge's own wake
    // message). Returns true when `result` is the answer and nothing else may
    // see the message; false leaves it to the caller unchanged.
    bool window_message(Window window, unsigned message, WParam wparam, LParam lparam, LParam& result);
    // The window the bridge posts its wake message to.
    void set_window(Window window) noexcept;

private:
    void sync(Window window);           // window thread: applies want_ to the window
    void place_windows(void* context);  // HIMC

    mutable std::mutex mutex_;
    Window window_ = nullptr;
    unsigned wake_ = 0;
    // Render side.
    bool want_ = false;
    ImVec2 caret_{};
    float line_height_ = 0.0F;
    // Window side.
    bool applied_ = false;
    bool game_had_context_ = true; // the game's own IME state, restored on deactivate
    Composition composition_;
    std::wstring result_;
};

} // namespace medicon::gui
