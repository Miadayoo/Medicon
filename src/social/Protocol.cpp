#include <medicon/social/Protocol.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>

namespace medicon::social {
namespace {

using Json = nlohmann::json;

std::optional<Json> parse_object(std::string_view text) {
    Json json = Json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) return std::nullopt;
    return json;
}

bool read_int(const Json& object, const char* key, std::int64_t& out) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number_integer()) return false;
    out = found->get<std::int64_t>();
    return true;
}

bool read_string(const Json& object, const char* key, std::string& out) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) return false;
    out = found->get<std::string>();
    return true;
}

std::optional<UserRef> read_user(const Json& value) {
    UserRef user;
    if (!value.is_object() || !read_int(value, "id", user.id) || !read_string(value, "name", user.name))
        return std::nullopt;
    std::string rank;
    if (read_string(value, "rank", rank) && rank == "owner") user.rank = Rank::Owner;
    return user;
}

std::optional<NewsItem> read_news(const Json& value) {
    if (!value.is_object()) return std::nullopt;
    NewsItem item;
    std::string kind;
    if (!read_int(value, "id", item.id) || !read_string(value, "kind", kind) ||
        !read_string(value, "title", item.title) || !read_string(value, "body", item.body) ||
        !read_int(value, "createdAt", item.created_at))
        return std::nullopt;
    if (kind == "news") item.kind = NewsKind::News;
    else if (kind == "changelog") item.kind = NewsKind::Changelog;
    else return std::nullopt;
    if (const auto version = value.find("version"); version != value.end() && version->is_string())
        item.version = version->get<std::string>();
    return item;
}

std::optional<ChatMessage> read_message(const Json& value) {
    if (!value.is_object()) return std::nullopt;
    ChatMessage message;
    const auto author = value.find("author");
    if (!read_int(value, "id", message.id) || !read_string(value, "body", message.body) ||
        !read_int(value, "createdAt", message.created_at) || author == value.end())
        return std::nullopt;
    auto user = read_user(*author);
    if (!user) return std::nullopt;
    message.author = std::move(*user);
    read_int(value, "to", message.to); // DMs only
    return message;
}

std::optional<ChatMessage> read_message_field(const Json& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end()) return std::nullopt;
    return read_message(*found);
}

bool read_bool(const Json& object, const char* key, bool& out) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_boolean()) return false;
    out = found->get<bool>();
    return true;
}

std::vector<UserRef> read_users(const Json& object, const char* key) {
    std::vector<UserRef> users;
    const auto found = object.find(key);
    if (found == object.end() || !found->is_array()) return users;
    for (const auto& value : *found)
        if (auto user = read_user(value)) users.push_back(std::move(*user));
    return users;
}

std::string dump(const Json& json) {
    // Replace rather than throw: a malformed byte sequence must not take down a net thread.
    return json.dump(-1, ' ', false, Json::error_handler_t::replace);
}

} // namespace

const char* news_kind_name(NewsKind kind) noexcept {
    return kind == NewsKind::Changelog ? "changelog" : "news";
}

bool valid_model_identity(const ModelIdentity& player) noexcept {
    if (player.actor_id.empty() || player.actor_id.size() > 20 || player.name.empty() || player.name.size() > 128) return false;
    const auto start = player.actor_id.front() == '-' ? 1U : 0U;
    if (start == player.actor_id.size()) return false;
    for (std::size_t i = start; i < player.actor_id.size(); ++i)
        if (player.actor_id[i] < '0' || player.actor_id[i] > '9') return false;
    return std::none_of(player.name.begin(), player.name.end(), [](unsigned char c) { return c < 32 || c == 127; });
}

bool valid_model_id(std::string_view model) noexcept { return model == "neon" || model == "chocolat"; }

