#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace medicon::social {

// license.bin: the license key encrypted with DPAPI for the current Windows
// user. Never logs the key; errors describe the failure only.
class LicenseStore {
public:
    explicit LicenseStore(std::filesystem::path file, std::string purpose = "Medicon.Social.License.v1")
        : file_(std::move(file)), purpose_(std::move(purpose)) {}

    // nullopt without an error when no key is stored.
    std::optional<std::string> load(std::string& error) const;
    bool save(std::string_view key, std::string& error) const;
    // Logout. A missing file counts as success.
    bool clear(std::string& error) const;

    const std::filesystem::path& file() const noexcept { return file_; }

private:
    std::filesystem::path file_;
    std::string purpose_;
};

} // namespace medicon::social
