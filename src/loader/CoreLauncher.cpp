#include "Platform.hpp"

#include <medicon/update/HostApi.h>

#include <Windows.h>

namespace medicon::update {
namespace {

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size < 0 ? 0 : size), L'\0');
    if (size > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
    return wide;
}

class WindowsLauncher final : public CoreLauncher {
public:
    explicit WindowsLauncher(std::wstring data_dir) : data_dir_(std::move(data_dir)) {}

    LaunchResult launch(const std::string& version, const std::string& core_path, State state,
                        const std::string& previous_version, std::string& error) override {
        // Tell the Core's DllMain not to auto-start: this launcher drives it
        // through Medicon_Initialize instead.
        SetEnvironmentVariableW(L"MEDICON_LOADER", L"1");
        // The absolute, verified path only. LOAD_WITH_ALTERED_SEARCH_PATH lets the
        // Core resolve its own neighbours, not the game's working directory.
        const HMODULE core = LoadLibraryExW(widen(core_path).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!core) {
            error = "Windows error " + std::to_string(GetLastError()) + " loading the file";
            return LaunchResult::LoadFailed;
        }

        const auto api = reinterpret_cast<MediconGetApiVersionFn>(GetProcAddress(core, "Medicon_GetAPIVersion"));
        const auto initialize = reinterpret_cast<MediconInitializeFn>(GetProcAddress(core, "Medicon_Initialize"));
        // A Core without the required exports, or built for another ABI, is never started.
        if (!api || !initialize || api() != MEDICON_CORE_API_VERSION) {
            error = !api || !initialize ? "missing Medicon exports" : "Core API " + std::to_string(api());
            FreeLibrary(core);
            return LaunchResult::Rejected;
        }

        version_ = widen(version);
        previous_ = widen(previous_version);
        MediconHostAPI host{};
        host.size = sizeof(host);
        host.api_version = MEDICON_CORE_API_VERSION;
        host.core_version = version_.c_str();
        host.previous_version = previous_.empty() || previous_ == version_ ? nullptr : previous_.c_str();
        host.data_directory = data_dir_.c_str();
        host.update_state = static_cast<std::uint32_t>(state);

        if (initialize(&host) != 1) {
            // The Core cleans up its own partial start in Medicon_Initialize.
            // It stays mapped: hooks or a worker may already hold references.
            error = "Medicon_Initialize failed";
            return LaunchResult::Rejected;
        }
        return LaunchResult::Started;
    }

private:
    std::wstring data_dir_;
    std::wstring version_;
    std::wstring previous_;
};

} // namespace

std::unique_ptr<CoreLauncher> make_core_launcher(const std::wstring& data_dir) {
    return std::make_unique<WindowsLauncher>(data_dir);
}

} // namespace medicon::update
