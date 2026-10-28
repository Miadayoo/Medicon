#include <Windows.h>
#include <cwchar>
#include <medicon/platform/OverlayApi.h>

extern "C" int MediconAutoUnloadReady();
extern "C" void MediconEnableAutoUnload() noexcept;

namespace {
DWORD WINAPI controller(void* context) {
    const auto module = static_cast<HMODULE>(context);
    try {
        MediconStartOverlay();
        bool end_was_down = false;
        for (;;) {
            const int unload_ready = MediconAutoUnloadReady();
            if (unload_ready > 0) {
                FreeLibraryAndExitThread(module, 0);
            }
            const bool end_down = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
            if (end_down && !end_was_down) {
                const HWND foreground = GetForegroundWindow();
                DWORD process_id{};
                if (foreground) GetWindowThreadProcessId(foreground, &process_id);
                const auto status = MediconOverlayStatus();
                if (process_id == GetCurrentProcessId() && (status == 0 || status == 4))
                    MediconStartOverlay();
            }
            end_was_down = end_down;
            Sleep(25);
        }
    } catch (...) {
        OutputDebugStringA("[Medicon] Automatic overlay controller stopped after an unexpected error.\n");
    }
    return 1;
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
        // MediconLoader sets MEDICON_LOADER before LoadLibraryExW and then calls
        // Medicon_Initialize itself, so the Core must not also auto-start here.
        if (_wcsicmp(name, L"Minecraft.Windows.exe") == 0 && GetEnvironmentVariableW(L"MEDICON_LOADER", nullptr, 0) == 0) {
            MediconEnableAutoUnload();
            if (HANDLE thread = CreateThread(nullptr, 0, controller, instance, 0, nullptr)) CloseHandle(thread);
        }
    }
    return TRUE;
}
