#include <Windows.h>
#include <cwchar>
#include <medicon/platform/OverlayApi.h>

namespace {
DWORD WINAPI bootstrap(void*) {
    MediconStartOverlay();
    return 0;
}
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        wchar_t executable[MAX_PATH]{};
        GetModuleFileNameW(nullptr, executable, MAX_PATH);
        const auto* name = std::wcsrchr(executable, L'\\');
        name = name ? name + 1 : executable;
        // Explicit C API users and tests retain their existing lifecycle. The
        // worker cannot run until loader notifications finish; never join here.
        if (_wcsicmp(name, L"Minecraft.Windows.exe") == 0) {
            if (HANDLE thread = CreateThread(nullptr, 0, bootstrap, nullptr, 0, nullptr)) CloseHandle(thread);
        }
    }
    return TRUE;
}

