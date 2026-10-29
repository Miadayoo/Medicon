#pragma once

#include "medicon/social/Config.hpp"
#include "medicon/social/Protocol.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace medicon::social {

// Spec 4.4. Replaced is the 4009 case: another client took over this account.
enum class ConnectionState { NoLicense, Authenticating, Connecting, Online, Reconnecting, AuthFailed, Replaced };
const char* connection_state_name(ConnectionState state) noexcept;

// --- Net threads -> render thread --------------------------------------------
// Parsing is finished on the net side; the render thread only copies values.

struct StateChanged {
    ConnectionState state = ConnectionState::NoLicense;
    std::string reason; // server error code for AuthFailed: invalid_key, revoked, banned
};
struct SessionStarted {
    UserRef me;
    std::int64_t server_time = 0;
};
struct NewsPosted {
    NewsItem item;
};
struct NewsPage {
    NewsKind kind = NewsKind::News;
    std::optional<std::int64_t> before; // nullopt: the newest page
    std::vector<NewsItem> items;
    int limit = 20;      // a shorter page means there is nothing older
    bool failed = false; // the request did not succeed; items is empty
};
struct SettingsLoaded {
    NotifySettings notify;
    bool debug_timing = false;
    bool debug_toast_slow_motion = false;
    bool music_enabled = true;
};
// The answer to a Login command.
struct LoginFinished {
    bool ok = false;
    std::string error; // invalid_key, revoked, banned, rate_limited, unreachable, bad_server_url
};
// A failed deletion leaves the encrypted license on disk and must be shown to
// the user; no credential or filesystem error text is included in the event.
struct LogoutFinished {
    bool credential_removed = false;
};
// social.json is not a JSON object, so it was left as it is.
struct SettingsSaveFailed {};

// Conversation 0 is Global; any other value is the friend's user id for a DM.
using ConversationId = std::int64_t;
inline constexpr ConversationId global_conversation = 0;

// global.new / dm.new. A DM I sent from elsewhere never arrives here.
struct MessageReceived {
    ConversationId conversation = global_conversation;
    ChatMessage message;
};
// A history page (newest first), as asked for by FetchMessages.
struct MessagePage {
    ConversationId conversation = global_conversation;
    std::optional<std::int64_t> before;
    std::vector<ChatMessage> items;
    int limit = 50;
    bool failed = false;
    std::string error; // not_friends when the DM is no longer allowed
};
// The ack of a request this client sent with `nonce` (send, friend changes).
struct RequestFinished {
    std::string nonce;
    bool ok = false;
    std::string error; // rate_limited, empty, too_long, not_friends, offline, ...
    std::optional<ChatMessage> message;
};
struct ReadBy {
    std::int64_t by = 0;
    std::int64_t up_to = 0;
};
struct FriendsLoaded {
    FriendList list;
    bool failed = false;
};
struct FriendRequested {
    UserRef from;
};
struct FriendUpdated {
    UserRef user;
    FriendStatus status = FriendStatus::Accepted;
};
struct PresenceChanged {
    std::int64_t user_id = 0;
    bool online = false;
};
struct SearchFinished {
    std::string query;
    std::vector<SearchResult> items;
    bool failed = false;
};
using Event = std::variant<StateChanged, SessionStarted, NewsPosted, NewsPage, SettingsLoaded, LoginFinished, LogoutFinished,
                           SettingsSaveFailed, MessageReceived, MessagePage, RequestFinished, ReadBy, FriendsLoaded,
                           FriendRequested, FriendUpdated, PresenceChanged, SearchFinished>;

// --- Render thread -> REST worker --------------------------------------------

