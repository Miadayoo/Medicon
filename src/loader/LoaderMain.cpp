#include "Platform.hpp"

#include <medicon/update/HostApi.h>

#include <Windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

// MediconLoader.dll: the small, stable DLL the user installs once. DllMain only
// starts a worker thread; the update itself runs there, never under the loader
// lock (docs/auto-update.md).
namespace {

using namespace medicon::update;

// The one host ever contacted. Build with -DMEDICON_UPDATE_HOST=... to change it.
#ifndef MEDICON_UPDATE_HOST
#define MEDICON_UPDATE_HOST "medicon.auau1000.com"
#endif

std::mutex g_status_mutex;
Status g_status;
std::atomic_bool g_started{false};

void set_status(const Status& status) {
    std::lock_guard lock(g_status_mutex);
    g_status = status;
}

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size < 0 ? 0 : size), L'\0');
    if (size > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
    return wide;
}

// Without this the game simply runs without Medicon and the player cannot
// tell why. Shown from the worker, never from DllMain.
void show_failure(const std::wstring& data_dir, const std::string& reason) {
    std::wstring text = L"Medicon could not start.\n\n" + widen(reason);
    if (!data_dir.empty()) text += L"\n\nDetails: " + data_dir + L"\\logs\\updater.log";
    MessageBoxW(nullptr, text.c_str(), L"Medicon", MB_OK | MB_ICONWARNING | MB_TOPMOST);
}

DWORD WINAPI worker(void*) {
    std::wstring data_dir;
    try {
        data_dir = default_data_dir();
        if (data_dir.empty()) {
            show_failure(data_dir, "The Medicon data folder in %LOCALAPPDATA% could not be created.");
            return 1;
        }
        auto fetcher = make_winhttp_fetcher(MEDICON_UPDATE_HOST, "MediconLoader/1");
        auto verifier = make_cng_verifier();
        auto store = make_file_store(data_dir);
        auto launcher = make_core_launcher(data_dir);
        store->log("MediconLoader starting");
        UpdateConfig config{MEDICON_UPDATE_HOST, game_file_version()};
        const auto outcome = run_update(*fetcher, *verifier, *store, *launcher, config, set_status);
        if (outcome.started_version.empty()) {
            store->log("MediconLoader could not start any Core: " + outcome.failure);
            show_failure(data_dir, outcome.failure);
        }
    } catch (const std::exception& error) {
        // Never let an exception escape the worker into the game.
        show_failure(data_dir, std::string("Unexpected loader error: ") + error.what());
    } catch (...) {
        show_failure(data_dir, "Unexpected loader error.");
    }
    return 0;
}

void start_once() {
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;
    if (HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr)) CloseHandle(thread);
}

} // namespace

extern "C" __declspec(dllexport) int MediconLoaderGetStatus(MediconLoaderStatus* out) {
    if (!out || out->size < sizeof(MediconLoaderStatus)) return 0;
    std::lock_guard lock(g_status_mutex);
    out->phase = static_cast<std::uint32_t>(g_status.phase);
    out->progress_permille = g_status.progress_permille;
    const auto copy = [](wchar_t* dst, std::size_t cap, const std::string& value) {
        const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
        const std::size_t length = size > 0 ? std::min<std::size_t>(static_cast<std::size_t>(size), cap - 1) : 0;
        if (length) MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), dst, static_cast<int>(length));
        dst[length] = 0;
    };
    copy(out->current_version, 32, g_status.current_version);
    copy(out->latest_version, 32, g_status.latest_version);
    const std::size_t error_length = std::min(g_status.last_error.size(), sizeof(out->last_error) - 1);
    std::memcpy(out->last_error, g_status.last_error.data(), error_length);
    out->last_error[error_length] = 0;
    return 1;
}

// Lets another injector start the update explicitly, like MediconStartOverlay.
extern "C" __declspec(dllexport) int MediconLoaderStart(void) {
    start_once();
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        // Only inside the game, and only a thread is created here: no network,
        // no LoadLibrary, nothing that could deadlock the loader lock.
        wchar_t executable[MAX_PATH]{};
        GetModuleFileNameW(nullptr, executable, MAX_PATH);
        const wchar_t* name = wcsrchr(executable, L'\\');
        name = name ? name + 1 : executable;
        if (_wcsicmp(name, L"Minecraft.Windows.exe") == 0) start_once();
    }
    return TRUE;
}
