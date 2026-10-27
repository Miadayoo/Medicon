#include "Files.hpp"

#include <Windows.h>

#include <chrono>
#include <fstream>
#include <iterator>
#include <system_error>

namespace medicon::social::detail {

bool replace_file(const std::filesystem::path& path, std::string_view bytes, std::string& error) {
    error.clear();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    auto temporary = path;
    temporary += ".tmp." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.close();
        if (!stream) {
            std::filesystem::remove(temporary, ec);
            error = path.filename().string() + " could not be written";
            return false;
        }
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        std::filesystem::remove(temporary, ec);
        error = "Replacing " + path.filename().string() + " failed (Win32 error " + std::to_string(code) + ")";
        return false;
    }
    return true;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

} // namespace medicon::social::detail
