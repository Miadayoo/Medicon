#pragma once

#include <medicon/alt/Auth.hpp>

namespace medicon::alt {

// Service-owned credentials; never sent through the UI channel.
struct SessionResult {
    std::string authorization;
    std::string error;
    std::string diagnostic;
};
SessionResult prepare_session(SignInHost& host, Credentials& account, std::string_view application_id,
                              std::string_view device_id, std::string_view game_version);

struct ServerTokenResult {
    std::string token;
    std::string error;
    std::string diagnostic;
};
// Extracts only the public signing key. No game-owned private key is read.
std::optional<std::string> server_public_key(std::string_view token);
std::string server_token_xuid(std::string_view token);
bool valid_server_token(std::string_view token, std::int64_t now_seconds);
// Exact credential destination. Never route a service token to a server's
// signaling endpoint, another host, a redirect, or a URL containing userinfo.
bool native_multiplayer_url(std::string_view url) noexcept;
ServerTokenResult authorize_connection(SignInHost& host, std::string_view authorization,
                                      std::string_view native_token, std::int64_t now_seconds);
} // namespace medicon::alt
