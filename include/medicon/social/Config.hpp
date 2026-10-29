#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace medicon::social {

// Embedded at build time; social.json cannot change the connection destination.
inline constexpr std::string_view server_url = MEDICON_SOCIAL_SERVER_URL;

// "notify" in social.json (spec 4.10).
struct NotifySettings {
    bool dm = true;
    bool friend_requests = true;
    bool news = true;
    bool in_game = true;
    bool sound = true;
    float in_game_offset_y = 0.0F; // reference px at 1080p, added to the top margin while playing
    bool operator==(const NotifySettings&) const = default;
};

// %APPDATA%\Medicon\social.json.
struct SocialConfig {
    NotifySettings notify;
    bool music_enabled = true;
    // Logs the per-frame cost of Social drawing (spec 4.9).
    bool debug_timing = false;
    // Plays notification animations at 0.25x to inspect each stage.
    bool debug_toast_slow_motion = false;
};

struct ConfigLoad {
    SocialConfig config;
    // Field names only, never values: the file can hold a license key.
    std::vector<std::string> warnings;
    // False when the text is not a JSON object. The caller must then keep the
    // file as it is instead of replacing it with defaults.
    bool parsed = false;
};

// Unknown fields are ignored; a field of the wrong type keeps its default.
ConfigLoad parse_config(std::string_view text);
// The document written when social.json does not exist yet.
std::string default_config_text();
// `existing` with its "notify" object replaced, every other field kept. Empty
// `existing` starts from the defaults. nullopt when it is not a JSON object:
// like config.json, a broken file is never overwritten.
std::optional<std::string> with_notify(std::string_view existing, const NotifySettings& notify);
// Saves the Social settings screen, preserving other music/unknown fields.
std::optional<std::string> with_settings(std::string_view existing, const NotifySettings& notify, bool music_enabled);
// Remove the obsolete server URL and plaintext credential while preserving other
// settings. nullopt means the document cannot safely be migrated.
std::optional<std::string> without_legacy_settings(std::string_view existing);

struct Endpoint {
    bool secure = true;
    std::string host;
    std::uint16_t port = 443;
    std::string base_path; // "" or "/prefix", never with a trailing slash
    bool operator==(const Endpoint&) const = default;
};

// Accepts https:// (production) and http:// (a local server while testing).
std::optional<Endpoint> parse_server_url(std::string_view url);
// serverUrl with its scheme changed to wss (ws for http) and /ws appended (spec 4.10).
std::string websocket_url(const Endpoint& endpoint);

} // namespace medicon::social
