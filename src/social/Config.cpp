#include <medicon/social/Config.hpp>

#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>

namespace medicon::social {
namespace {

using Json = nlohmann::json;

void read_bool(const Json& object, const char* key, bool& out, std::vector<std::string>& warnings,
               const std::string& location) {
    const auto found = object.find(key);
    if (found == object.end()) return;
    if (found->is_boolean()) out = found->get<bool>();
    else warnings.push_back(location + key + ": expected a boolean");
}

bool ascii_digits(std::string_view text) {
    if (text.empty()) return false;
    for (const char c : text) if (c < '0' || c > '9') return false;
    return true;
}

} // namespace

ConfigLoad parse_config(std::string_view text) {
    ConfigLoad load;
    const Json root = Json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) return load;
    load.parsed = true;
    auto& config = load.config;
    read_bool(root, "debugTiming", config.debug_timing, load.warnings, {});
    read_bool(root, "debugToastSlowMotion", config.debug_toast_slow_motion, load.warnings, {});
    if (const auto music = root.find("music"); music != root.end()) {
        if (!music->is_object()) load.warnings.emplace_back("music: expected an object");
        else read_bool(*music, "enabled", config.music_enabled, load.warnings, "music.");
    }
    // The obsolete field is ignored; its value must never enter warnings.
    if (root.contains("debugLicenseKey"))
        load.warnings.emplace_back("debugLicenseKey: no longer used; sign in on the login screen and remove it");
    if (const auto notify = root.find("notify"); notify != root.end()) {
        if (!notify->is_object()) {
            load.warnings.emplace_back("notify: expected an object");
        } else {
            auto& settings = config.notify;
            read_bool(*notify, "dm", settings.dm, load.warnings, "notify.");
            read_bool(*notify, "friend", settings.friend_requests, load.warnings, "notify.");
            read_bool(*notify, "news", settings.news, load.warnings, "notify.");
            read_bool(*notify, "inGame", settings.in_game, load.warnings, "notify.");
            read_bool(*notify, "sound", settings.sound, load.warnings, "notify.");
            if (const auto offset = notify->find("inGameOffsetY"); offset != notify->end()) {
                const double value = offset->is_number() ? offset->get<double>() : std::numeric_limits<double>::quiet_NaN();
                // Keep the popup on screen: a stray huge value would hide every notification.
                if (std::isfinite(value) && std::abs(value) <= 1080.0) settings.in_game_offset_y = static_cast<float>(value);
                else load.warnings.emplace_back("notify.inGameOffsetY: expected a number within +/-1080");
            }
        }
    }
    return load;
}

namespace {

nlohmann::ordered_json notify_json(const NotifySettings& notify) {
    // Whole pixels read better in a file people edit by hand.
    const float offset = notify.in_game_offset_y;
    nlohmann::ordered_json offset_value = std::round(offset) == offset ? nlohmann::ordered_json(static_cast<int>(offset))
                                                                       : nlohmann::ordered_json(offset);
    return {{"dm", notify.dm}, {"friend", notify.friend_requests}, {"news", notify.news},
            {"inGame", notify.in_game}, {"sound", notify.sound}, {"inGameOffsetY", std::move(offset_value)}};
}

} // namespace

std::string default_config_text() {
    const SocialConfig defaults;
    // Insertion order keeps the file readable in the order of spec 4.10.
    nlohmann::ordered_json root{{"notify", notify_json(defaults.notify)}, {"music", {{"enabled", defaults.music_enabled}}}};
    return root.dump(2) + "\n";
}

std::optional<std::string> with_notify(std::string_view existing, const NotifySettings& notify) {
    if (existing.find_first_not_of(" \t\r\n") == std::string_view::npos) existing = {};
    auto root = existing.empty() ? nlohmann::ordered_json::parse(default_config_text())
                                 : nlohmann::ordered_json::parse(existing, nullptr, false);
    if (root.is_discarded() || !root.is_object()) return std::nullopt;
    root.erase("debugLicenseKey");
    root.erase("serverUrl");
    root["notify"] = notify_json(notify);
    return root.dump(2, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
}

std::optional<std::string> without_legacy_settings(std::string_view existing) {
    auto root = nlohmann::ordered_json::parse(existing, nullptr, false);
    if (root.is_discarded() || !root.is_object()) return std::nullopt;
    const auto removed_key = root.erase("debugLicenseKey");
    const auto removed_url = root.erase("serverUrl");
    if (removed_key == 0 && removed_url == 0) return std::string(existing);
    return root.dump(2, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
}

std::optional<std::string> with_settings(std::string_view existing, const NotifySettings& notify, bool music_enabled) {
    const auto saved = with_notify(existing, notify);
    if (!saved) return std::nullopt;
    auto root = nlohmann::ordered_json::parse(*saved);
    auto& music = root["music"];
    if (!music.is_object()) music = nlohmann::ordered_json::object();
    music["enabled"] = music_enabled;
    return root.dump(2, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
}

std::optional<Endpoint> parse_server_url(std::string_view url) {
    Endpoint endpoint;
    constexpr std::string_view https = "https://", http = "http://";
    if (url.starts_with(https)) {
        url.remove_prefix(https.size());
    } else if (url.starts_with(http)) {
        endpoint.secure = false;
        endpoint.port = 80;
        url.remove_prefix(http.size());
    } else {
        return std::nullopt;
    }
    const auto slash = url.find('/');
    std::string_view authority = url.substr(0, slash);
    std::string_view path = slash == std::string_view::npos ? std::string_view{} : url.substr(slash);
    // Queries, fragments and credentials have no meaning for a base URL.
    if (authority.empty() || authority.find_first_of("@?#") != std::string_view::npos ||
        path.find_first_of("?#") != std::string_view::npos)
        return std::nullopt;
    const auto colon = authority.rfind(':');
    // IPv6 literals ("[::1]") are not needed for this server and are rejected.
    if (authority.find_first_of("[]") != std::string_view::npos) return std::nullopt;
    if (colon != std::string_view::npos) {
        const auto port = authority.substr(colon + 1);
        if (!ascii_digits(port) || port.size() > 5) return std::nullopt;
        const unsigned long value = std::stoul(std::string(port));
        if (value == 0 || value > 65535) return std::nullopt;
        endpoint.port = static_cast<std::uint16_t>(value);
        authority = authority.substr(0, colon);
    }
    if (authority.empty()) return std::nullopt;
    endpoint.host = std::string(authority);
    while (path.ends_with('/')) path.remove_suffix(1);
    endpoint.base_path = std::string(path);
    return endpoint;
}

std::string websocket_url(const Endpoint& endpoint) {
    const bool default_port = endpoint.port == (endpoint.secure ? 443 : 80);
    std::string url = endpoint.secure ? "wss://" : "ws://";
    url += endpoint.host;
    if (!default_port) url += ":" + std::to_string(endpoint.port);
    return url + endpoint.base_path + "/ws";
}

} // namespace medicon::social
