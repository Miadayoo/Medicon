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
#include <cstdio>
#include <utility>

namespace medicon::gui {
namespace {

HWND as_window(ImeBridge::Window window) { return static_cast<HWND>(window); }

// Wake message parameters.
constexpr WPARAM wake_sync = 0;
constexpr WPARAM wake_toggle = 1;

// While an input is focused the window thread re-checks that the game has not
// taken the IME away again (it does on its own focus changes).
constexpr auto refresh_interval = std::chrono::milliseconds(250);

// The IME's own switch fires IMN_SETOPENSTATUS while the key is still being
// taken from the queue, just before the key itself is dispatched.
constexpr std::uint64_t handled_window_ms = 100;

// 半角/全角 (it alternates between two codes), 漢字, and Alt+` on a US layout.
bool toggle_key(unsigned message, WPARAM key) {
    if (key == VK_KANJI || key == VK_OEM_AUTO || key == VK_OEM_ENLW) return true;
    return message == WM_SYSKEYDOWN && key == VK_OEM_3;
}

} // namespace

ImeBridge::ImeBridge() : wake_(RegisterWindowMessageW(L"MediconSocialImeSync")) {}

void ImeBridge::set_window(Window window) noexcept {
    std::lock_guard lock(mutex_);
    window_ = window;
}

void ImeBridge::set_reporter(std::function<void(std::string_view)> report) { report_ = std::move(report); }

void ImeBridge::report(std::string_view message) const {
    if (report_) report_(message);
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
        const auto now = std::chrono::steady_clock::now();
        if (changed || (active && now - last_wake_ >= refresh_interval)) {
            post = window_;
            last_wake_ = now;
        }
    }
    if (post && wake_) PostMessageW(as_window(post), wake_, wake_sync, 0);
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
    bool want = false, was = false;
    {
        std::lock_guard lock(mutex_);
        want = want_;
        was = std::exchange(applied_, want);
        if (!want) composition_ = {};
    }
    if (want == was) {
        if (want) ensure_context(handle);
        return;
    }
    if (want) {
        // Games often detach the IME from their window; give it the default one.
        HIMC current = ImmGetContext(window);
        game_had_context_ = current != nullptr;
        if (current) ImmReleaseContext(window, current);
        ImmAssociateContextEx(window, nullptr, IACE_DEFAULT);
        if (!std::exchange(reported_on_, true)) {
            HIMC context = ImmGetContext(window);
            char line[160];
            std::snprintf(line, sizeof line, "Social IME on: game context=%d, context=%d, open=%d, layout=%08llX.",
                          game_had_context_ ? 1 : 0, context ? 1 : 0, context && ImmGetOpenStatus(context) ? 1 : 0,
                          static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(GetKeyboardLayout(0))));
            if (context) ImmReleaseContext(window, context);
            report(line);
        }
    } else {
        toggle_pending_ = false;
        if (HIMC context = ImmGetContext(window)) {
            ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
            ImmReleaseContext(window, context);
        }
        if (!game_had_context_) ImmAssociateContext(window, nullptr);
    }
}

void ImeBridge::ensure_context(Window handle) {
    const HWND window = as_window(handle);
    if (HIMC context = ImmGetContext(window)) {
        ImmReleaseContext(window, context);
        return;
    }
    ImmAssociateContextEx(window, nullptr, IACE_DEFAULT);
    if (!std::exchange(reported_restore_, true)) report("Social IME: the game removed the input context; restored it.");
}

// The game can keep the IME from seeing its on/off key (its context was gone
// when the key was read, or the key never reaches the IME). If the IME did not
// switch on its own, switch it here once the key has been dispatched.
void ImeBridge::key_down(Window handle, unsigned message, WParam key) {
    {
        std::lock_guard lock(mutex_);
        if (!applied_) return;
    }
    ensure_context(handle);
    if (!toggle_key(message, static_cast<WPARAM>(key)) || !wake_) return;
    const std::uint64_t now = GetTickCount64();
    if (now - open_notify_tick_ < handled_window_ms) return; // the IME already switched
    const HWND window = as_window(handle);
    HIMC context = ImmGetContext(window);
    if (!context) return;
    toggle_before_ = ImmGetOpenStatus(context) != FALSE;
    ImmReleaseContext(window, context);
    toggle_tick_ = now;
    toggle_pending_ = true;
    PostMessageW(window, wake_, wake_toggle, 0);
}

void ImeBridge::check_toggle(Window handle) {
    if (!std::exchange(toggle_pending_, false)) return;
    {
        std::lock_guard lock(mutex_);
        if (!applied_) return;
    }
    if (open_notify_tick_ >= toggle_tick_) return; // the IME handled the key itself
    const HWND window = as_window(handle);
    HIMC context = ImmGetContext(window);
    if (!context) return;
    if ((ImmGetOpenStatus(context) != FALSE) == toggle_before_) {
        const bool open = !toggle_before_;
        ImmSetOpenStatus(context, open ? TRUE : FALSE);
        // Opened from here, start in hiragana like the IME's own key does.
        DWORD conversion = 0, sentence = 0;
        if (open && ImmGetConversionStatus(context, &conversion, &sentence))
            ImmSetConversionStatus(context, conversion | IME_CMODE_NATIVE | IME_CMODE_FULLSHAPE, sentence);
        if (!std::exchange(reported_toggle_, true)) report("Social IME: the on/off key did not reach the IME; switched it directly.");
    }
    ImmReleaseContext(window, context);
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
        if (wparam == wake_toggle) check_toggle(handle);
        result = 0;
        return true;
    }
    if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
        key_down(handle, message, wparam);
        return false; // the key itself still goes on as before
    }
    if (message == WM_IME_NOTIFY && wparam == IMN_SETOPENSTATUS) open_notify_tick_ = GetTickCount64();
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
        if (!std::exchange(reported_compose_, true)) report("Social IME: composition started.");
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
