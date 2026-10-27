#pragma once

#include <medicon/update/Loader.hpp>

#include <filesystem>

#include <memory>
#include <optional>
#include <string>

// Windows implementations behind the portable update interfaces. The header is
// tiny so UpdateManager and its tests never pull in Win32.
namespace medicon::update {

// WinHTTP GET to one host over HTTPS, redirects refused, default certificate
// checks, bounded body and timeouts.
std::unique_ptr<Fetcher> make_winhttp_fetcher(std::string host, std::string user_agent);

// CNG SHA-256 and RSA-PSS verification with the embedded release key.
std::unique_ptr<Verifier> make_cng_verifier();

// %LOCALAPPDATA%\Medicon with versions\, cache\downloads\, logs\ and state.json.
std::unique_ptr<FileStore> make_file_store(const std::wstring& data_dir);

// LoadLibraryExW of a verified Core, API check, Medicon_Initialize with host info.
std::unique_ptr<CoreLauncher> make_core_launcher(const std::wstring& data_dir);

// %LOCALAPPDATA%\Medicon, created if missing; empty on failure.
std::wstring default_data_dir();
// The running Minecraft.Windows.exe FileVersion ("1.26.52.3"), or empty.
std::string game_file_version();

} // namespace medicon::update
