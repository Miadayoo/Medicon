#pragma once

#include <Windows.h>

#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

namespace medicon::alt::detail {

// Runs on the sign-in worker. A non-null owner is required for SetClipboardData
// after EmptyClipboard; OpenClipboard(nullptr) cannot be used for this write.
inline bool copy_sign_in_code(std::string_view code) {
    if (code.empty()) return false;
    std::wstring text;
    for (const unsigned char c : code) {
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return false;
        text.push_back(static_cast<wchar_t>(c));
    }
    struct Owner {
        HWND window = CreateWindowExW(0, L"STATIC", L"Medicon sign-in code", 0, 0, 0, 0, 0,
                                       HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~Owner() { if (window) DestroyWindow(window); }
    } owner;
    if (!owner.window) return false;
    bool opened = false;
    for (int attempt = 0; attempt < 10 && !opened; ++attempt) {
        opened = OpenClipboard(owner.window) != FALSE;
        if (!opened) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!opened) return false;
    struct Close { ~Close() { CloseClipboard(); } } close;
    const auto bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return false;
    void* target = GlobalLock(memory);
    if (!target) { GlobalFree(memory); return false; }
    std::memcpy(target, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT, memory)) {
        GlobalFree(memory);
        return false;
    }
    return true;
}

} // namespace medicon::alt::detail
