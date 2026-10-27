#include <medicon/gui/Ime.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <imm.h>

#include <cmath>
#include <utility>

namespace medicon::gui {
namespace {

HWND as_window(ImeBridge::Window window) { return static_cast<HWND>(window); }

} // namespace

ImeBridge::ImeBridge() : wake_(RegisterWindowMessageW(L"MediconSocialImeSync")) {}

void ImeBridge::set_window(Window window) noexcept {
    std::lock_guard lock(mutex_);
    window_ = window;
}

void ImeBridge::set_active(bool active, ImVec2 caret, float line_height) {
    Window post = nullptr;
    {
        std::lock_guard lock(mutex_);
        const bool changed = want_ != active;
        want_ = active;
        caret_ = caret;
        line_height_ = line_height;
        // The window thread applies the change; a posted message wakes it.
        if (changed) post = window_;
    }
    if (post && wake_) PostMessageW(as_window(post), wake_, 0, 0);
}

ImeBridge::Composition ImeBridge::composition() const {
    std::lock_guard lock(mutex_);
    return composition_;
}

std::wstring ImeBridge::take_result() {
    std::lock_guard lock(mutex_);
    return std::exchange(result_, {});
}

void ImeBridge::sync(Window handle) {
    const HWND window = as_window(handle);
    bool want = false;
    {
        std::lock_guard lock(mutex_);
        want = want_;
        if (want == applied_) return;
        applied_ = want;
        if (!want) composition_ = {};
    }
    if (want) {
        // Games often detach the IME from their window; give it the default one.
        HIMC current = ImmGetContext(window);
        game_had_context_ = current != nullptr;
        if (current) ImmReleaseContext(window, current);
        ImmAssociateContextEx(window, nullptr, IACE_DEFAULT);
    } else {
        if (HIMC context = ImmGetContext(window)) {
            ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
            ImmReleaseContext(window, context);
        }
        if (!game_had_context_) ImmAssociateContext(window, nullptr);
    }
}

void ImeBridge::place_windows(void* handle) {
    const HIMC context = static_cast<HIMC>(handle);
    ImVec2 caret;
    float line = 0.0F;
    {
        std::lock_guard lock(mutex_);
        caret = caret_;
        line = line_height_;
    }
    const POINT top{static_cast<LONG>(std::lround(caret.x)), static_cast<LONG>(std::lround(caret.y))};
    const LONG bottom = static_cast<LONG>(std::lround(caret.y + line));
    COMPOSITIONFORM composition{CFS_POINT, top, {}};
    ImmSetCompositionWindow(context, &composition);
    // Candidates open just below the caret line and never cover it.
    CANDIDATEFORM candidate{0, CFS_EXCLUDE, {top.x, bottom}, {top.x, top.y, top.x + 1, bottom}};
    ImmSetCandidateWindow(context, &candidate);
}

bool ImeBridge::window_message(Window handle, unsigned message, WParam wparam, LParam lparam, LParam& result) {
    const HWND window = as_window(handle);
    if (wake_ && message == wake_) {
        sync(handle);
        result = 0;
        return true;
    }
    if (message < WM_IME_STARTCOMPOSITION || (message > WM_IME_COMPOSITION && message < WM_IME_SETCONTEXT) ||
        message > WM_IME_KEYUP)
        return false;
    sync(handle);
    {
        std::lock_guard lock(mutex_);
        if (!applied_) return false;
    }
    switch (message) {
    case WM_IME_SETCONTEXT:
        // The composition is drawn in the input itself; keep the candidate list.
        result = DefWindowProcW(window, message, wparam, lparam & ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW));
        return true;
    case WM_IME_STARTCOMPOSITION:
        if (HIMC context = ImmGetContext(window)) {
            place_windows(context);
            ImmReleaseContext(window, context);
        }
        result = 0;
        return true;
    case WM_IME_COMPOSITION: {
        HIMC context = ImmGetContext(window);
        if (!context) {
            result = 0;
            return true;
        }
        const auto read = [&](DWORD kind) {
            const LONG bytes = ImmGetCompositionStringW(context, kind, nullptr, 0);
            std::wstring text(bytes > 0 ? static_cast<std::size_t>(bytes) / sizeof(wchar_t) : 0, L'\0');
            if (bytes > 0) ImmGetCompositionStringW(context, kind, text.data(), static_cast<DWORD>(bytes));
            return text;
        };
        std::wstring confirmed = (lparam & GCS_RESULTSTR) ? read(GCS_RESULTSTR) : std::wstring{};
        Composition composing;
        if (lparam & GCS_COMPSTR) {
            composing.text = read(GCS_COMPSTR);
            const LONG cursor = (lparam & GCS_CURSORPOS) ? ImmGetCompositionStringW(context, GCS_CURSORPOS, nullptr, 0) : -1;
            composing.cursor = cursor >= 0 ? static_cast<int>(cursor) : static_cast<int>(composing.text.size());
        }
        place_windows(context);
        ImmReleaseContext(window, context);
        {
            std::lock_guard lock(mutex_);
            result_ += confirmed;
            composition_ = std::move(composing);
        }
        // Not passed on: the default handling would also send the text as WM_IME_CHAR.
        result = 0;
        return true;
    }
    case WM_IME_ENDCOMPOSITION: {
        std::lock_guard lock(mutex_);
        composition_ = {};
        result = 0;
        return true;
    }
    case WM_IME_CHAR:
        result = 0; // already taken from GCS_RESULTSTR
        return true;
    default:
        // Candidate list, mode changes and the IME's own notifications.
        result = DefWindowProcW(window, message, wparam, lparam);
        return true;
    }
}

} // namespace medicon::gui
