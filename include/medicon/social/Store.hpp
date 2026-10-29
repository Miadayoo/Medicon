#pragma once

#include "medicon/social/Events.hpp"

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace medicon::social {

// One line of a conversation. My own messages appear at once as Pending
// (spec 4.6: 50 % until the ack) and turn Sent or Failed.
struct ChatEntry {
    enum class Status { Sent, Pending, Failed };
    ChatMessage message;   // id is 0 until the ack
    Status status = Status::Sent;
    std::string nonce;     // my requests only
    std::string error;     // why it failed
};

// Global (id 0) or one DM, oldest first.
struct Conversation {
    std::vector<ChatEntry> entries;
    bool has_more = true;  // older history may exist
    bool loading = false;  // a FetchMessages is in flight
    bool loaded = false;   // the newest page arrived at least once
    std::string error;     // not_friends
    // Counts arrivals, so a view can tell new messages from a page of history.
    std::uint32_t arrivals = 0;
};

// Everything Social views show (spec 4.5). Render thread only: events are
// copied in at the start of a frame and nothing here locks.
class SocialStore {
public:
    // Upper bound per news kind; older pages can be fetched again.
    static constexpr std::size_t max_news = 200;

    // Returns false when the event changes nothing visible (a duplicate news
    // item delivered by both news.new and a page fetch, for example).
    bool apply(const Event& event);

    ConnectionState state() const noexcept { return state_; }
    // The net side reported a state at least once. Until then state() is the
    // NoLicense default, not an observation.
    bool state_known() const noexcept { return state_known_; }
    const std::string& state_reason() const noexcept { return state_reason_; }
    const std::optional<UserRef>& me() const noexcept { return me_; }
    // Newest first.
    const std::vector<NewsItem>& news(NewsKind kind) const noexcept { return news_[slot(kind)]; }
    // An older page may exist (false once a page came back shorter than asked for).
    bool has_more(NewsKind kind) const noexcept { return has_more_[slot(kind)]; }
    // A page request is in flight; set by whoever sends FetchNews.
    bool loading(NewsKind kind) const noexcept { return loading_[slot(kind)]; }
    void set_loading(NewsKind kind, bool loading) noexcept { loading_[slot(kind)] = loading; }
    const NotifySettings& notify() const noexcept { return notify_; }
    // The settings screen applies a change at once; saving happens on the net side.
    void set_notify(const NotifySettings& notify) noexcept { notify_ = notify; }
    bool music_enabled() const noexcept { return music_enabled_; }
    void set_music_enabled(bool enabled) noexcept { music_enabled_ = enabled; }
    bool debug_timing() const noexcept { return debug_timing_; }

    // --- Chat (phases 4-6) ----------------------------------------------------
    static constexpr std::size_t max_messages = 500; // per conversation; older pages can be fetched again
    // Never null: an empty conversation is created on first use.
    const Conversation& conversation(ConversationId id) const;
    Conversation& conversation(ConversationId id);
    // My message, shown before the server answers.
    void add_pending(ConversationId id, const std::string& nonce, const std::string& body);
    // A failed message sent again under a new nonce.
    bool retry(ConversationId id, const std::string& old_nonce, const std::string& nonce);

    const std::vector<Friend>& friends() const noexcept { return friends_; }
    const Friend* find_friend(std::int64_t id) const noexcept;
    const std::vector<UserRef>& incoming() const noexcept { return incoming_; }
    const std::vector<UserRef>& outgoing() const noexcept { return outgoing_; }
    bool friends_loaded() const noexcept { return friends_loaded_; }
    // The DM on screen: its new messages are read at once and never counted as unread.
    void set_open_dm(std::int64_t id) noexcept { open_dm_ = id; }
    std::int64_t open_dm() const noexcept { return open_dm_; }
    void clear_unread(std::int64_t id) noexcept;
    // Unread DMs or pending requests: the Main Menu badge (spec 4.6).
    bool attention() const noexcept;

    const std::string& search_query() const noexcept { return search_query_; }
    const std::vector<SearchResult>& search_results() const noexcept { return search_results_; }
    // Applies a request's result to the search rows before the server's push arrives.
    void set_relation(std::int64_t user_id, Relation relation) noexcept;

private:
    bool merge_message(Conversation& conversation, const ChatMessage& message);
    Friend* friend_entry(std::int64_t id) noexcept;
    static std::size_t slot(NewsKind kind) noexcept { return static_cast<std::size_t>(kind); }
    bool merge_news(NewsKind kind, const NewsItem& item);

    ConnectionState state_ = ConnectionState::NoLicense;
    bool state_known_ = false;
    std::string state_reason_;
    std::optional<UserRef> me_;
    std::array<std::vector<NewsItem>, news_kind_count> news_{};
    std::array<bool, news_kind_count> has_more_{true, true};
    std::array<bool, news_kind_count> loading_{};
    NotifySettings notify_{};
    bool music_enabled_ = true;
    bool debug_timing_ = false;
    std::map<ConversationId, Conversation> conversations_;
    std::vector<Friend> friends_;
    std::vector<UserRef> incoming_, outgoing_;
    bool friends_loaded_ = false;
    std::int64_t open_dm_ = 0;
    std::string search_query_;
    std::vector<SearchResult> search_results_;
};

} // namespace medicon::social
