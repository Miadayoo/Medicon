#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <utility>

namespace medicon {

class ModuleManager;

// Schema v1. Unknown module/setting IDs are ignored for forward compatibility.
// A missing file succeeds with defaults; malformed known values never apply.
// save() replaces the destination atomically using a sibling temporary file.
class ConfigStore {
public:
    explicit ConfigStore(std::filesystem::path path) : path_(std::move(path)) {}
    const std::filesystem::path& path() const noexcept { return path_; }
    bool load(ModuleManager& modules, std::string& error) const;
    bool save(const ModuleManager& modules, std::string& error) const;

private:
    std::filesystem::path path_;
};

} // namespace medicon