std::optional<ServerFrame> parse_server_frame(std::string_view text) {
    const auto json = parse_object(text);
    if (!json) return std::nullopt;
    std::string type;
    if (!read_string(*json, "t", type)) return std::nullopt;
    if (type == "hello") {
        const auto user = json->find("user");
        HelloFrame hello;
        if (user == json->end() || !read_int(*json, "serverTime", hello.server_time)) return std::nullopt;
        auto parsed = read_user(*user);
        if (!parsed) return std::nullopt;
        hello.user = std::move(*parsed);
        return hello;
    }
    if (type == "pong") return PongFrame{};
    if (type == "model.peers") {
        const auto peers = json->find("peers");
        if (peers == json->end() || !peers->is_array() || peers->size() > max_model_peers) return std::nullopt;
        ModelPeersFrame frame;
        for (const auto& item : *peers) {
            PlayerModel peer;
            if (!item.is_object() || !read_string(item, "actorId", peer.player.actor_id) ||
                !read_string(item, "name", peer.player.name) || !valid_model_identity(peer.player) ||
                !read_string(item, "model", peer.model) || !valid_model_id(peer.model) ||
                !read_int(item, "userId", peer.user_id) || peer.user_id <= 0) return std::nullopt;
            if (std::any_of(frame.peers.begin(), frame.peers.end(), [&](const auto& previous) {
                return previous.player == peer.player || previous.user_id == peer.user_id;
            })) return std::nullopt;
            frame.peers.push_back(std::move(peer));
        }
        return frame;
    }
    if (type == "news.new") {
        const auto item = json->find("item");
        if (item == json->end()) return std::nullopt;
        auto parsed = read_news(*item);
        if (!parsed) return std::nullopt;
        return NewsFrame{std::move(*parsed)};
    }
    if (type == "kicked") {
        KickedFrame kicked;
        if (!read_string(*json, "reason", kicked.reason)) return std::nullopt;
        return kicked;
    }
    if (type == "ack") {
        AckFrame ack;
        if (!read_string(*json, "id", ack.id) || !read_bool(*json, "ok", ack.ok)) return std::nullopt;
        if (!ack.ok) read_string(*json, "error", ack.error);
        ack.message = read_message_field(*json, "message");
        return ack;
    }
    if (type == "global.new" || type == "dm.new") {
        auto message = read_message_field(*json, "message");
        if (!message) return std::nullopt;
        if (type == "dm.new") return DmNewFrame{std::move(*message)};
        return GlobalNewFrame{std::move(*message)};
    }
    if (type == "dm.read") {
        DmReadFrame read;
        if (!read_int(*json, "by", read.by) || !read_int(*json, "upTo", read.up_to)) return std::nullopt;
        return read;
    }
    if (type == "friend.request") {
        const auto from = json->find("from");
        if (from == json->end()) return std::nullopt;
        auto user = read_user(*from);
        if (!user) return std::nullopt;
        return FriendRequestFrame{std::move(*user)};
    }
    if (type == "friend.update") {
        const auto user = json->find("user");
        std::string status;
        if (user == json->end() || !read_string(*json, "status", status)) return std::nullopt;
        auto parsed = read_user(*user);
        if (!parsed) return std::nullopt;
        FriendUpdateFrame update{std::move(*parsed)};
        if (status == "accepted") update.status = FriendStatus::Accepted;
        else if (status == "declined") update.status = FriendStatus::Declined;
        else if (status == "removed") update.status = FriendStatus::Removed;
        else if (status == "cancelled") update.status = FriendStatus::Cancelled;
        else return std::nullopt;
        return update;
    }
    if (type == "presence") {
        PresenceFrame presence;
        if (!read_int(*json, "userId", presence.user_id) || !read_bool(*json, "online", presence.online)) return std::nullopt;
        return presence;
    }
    return OtherFrame{std::move(type)};
}

std::string ping_frame() { return dump(Json{{"t", "ping"}}); }

std::string model_sync_frame(const std::optional<PlayerModel>& self, const std::vector<ModelIdentity>& watching) {
    Json frame{{"t", "model.sync"}, {"self", nullptr}, {"watching", Json::array()}};
    if (self && valid_model_identity(self->player) && valid_model_id(self->model))
        frame["self"] = {{"actorId", self->player.actor_id}, {"name", self->player.name}, {"model", self->model}};
    for (const auto& player : watching) {
        if (frame["watching"].size() >= max_model_peers) break;
        if (valid_model_identity(player)) frame["watching"].push_back({{"actorId", player.actor_id}, {"name", player.name}});
    }
    return dump(frame);
}

std::string global_send_frame(std::string_view nonce, std::string_view body) {
    return dump(Json{{"t", "global.send"}, {"id", nonce}, {"body", body}});
}

std::string dm_send_frame(std::string_view nonce, std::int64_t to, std::string_view body) {
    return dump(Json{{"t", "dm.send"}, {"id", nonce}, {"to", to}, {"body", body}});
}

std::string dm_read_frame(std::int64_t with, std::int64_t up_to) {
    return dump(Json{{"t", "dm.read"}, {"with", with}, {"upTo", up_to}});
}

