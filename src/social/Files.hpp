#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace medicon::social::detail {

// Writes a temporary file next to `path` and renames it over the original, so a
// crash never leaves half a file. `error` never contains the bytes.
bool replace_file(const std::filesystem::path& path, std::string_view bytes, std::string& error);
// The whole file, or an empty string when it is missing or unreadable.
std::string read_file(const std::filesystem::path& path);

} // namespace medicon::social::detail
