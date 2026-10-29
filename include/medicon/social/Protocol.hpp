#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace medicon::social {

// Wire types shared with social-server (spec section 2). IDs are server-assigned
// int64 values; times are Unix epoch milliseconds.
// Owners may post News and Change Log from the client (Admin tab).
enum class Rank { User, Owner };

struct UserRef {
    std::int64_t id = 0;
    std::string name;
    Rank rank = Rank::User; // sent for me (hello) and message authors; User elsewhere
    bool operator==(const UserRef&) const = default;
};

enum class NewsKind { News, Changelog };
inline constexpr std::size_t news_kind_count = 2;
const char* news_kind_name(NewsKind kind) noexcept; // "news" / "changelog" on the wire

struct NewsItem {
    std::int64_t id = 0;
    NewsKind kind = NewsKind::News;
    std::string title;
    std::string body;
    std::optional<std::string> version;
    std::int64_t created_at = 0;
    bool operator==(const NewsItem&) const = default;
};

// A Global message, or a DM when `to` is set (spec 2.4).
struct ChatMessage {
    std::int64_t id = 0;
    UserRef author;
    std::string body;
    std::int64_t created_at = 0;
    std::int64_t to = 0; // DM recipient; 0 for Global
    bool operator==(const ChatMessage&) const = default;
};

struct Friend {
    UserRef user;
    bool online = false;
    int unread = 0;
    std::int64_t their_read_up_to = 0; // how far they read my messages
    bool operator==(const Friend&) const = default;
};

struct FriendList {
    std::vector<Friend> friends;
    std::vector<UserRef> incoming;
    std::vector<UserRef> outgoing;
};

// How another user relates to me (spec 2.3 /users/search).
enum class Relation { None, Friend, Outgoing, Incoming };

struct SearchResult {
    UserRef user;
    Relation relation = Relation::None;
};

enum class FriendStatus { Accepted, Declined, Removed, Cancelled };

// --- Server -> client WebSocket frames ----------------------------------------

struct HelloFrame {
    UserRef user;
    std::int64_t server_time = 0;
};
struct PongFrame {};
struct NewsFrame {
    NewsItem item;
};
struct KickedFrame {
    std::string reason; // "revoked" or "banned"
};
// The answer to a request carrying `id`; sends include the stored message.
struct AckFrame {
    std::string id;
    bool ok = false;
    std::string error;
    std::optional<ChatMessage> message;
};
struct GlobalNewFrame {
    ChatMessage message;
};
struct DmNewFrame {
    ChatMessage message;
};
struct DmReadFrame {
    std::int64_t by = 0;
    std::int64_t up_to = 0;
};
struct FriendRequestFrame {
    UserRef from;
};
struct FriendUpdateFrame {
    UserRef user;
    FriendStatus status = FriendStatus::Accepted;
};
struct PresenceFrame {
    std::int64_t user_id = 0;
    bool online = false;
};
// A server actor id is a decimal string: int64 ids must not pass through a JS number.
struct ModelIdentity {
    std::string actor_id, name;
    bool operator==(const ModelIdentity&) const = default;
};
struct PlayerModel {
    ModelIdentity player;
    std::string model; // Allowlisted bundled model id, never a URL or a file path.
    std::int64_t user_id{};
    bool operator==(const PlayerModel&) const = default;
};
inline constexpr std::size_t max_model_peers = 64;
bool valid_model_identity(const ModelIdentity& player) noexcept;
bool valid_model_id(std::string_view model) noexcept;
struct ModelPeersFrame { std::vector<PlayerModel> peers; };
// A type this client does not handle yet. Newer servers stay compatible.
struct OtherFrame {
    std::string type;
};
using ServerFrame = std::variant<HelloFrame, PongFrame, NewsFrame, KickedFrame, AckFrame, GlobalNewFrame, DmNewFrame,
                                 DmReadFrame, FriendRequestFrame, FriendUpdateFrame, PresenceFrame, ModelPeersFrame, OtherFrame>;

// nullopt when the text is not a JSON object with a string "t", or when a known
// type lacks a required field.
std::optional<ServerFrame> parse_server_frame(std::string_view text);

// --- Client -> server ----------------------------------------------------------

std::string ping_frame();
std::string model_sync_frame(const std::optional<PlayerModel>& self, const std::vector<ModelIdentity>& watching);
std::string login_body(std::string_view key, std::string_view client_version);
// `nonce` comes back in the ack.
std::string global_send_frame(std::string_view nonce, std::string_view body);
std::string dm_send_frame(std::string_view nonce, std::int64_t to, std::string_view body);
std::string dm_read_frame(std::int64_t with, std::int64_t up_to);
std::string friend_request_frame(std::string_view nonce, std::string_view name);
std::string friend_respond_frame(std::string_view nonce, std::int64_t user_id, bool accept);
std::string friend_remove_frame(std::string_view nonce, std::int64_t user_id);
// Owners only. An empty version is left out.
std::string news_post_frame(std::string_view nonce, NewsKind kind, std::string_view title, std::string_view body,
                            std::string_view version);
// Percent-encodes a query parameter value.
std::string url_encode(std::string_view value);

// --- REST responses --------------------------------------------------------------

struct LoginResult {
    std::string token;
    std::int64_t expires_at = 0;
    UserRef user;
};
std::optional<LoginResult> parse_login(std::string_view body);
std::optional<std::vector<NewsItem>> parse_news_page(std::string_view body);
// {items: [Message]} of /global/messages and /dm/{id}/messages, newest first.
std::optional<std::vector<ChatMessage>> parse_messages(std::string_view body);
std::optional<FriendList> parse_friends(std::string_view body);
std::optional<std::vector<SearchResult>> parse_search(std::string_view body);
// The code of an {"error": code} body, or an empty string.
std::string parse_error_code(std::string_view body);

// Close codes (spec 2.6).
inline constexpr std::uint16_t close_going_away = 1001;
inline constexpr std::uint16_t close_token_invalid = 4001;
inline constexpr std::uint16_t close_kicked = 4003;
inline constexpr std::uint16_t close_replaced = 4009;

} // namespace medicon::social
