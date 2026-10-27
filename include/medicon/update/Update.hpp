#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Portable rules of the Core auto-update (docs/auto-update.md). No WinHTTP,
// BCrypt or file system here, so every decision is unit tested on any host.
namespace medicon::update {

inline constexpr std::uint32_t loader_api = 1;
inline constexpr std::uint64_t max_core_bytes = 64ull * 1024 * 1024;
inline constexpr std::uint64_t max_manifest_bytes = 64 * 1024;
inline constexpr std::uint64_t max_signature_bytes = 4096;
inline constexpr std::string_view core_file_name = "MediconCore.dll";

struct Version {
    int major = 0, minor = 0, patch = 0;
    auto operator<=>(const Version&) const = default;
};
// "x.y.z", each part 0..9999 without leading zeros. Anything else, including
// "..", separators and spaces, is refused, because a version names a directory.
[[nodiscard]] std::optional<Version> parse_version(std::string_view text) noexcept;
[[nodiscard]] std::string to_string(const Version& version);

struct Manifest {
    Version version;
    std::string version_text;
    std::string channel;
    std::string mcbe_version;
    std::uint32_t loader_api = 0;
    std::string download_url;
    std::string sha256; // 64 lowercase hex characters
    std::uint64_t size = 0;
    std::vector<std::string> changelog;
};

// Parses and checks the (already signature-verified) manifest bytes against
// what this loader accepts. download_url must be https://<allowed_host>
// /medicon/releases/<version>/MediconCore.dll exactly.
[[nodiscard]] std::optional<Manifest> parse_manifest(std::string_view bytes, std::string_view allowed_host,
                                                     std::string& error);

// "1.26.52" against the running game's FileVersion "1.26.52.3": same first three parts.
[[nodiscard]] bool mcbe_compatible(std::string_view manifest_mcbe, std::string_view game_version) noexcept;

[[nodiscard]] bool same_digest(std::string_view a, std::string_view b) noexcept;

// state.json: which installed versions started fine or failed, and the one that ran last.
struct LoaderState {
    std::vector<std::string> good;
    std::vector<std::string> bad;
    std::string last_run;
};
[[nodiscard]] LoaderState parse_state(std::string_view json) noexcept;
[[nodiscard]] std::string serialize_state(const LoaderState& state);
void mark_good(LoaderState& state, const std::string& version);
void mark_bad(LoaderState& state, const std::string& version);

// Newest first, without versions that failed to start.
[[nodiscard]] std::vector<std::string> start_order(std::vector<std::string> verified, const LoaderState& state);
// Installed versions to delete: all but the newest `keep` good ones and `running`.
[[nodiscard]] std::vector<std::string> prune(const std::vector<std::string>& installed, const LoaderState& state,
                                             const std::string& running, std::size_t keep = 2);

enum class Phase : std::uint32_t {
    Idle, Checking, Downloading, Verifying, Ready, Loading, UpToDate, Failed, Offline, Running
};
[[nodiscard]] const char* phase_text(Phase phase) noexcept;

} // namespace medicon::update
