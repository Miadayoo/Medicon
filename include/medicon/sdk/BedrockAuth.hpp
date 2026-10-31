#pragma once

#include <medicon/memory/pattern.hpp>

#include <cstdint>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace medicon::sdk {

// The signed-in Xbox account read from a Social::XboxLiveIdentity. Copied bytes
// only; this never calls or mutates a game-owned object.
struct AccountIdentity {
    std::string xuid;     // decimal, as the game stores it (XboxLiveIdentity +0x30)
    std::string gamertag; // XboxLiveIdentity +0x1D8
};

namespace detail {
// Observed 1.26.52.03 Social::XboxLiveIdentity layout (docs/alt-manager-*). Bytes
// are read in-process with bounded reads; a malformed layout yields nullopt and
// never a partial or aliasing result. `xbox_identity` points at the identity object.
inline constexpr std::size_t xbox_identity_xuid_offset = 0x30;
inline constexpr std::size_t xbox_identity_gamertag_offset = 0x1D8;
std::optional<AccountIdentity> read_account_identity(const void* xbox_identity) noexcept;

struct ConnectionAuth {
    std::array<std::byte, 16> prefix{};
    std::string token;
};
std::optional<ConnectionAuth> read_connection_auth(const void* auth) noexcept;
// Borrowed MSVC string view for the synchronous, read-only native toJson call.
// This object has no foreign destructor and never transfers ownership.
struct ConnectionAuthView {
    alignas(8) std::array<std::byte, 48> bytes{};
    ConnectionAuthView(const ConnectionAuth& original, const std::string& token) noexcept;
};
} // namespace detail

// Resolves the documented auth signatures and validates the exact executable
// profile used by the server-login serializer hook. Game-owned objects are read
// as bounded byte snapshots, without sharing C++ allocation ownership.
class BedrockAuth {
public:
    enum class State { waiting, ready, unsupported };

    // Scans the auth catalog once. Read-only; safe to call while other SDKs run.
    State locate(std::string& diagnostic);
    [[nodiscard]] State state() const noexcept { return state_; }

    // Research addresses (0 when unavailable). Only login_serialization_target
    // applies the executable identity guard required for the installed hook.
    [[nodiscard]] std::uintptr_t connection_request_create() const noexcept { return connection_request_create_; }
    [[nodiscard]] std::uintptr_t connection_auth_info_to_json() const noexcept { return connection_auth_info_to_json_; }
    [[nodiscard]] std::uintptr_t user_impl_create() const noexcept { return user_impl_create_; }
    [[nodiscard]] std::uintptr_t get_global_token() const noexcept { return get_global_token_; }
    [[nodiscard]] std::uintptr_t start_multiplayer_session() const noexcept { return start_multiplayer_session_; }
    // Exact PE identity and resolved RVA for the reviewed two-argument ABI.
    [[nodiscard]] std::uintptr_t login_serialization_target(std::string& diagnostic) const;

private:
    State state_ = State::waiting;
    bool scanned_ = false;
    std::string terminal_diagnostic_;
    std::uintptr_t connection_request_create_ = 0;
    std::uintptr_t connection_auth_info_to_json_ = 0;
    std::uintptr_t user_impl_create_ = 0;
    std::uintptr_t get_global_token_ = 0;
    std::uintptr_t start_multiplayer_session_ = 0;
};

} // namespace medicon::sdk