struct FetchNews {
    NewsKind kind = NewsKind::News;
    std::optional<std::int64_t> before;
    int limit = 20;
};
// Global (conversation 0) or DM history, 50 newest before `before`.
struct FetchMessages {
    ConversationId conversation = global_conversation;
    std::optional<std::int64_t> before;
    int limit = 50;
};
struct FetchFriends {};
struct SearchUsers {
    std::string query;
};
// Owners: upload a Core DLL and post its Change Log in one request
// (docs/auto-update.md). The REST worker reads dll_path and POSTs /releases;
// answered by RequestFinished with the same nonce. Here, not Command, because
// it is a large HTTPS upload, not a small WebSocket frame.
struct PublishRelease {
    std::string nonce;
    std::string version;
    std::string title;
    std::string body;
    std::wstring dll_path;
};
using ApiRequest = std::variant<FetchNews, FetchMessages, FetchFriends, SearchUsers, PublishRelease>;

// --- Render thread -> send thread --------------------------------------------

// Signs in with a key typed on the login screen; saved to license.bin only once
// the server accepts it.
struct Login {
    std::string key;
};
// Deletes license.bin and disconnects.
struct Logout {};
// Tries again after "connected elsewhere" (close 4009).
struct Reconnect {};
struct SaveSettings {
    NotifySettings notify;
    bool music_enabled = true;
};
// Requests answered by RequestFinished with the same nonce. While offline they
// fail at once with "offline".
struct SendChat {
    std::string nonce;
    ConversationId conversation = global_conversation;
    std::string body;
};
struct MarkRead {
    std::int64_t with = 0;
    std::int64_t up_to = 0;
};
struct RequestFriend {
    std::string nonce;
    std::string name;
};
struct RespondFriend {
    std::string nonce;
    std::int64_t user_id = 0;
    bool accept = false;
};
// Removes a friend, or cancels my own pending request.
struct RemoveFriend {
    std::string nonce;
    std::int64_t user_id = 0;
};
// Owners: News or Change Log for everyone (answered by RequestFinished).
struct PostNews {
    std::string nonce;
    NewsKind kind = NewsKind::News;
    std::string title;
    std::string body;
    std::string version;
};
using Command = std::variant<Login, Logout, Reconnect, SaveSettings, SendChat, MarkRead, RequestFriend, RespondFriend,
                             RemoveFriend, PostNews>;

// Many producers, one consumer: the render thread (spec 4.3). An idle frame costs
// one atomic load; a frame with events swaps vectors under the lock.
template <class T>
class Inbox {
public:
    explicit Inbox(std::size_t limit = 1024) : limit_(limit) {}

    void push(T value) {
        std::lock_guard lock(mutex_);
        // A window that stops presenting (minimized) must not grow this forever.
        if (items_.size() >= limit_) items_.erase(items_.begin());
        items_.push_back(std::move(value));
        pending_.store(true, std::memory_order_release);
    }

    // `out` must be empty; its capacity is handed back for the next batch.
    bool drain(std::vector<T>& out) {
        if (!pending_.load(std::memory_order_acquire)) return false;
        std::lock_guard lock(mutex_);
        out.swap(items_);
        pending_.store(false, std::memory_order_relaxed);
        return !out.empty();
    }

private:
    std::mutex mutex_;
    std::vector<T> items_;
    std::atomic_bool pending_{false};
    std::size_t limit_;
};

// One consumer thread sleeping on a condition variable, never polling.
template <class T>
class Outbox {
public:
    void push(T value) {
        {
            std::lock_guard lock(mutex_);
            if (closed_) return;
            items_.push_back(std::move(value));
        }
        ready_.notify_one();
    }

    // Waits for an item, the deadline, or close(). nullopt for the last two.
    std::optional<T> pop(std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock(mutex_);
        ready_.wait_until(lock, deadline, [this] { return closed_ || !items_.empty(); });
        return take();
    }

    // Waits for an item or close(); nullopt only after close().
    std::optional<T> pop() {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [this] { return closed_ || !items_.empty(); });
        return take();
    }

    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
            items_.clear();
        }
        ready_.notify_all();
    }

private:
    std::optional<T> take() {
        if (closed_ || items_.empty()) return std::nullopt;
        T value = std::move(items_.front());
        items_.pop_front();
        return value;
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<T> items_;
    bool closed_ = false;
};

// The only surface shared between the network service and the render thread.
struct Channel {
    Inbox<Event> events;
    Outbox<ApiRequest> api;
    Outbox<Command> commands;
};

} // namespace medicon::social