std::string friend_request_frame(std::string_view nonce, std::string_view name) {
    return dump(Json{{"t", "friend.request"}, {"id", nonce}, {"name", name}});
}

std::string friend_respond_frame(std::string_view nonce, std::int64_t user_id, bool accept) {
    return dump(Json{{"t", "friend.respond"}, {"id", nonce}, {"userId", user_id}, {"accept", accept}});
}

std::string friend_remove_frame(std::string_view nonce, std::int64_t user_id) {
    return dump(Json{{"t", "friend.remove"}, {"id", nonce}, {"userId", user_id}});
}

std::string news_post_frame(std::string_view nonce, NewsKind kind, std::string_view title, std::string_view body,
                            std::string_view version) {
    Json frame{{"t", "news.post"}, {"id", nonce}, {"kind", news_kind_name(kind)}, {"title", title}, {"body", body}};
    if (!version.empty()) frame["version"] = version;
    return dump(frame);
}

std::string url_encode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : value) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

std::string login_body(std::string_view key, std::string_view client_version) {
    return dump(Json{{"key", key}, {"clientVersion", client_version}});
}

std::optional<LoginResult> parse_login(std::string_view body) {
    const auto json = parse_object(body);
    if (!json) return std::nullopt;
    LoginResult result;
    const auto user = json->find("user");
    if (!read_string(*json, "token", result.token) || result.token.empty() ||
        !read_int(*json, "expiresAt", result.expires_at) || user == json->end())
        return std::nullopt;
    auto parsed = read_user(*user);
    if (!parsed) return std::nullopt;
    result.user = std::move(*parsed);
    return result;
}

std::optional<std::vector<NewsItem>> parse_news_page(std::string_view body) {
    const auto json = parse_object(body);
    if (!json) return std::nullopt;
    const auto items = json->find("items");
    if (items == json->end() || !items->is_array()) return std::nullopt;
    std::vector<NewsItem> result;
    result.reserve(items->size());
    for (const auto& value : *items) {
        // Skip an entry this client cannot read instead of dropping the whole page.
        if (auto item = read_news(value)) result.push_back(std::move(*item));
    }
    return result;
}

std::optional<std::vector<ChatMessage>> parse_messages(std::string_view body) {
    const auto json = parse_object(body);
    if (!json) return std::nullopt;
    const auto items = json->find("items");
    if (items == json->end() || !items->is_array()) return std::nullopt;
    std::vector<ChatMessage> result;
    result.reserve(items->size());
    for (const auto& value : *items)
        if (auto message = read_message(value)) result.push_back(std::move(*message));
    return result;
}

std::optional<FriendList> parse_friends(std::string_view body) {
    const auto json = parse_object(body);
    if (!json) return std::nullopt;
    const auto friends = json->find("friends");
    if (friends == json->end() || !friends->is_array()) return std::nullopt;
    FriendList list;
    for (const auto& value : *friends) {
        auto user = read_user(value);
        if (!user) continue;
        Friend entry{std::move(*user)};
        read_bool(value, "online", entry.online);
        std::int64_t unread = 0;
        read_int(value, "unread", unread);
        entry.unread = static_cast<int>(std::max<std::int64_t>(0, unread));
        read_int(value, "theirReadUpTo", entry.their_read_up_to);
        list.friends.push_back(std::move(entry));
    }
    list.incoming = read_users(*json, "incoming");
    list.outgoing = read_users(*json, "outgoing");
    return list;
}

std::optional<std::vector<SearchResult>> parse_search(std::string_view body) {
    const auto json = parse_object(body);
    if (!json) return std::nullopt;
    const auto items = json->find("items");
    if (items == json->end() || !items->is_array()) return std::nullopt;
    std::vector<SearchResult> result;
    for (const auto& value : *items) {
        auto user = read_user(value);
        if (!user) continue;
        SearchResult item{std::move(*user)};
        std::string relation;
        read_string(value, "relation", relation);
        if (relation == "friend") item.relation = Relation::Friend;
        else if (relation == "outgoing") item.relation = Relation::Outgoing;
        else if (relation == "incoming") item.relation = Relation::Incoming;
        result.push_back(std::move(item));
    }
    return result;
}

std::string parse_error_code(std::string_view body) {
    std::string code;
    if (const auto json = parse_object(body)) read_string(*json, "error", code);
    return code;
}

} // namespace medicon::social
