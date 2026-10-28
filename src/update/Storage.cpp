#include "../loader/Platform.hpp"

#include <Windows.h>
#include <bcrypt.h>
#include <shlobj.h>

#include <array>
#include <fstream>
#include <sstream>
#include <system_error>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace medicon::update {
namespace {

namespace fs = std::filesystem;

std::string narrow(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(size < 0 ? 0 : size), '\0');
    if (size > 0)
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), size, nullptr, nullptr);
    return utf8;
}

std::optional<std::string> read_file(const fs::path& path, std::uint64_t cap) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size > cap) return std::nullopt;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;
    std::string data((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return stream.good() || stream.eof() ? std::optional<std::string>(std::move(data)) : std::nullopt;
}

bool write_file(const fs::path& path, std::string_view data) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    stream.write(data.data(), static_cast<std::streamsize>(data.size()));
    stream.flush();
    return stream.good();
}

class WindowsStore final : public FileStore {
public:
    explicit WindowsStore(fs::path root) : root_(std::move(root)) {
        std::error_code ec;
        fs::create_directories(root_ / "versions", ec);
        fs::create_directories(root_ / "cache" / "downloads", ec);
        fs::create_directories(root_ / "logs", ec);
    }

    std::vector<InstalledCore> installed() override {
        std::vector<InstalledCore> result;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(root_ / "versions", ec)) {
            if (!entry.is_directory(ec)) continue;
            // path::string() is the ANSI code page and throws on characters it
            // cannot hold; every narrow path here is UTF-8 instead.
            const auto version = narrow(entry.path().filename().wstring());
            const auto dir = entry.path();
            auto manifest = read_file(dir / "manifest.json", max_manifest_bytes);
            auto signature = read_file(dir / "manifest.sig", max_signature_bytes);
            std::error_code core_ec;
            if (manifest && signature && fs::exists(dir / std::string(core_file_name), core_ec))
                result.push_back({version, std::move(*manifest), std::move(*signature)});
        }
        return result;
    }

    std::string core_path(const std::string& version) override {
        // UTF-8, which the launcher widens back: a profile folder such as
        // C:\Users\<Japanese name> must survive the round trip.
        return narrow((root_ / "versions" / version / std::string(core_file_name)).wstring());
    }

    std::string core_sha256(const std::string& version) override {
        std::ifstream stream(root_ / "versions" / version / std::string(core_file_name), std::ios::binary);
        if (!stream) return {};
        BCRYPT_ALG_HANDLE alg = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
        std::string hex;
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
            std::array<char, 65536> buffer{};
            bool ok = true;
            while (stream) {
                stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const auto read = static_cast<ULONG>(stream.gcount());
                if (read && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), read, 0) != 0) { ok = false; break; }
            }
            std::array<std::uint8_t, 32> digest{};
            if (ok && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0) {
                hex.resize(64);
                for (std::size_t i = 0; i < digest.size(); ++i) {
                    hex[i * 2] = "0123456789abcdef"[(digest[i] >> 4) & 0xF];
                    hex[i * 2 + 1] = "0123456789abcdef"[digest[i] & 0xF];
                }
            }
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
        return hex;
    }

    bool install(const std::string& version, std::string_view dll, std::string_view manifest,
                 std::string_view signature) override {
        std::error_code ec;
        // Build the whole version in a temporary directory, then rename it in:
        // a half-written version is never visible, and the running Core is
        // never overwritten because a new version is a new directory.
        const auto staging = root_ / "cache" / ("staging-" + version + "-" + std::to_string(GetCurrentProcessId()));
        fs::remove_all(staging, ec);
        if (!fs::create_directories(staging, ec)) return false;
        const bool wrote = write_file(staging / std::string(core_file_name), dll) &&
                           write_file(staging / "manifest.json", manifest) &&
                           write_file(staging / "manifest.sig", signature);
        const auto target = root_ / "versions" / version;
        if (!wrote || fs::exists(target, ec)) { fs::remove_all(staging, ec); return false; }
        fs::rename(staging, target, ec);
        if (ec) { fs::remove_all(staging, ec); return false; }
        return true;
    }

    void remove(const std::string& version) override {
        std::error_code ec;
        fs::remove_all(root_ / "versions" / version, ec);
    }

    LoaderState load_state() override {
        const auto json = read_file(root_ / "state.json", 256 * 1024);
        return json ? parse_state(*json) : LoaderState{};
    }

    void save_state(const LoaderState& state) override {
        const auto temp = root_ / "state.json.tmp";
        if (!write_file(temp, serialize_state(state))) return;
        std::error_code ec;
        fs::rename(temp, root_ / "state.json", ec);
        if (ec) fs::remove(temp, ec);
    }

    void log(std::string_view line) override {
        std::ofstream stream(root_ / "logs" / "updater.log", std::ios::app);
        if (!stream) return;
        SYSTEMTIME now{};
        GetLocalTime(&now);
        char stamp[32]{};
        std::snprintf(stamp, sizeof stamp, "[%04u-%02u-%02u %02u:%02u:%02u] ", now.wYear, now.wMonth, now.wDay, now.wHour,
                      now.wMinute, now.wSecond);
        stream << stamp;
        stream.write(line.data(), static_cast<std::streamsize>(line.size()));
        stream << '\n';
    }

private:
    fs::path root_;
};

} // namespace

std::wstring default_data_dir() {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &path))) return {};
    std::filesystem::path root = std::filesystem::path(path) / L"Medicon";
    CoTaskMemFree(path);
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    return root.wstring();
}

std::string game_file_version() {
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.data(), &ignored);
    if (!size || size > 16 * 1024 * 1024) return {};
    std::vector<std::byte> data(size);
    VS_FIXEDFILEINFO* info = nullptr;
    UINT info_size = 0;
    if (!GetFileVersionInfoW(path.data(), 0, size, data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &info_size) || !info ||
        info_size < sizeof(*info) || info->dwSignature != 0xFEEF04BD)
        return {};
    return std::to_string(HIWORD(info->dwFileVersionMS)) + "." + std::to_string(LOWORD(info->dwFileVersionMS)) + "." +
           std::to_string(HIWORD(info->dwFileVersionLS)) + "." + std::to_string(LOWORD(info->dwFileVersionLS));
}

std::unique_ptr<FileStore> make_file_store(const std::wstring& data_dir) {
    return std::make_unique<WindowsStore>(std::filesystem::path(data_dir));
}

} // namespace medicon::update
