#include "../src/alt/Clipboard.hpp"

#include <iostream>
#include <stdexcept>

namespace {

// Clipboard contents belong to a window station. Test real Win32 writes without
// replacing the user's text, image, or any other format on the interactive one.
struct IsolatedClipboard {
    HWINSTA previous_station = GetProcessWindowStation();
    HDESK previous_desktop = GetThreadDesktop(GetCurrentThreadId());
    HWINSTA station = nullptr;
    HDESK desktop = nullptr;
    bool initialize() {
        station = CreateWindowStationW(nullptr, 0, WINSTA_ALL_ACCESS, nullptr);
        if (!station || !SetProcessWindowStation(station)) return false;
        desktop = CreateDesktopW(L"MediconClipboardTest", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        return desktop && SetThreadDesktop(desktop);
    }
    ~IsolatedClipboard() {
        SetThreadDesktop(previous_desktop);
        SetProcessWindowStation(previous_station);
        if (desktop) CloseDesktop(desktop);
        if (station) CloseWindowStation(station);
    }
};

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::wstring clipboard_text() {
    check(OpenClipboard(nullptr) != FALSE, "reading the test clipboard failed");
    std::wstring result;
    if (HANDLE memory = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* text = static_cast<const wchar_t*>(GlobalLock(memory))) {
            result = text;
            GlobalUnlock(memory);
        }
    }
    CloseClipboard();
    return result;
}

} // namespace

int main() {
    try {
        IsolatedClipboard clipboard;
        check(clipboard.initialize(), "could not create the isolated clipboard");
        check(medicon::alt::detail::copy_sign_in_code("ABCD1234"), "initial code copy failed");
        check(clipboard_text() == L"ABCD1234", "the pasted code differs after the owner window is destroyed");
        check(medicon::alt::detail::copy_sign_in_code("SECOND12"), "copying the next code failed");
        check(clipboard_text() == L"SECOND12", "the second code did not replace the first");
        check(!medicon::alt::detail::copy_sign_in_code("") &&
              !medicon::alt::detail::copy_sign_in_code("not a code"), "invalid text was copied");
        check(clipboard_text() == L"SECOND12", "invalid input changed the clipboard");
        std::cout << "Native code copy/paste passed in an isolated window station\n";
    } catch (const std::exception& error) {
        std::cerr << "alt clipboard test FAILED: " << error.what() << '\n';
        return 1;
    }
}
