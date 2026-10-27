#include <medicon/update/Update.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>

namespace medicon::update {
namespace {

bool part(std::string_view text, int& value) noexcept {
    if (text.empty() || text.size() > 4 || (text.size() > 1 && text.front() == '0')) return false;
    if (!std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool lower_hex(std::string_view text, std::size_t length) noexcept {
    return text.size() == length &&
           std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::vector<std::string> versions_in(const nlohmann::json& root, const char* key) {
    std::vector<std::string> result;
    const auto found = root.find(key);
    if (found == root.end() || !found->is_array()) return result;
    for (const auto& item : *found)
        if (item.is_string() && parse_version(item.get<std::string>())) result.push_back(item.get<std::string>());
    return result;
}

bool newer(const std::string& a, const std::string& b) { return *parse_version(a) > *parse_version(b); }

} // namespace

std::optional<Version> parse_version(std::string_view text) noexcept {
    Version version;
    const auto first = text.find('.');
    if (first == std::string_view::npos) return std::nullopt;
    const auto second = text.find('.', first + 1);
    if (second == std::string_view::npos || text.find('.', second + 1) != std::string_view::npos) return std::nullopt;
    if (!part(text.substr(0, first), version.major) || !part(text.substr(first + 1, second - first - 1), version.minor) ||
        !part(text.substr(second + 1), version.patch))
        return std::nullopt;
    return version;
}

std::string to_string(const Version& version) {
    return std::to_string(version.major) + "." + std::to_string(version.minor) + "." + std::to_string(version.patch);
}

std::optional<Manifest> parse_manifest(std::string_view bytes, std::string_view allowed_host, std::string& error) {
    error.clear();
    const auto fail = [&](const char* reason) -> std::optional<Manifest> {
        error = reason;
        return std::nullopt;
    };
    if (bytes.empty() || bytes.size() > max_manifest_bytes) return fail("manifest_size");
    const auto root = nlohmann::json::parse(bytes, nullptr, false);
    if (root.is_discarded() || !root.is_object()) return fail("manifest_json");
    const auto text = [&](const char* key) -> std::optional<std::string> {
        const auto found = root.find(key);
        if (found == root.end() || !found->is_string()) return std::nullopt;
        return found->get<std::string>();
    };
    Manifest manifest;
    const auto version = text("version");
    const auto parsed = version ? parse_version(*version) : std::nullopt;
    if (!parsed) return fail("manifest_version");
    manifest.version = *parsed;
    manifest.version_text = *version;
    manifest.channel = text("channel").value_or("");
    if (manifest.channel != "stable") return fail("manifest_channel");
    manifest.mcbe_version = text("mcbe_version").value_or("");
    const auto api = root.find("loader_api");
    if (api == root.end() || !api->is_number_unsigned()) return fail("manifest_loader_api");
    manifest.loader_api = api->get<std::uint32_t>();
    if (manifest.loader_api != loader_api) return fail("loader_api_mismatch");
    manifest.sha256 = text("sha256").value_or("");
    if (!lower_hex(manifest.sha256, 64)) return fail("manifest_sha256");
    const auto size = root.find("size");
    if (size == root.end() || !size->is_number_unsigned()) return fail("manifest_size_field");
    manifest.size = size->get<std::uint64_t>();
    if (manifest.size == 0 || manifest.size > max_core_bytes) return fail("manifest_size_field");
    manifest.download_url = text("download_url").value_or("");
    // Exactly this URL, so neither another host nor another path is ever fetched.
    const std::string expected = "https://" + std::string(allowed_host) + "/medicon/releases/" + manifest.version_text +
                                 "/" + std::string(core_file_name);
    if (allowed_host.empty() || manifest.download_url != expected) return fail("manifest_download_url");
    if (const auto log = root.find("changelog"); log != root.end() && log->is_array()) {
        for (const auto& line : *log)
            if (line.is_string() && manifest.changelog.size() < 50) manifest.changelog.push_back(line.get<std::string>());
    }
    return manifest;
}

bool mcbe_compatible(std::string_view manifest_mcbe, std::string_view game_version) noexcept {
    if (!parse_version(manifest_mcbe)) return false;
    if (!game_version.starts_with(manifest_mcbe)) return false;
    // "1.26.52" must not match "1.26.520.1".
    return game_version.size() == manifest_mcbe.size() || game_version[manifest_mcbe.size()] == '.';
}

bool same_digest(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size() || a.empty()) return false;
    unsigned char difference = 0;
    for (std::size_t i = 0; i < a.size(); ++i) difference |= static_cast<unsigned char>(a[i] ^ b[i]);
    return difference == 0;
}

LoaderState parse_state(std::string_view json) noexcept {
    LoaderState state;
    try {
        const auto root = nlohmann::json::parse(json, nullptr, false);
        if (root.is_discarded() || !root.is_object()) return state;
        state.good = versions_in(root, "good");
        state.bad = versions_in(root, "bad");
        if (const auto last = root.find("last_run"); last != root.end() && last->is_string() &&
                                                     parse_version(last->get<std::string>()))
            state.last_run = last->get<std::string>();
    } catch (...) {
        return {};
    }
    return state;
}

std::string serialize_state(const LoaderState& state) {
    nlohmann::json root;
    root["good"] = state.good;
    root["bad"] = state.bad;
    root["last_run"] = state.last_run;
    return root.dump(2) + "\n";
}

void mark_good(LoaderState& state, const std::string& version) {
    std::erase(state.bad, version);
    if (std::find(state.good.begin(), state.good.end(), version) == state.good.end()) state.good.push_back(version);
}

void mark_bad(LoaderState& state, const std::string& version) {
    std::erase(state.good, version);
    if (std::find(state.bad.begin(), state.bad.end(), version) == state.bad.end()) state.bad.push_back(version);
}

std::vector<std::string> start_order(std::vector<std::string> verified, const LoaderState& state) {
    std::erase_if(verified, [&](const std::string& version) {
        return !parse_version(version) || std::find(state.bad.begin(), state.bad.end(), version) != state.bad.end();
    });
    std::sort(verified.begin(), verified.end(), newer);
    verified.erase(std::unique(verified.begin(), verified.end()), verified.end());
    return verified;
}

std::vector<std::string> prune(const std::vector<std::string>& installed, const LoaderState& state,
                               const std::string& running, std::size_t keep) {
    std::vector<std::string> versions;
    for (const auto& version : installed)
        if (parse_version(version)) versions.push_back(version);
    std::sort(versions.begin(), versions.end(), newer);
    std::vector<std::string> kept;
    // The newest installed version may be an update that has not started yet.
    if (!versions.empty()) kept.push_back(versions.front());
    if (!running.empty()) kept.push_back(running);
    std::size_t good = 0;
    for (const auto& version : versions) {
        if (good == keep) break;
        if (std::find(state.good.begin(), state.good.end(), version) == state.good.end()) continue;
        kept.push_back(version);
        ++good;
    }
    std::vector<std::string> remove;
    for (const auto& version : versions)
        if (std::find(kept.begin(), kept.end(), version) == kept.end()) remove.push_back(version);
    return remove;
}

const char* phase_text(Phase phase) noexcept {
    switch (phase) {
    case Phase::Idle: return "Idle";
    case Phase::Checking: return "Checking for Updates";
    case Phase::Downloading: return "Downloading Update";
    case Phase::Verifying: return "Verifying Update";
    case Phase::Ready: return "Update Ready";
    case Phase::Loading: return "Loading Medicon";
    case Phase::UpToDate: return "Medicon is Up to Date";
    case Phase::Failed: return "Update Failed";
    case Phase::Offline: return "Running Offline";
    case Phase::Running: return "Medicon is Running";
    }
    return "Unknown";
}

} // namespace medicon::update
