#include <medicon/social/Store.hpp>

#include <algorithm>
#include <type_traits>

namespace medicon::social {

const char* connection_state_name(ConnectionState state) noexcept {
    switch (state) {
    case ConnectionState::NoLicense: return "no license";
    case ConnectionState::Authenticating: return "authenticating";
    case ConnectionState::Connecting: return "connecting";
    case ConnectionState::Online: return "online";
    case ConnectionState::Reconnecting: return "reconnecting";
    case ConnectionState::AuthFailed: return "authentication failed";
    case ConnectionState::Replaced: return "replaced by another connection";
    }
    return "unknown";
}

bool SocialStore::merge_news(NewsKind kind, const NewsItem& item) {
    auto& list = news_[slot(kind)];
    // Newest first: find the first entry with a smaller id.
    const auto position = std::find_if(list.begin(), list.end(), [&](const NewsItem& other) { return other.id <= item.id; });
    if (position != list.end() && position->id == item.id) return false;
    list.insert(position, item);
    if (list.size() > max_news) list.pop_back();
    return true;
}

const Conversation& SocialStore::conversation(ConversationId id) const {
    static const Conversation empty;
    const auto found = conversations_.find(id);
    return found == conversations_.end() ? empty : found->second;
}

Conversation& SocialStore::conversation(ConversationId id) { return conversations_[id]; }

bool SocialStore::merge_message(Conversation& conversation, const ChatMessage& message) {
    auto& entries = conversation.entries;
    // Oldest first; my unanswered messages (id 0) stay at the end.
    const auto position = std::find_if(entries.begin(), entries.end(), [&](const ChatEntry& entry) {
        return entry.message.id == 0 || entry.message.id >= message.id;
    });
    if (position != entries.end() && position->message.id == message.id) return false;
    entries.insert(position, ChatEntry{message});
    if (entries.size() > max_messages) {
        entries.erase(entries.begin());
        conversation.has_more = true;
    }
    return true;
}

void SocialStore::add_pending(ConversationId id, const std::string& nonce, const std::string& body) {
    ChatEntry entry;
    entry.message.body = body;
    entry.message.to = id;
    if (me_) entry.message.author = *me_;
    entry.status = ChatEntry::Status::Pending;
    entry.nonce = nonce;
    auto& target = conversation(id);
    target.entries.push_back(std::move(entry));
    ++target.arrivals;
}

bool SocialStore::retry(ConversationId id, const std::string& old_nonce, const std::string& nonce) {
    auto& entries = conversation(id).entries;
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const ChatEntry& entry) {
        return entry.nonce == old_nonce && entry.status == ChatEntry::Status::Failed;
    });
    if (found == entries.end()) return false;
    // Back to the end, where a fresh send would appear.
    ChatEntry entry = std::move(*found);
    entries.erase(found);
    entry.nonce = nonce;
    entry.status = ChatEntry::Status::Pending;
    entry.error.clear();
    entries.push_back(std::move(entry));
    return true;
}

Friend* SocialStore::friend_entry(std::int64_t id) noexcept {
    const auto found = std::find_if(friends_.begin(), friends_.end(), [&](const Friend& entry) { return entry.user.id == id; });
    return found == friends_.end() ? nullptr : &*found;
}

const Friend* SocialStore::find_friend(std::int64_t id) const noexcept {
    return const_cast<SocialStore*>(this)->friend_entry(id);
}

void SocialStore::clear_unread(std::int64_t id) noexcept {
    if (auto* entry = friend_entry(id)) entry->unread = 0;
}

bool SocialStore::attention() const noexcept {
    return !incoming_.empty() || std::any_of(friends_.begin(), friends_.end(), [](const Friend& entry) { return entry.unread > 0; });
}

void SocialStore::set_relation(std::int64_t user_id, Relation relation) noexcept {
    for (auto& result : search_results_)
        if (result.user.id == user_id) result.relation = relation;
}

namespace {

template <class T> bool erase_user(std::vector<T>& list, std::int64_t id, auto&& key) {
    const auto before = list.size();
    std::erase_if(list, [&](const T& entry) { return key(entry) == id; });
    return list.size() != before;
}

std::int64_t user_id(const UserRef& user) { return user.id; }
std::int64_t friend_id(const Friend& entry) { return entry.user.id; }

} // namespace

bool SocialStore::apply(const Event& event) {
    return std::visit([this](const auto& value) -> bool {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, StateChanged>) {
            state_known_ = true;
            const bool forget_account = value.state == ConnectionState::NoLicense || value.state == ConnectionState::AuthFailed;
            const bool changed = state_ != value.state || state_reason_ != value.reason || (forget_account && me_.has_value());
            if (!changed) return false;
            state_ = value.state;
            state_reason_ = value.reason;
            // The account is unknown until the next hello after a failed sign-in,
            // and nothing of the old account's chats or friends stays on screen.
            if (forget_account) {
                me_.reset();
                conversations_.clear();
                friends_.clear();
                incoming_.clear();
                outgoing_.clear();
                friends_loaded_ = false;
                search_results_.clear();
                search_query_.clear();
            }
            return true;
        } else if constexpr (std::is_same_v<T, SessionStarted>) {
            me_ = value.me;
            return true;
        } else if constexpr (std::is_same_v<T, NewsPosted>) {
            return merge_news(value.item.kind, value.item);
        } else if constexpr (std::is_same_v<T, NewsPage>) {
            const auto index = slot(value.kind);
            bool changed = loading_[index];
            loading_[slot(value.kind)] = false;
            if (value.failed) return true;
            // Only an older page says anything about the end; the newest page of a
            // reconnect leaves what scrolling already learned.
            if (value.before || value.items.size() < static_cast<std::size_t>(value.limit)) {
                const bool more = value.items.size() >= static_cast<std::size_t>(value.limit);
                changed = changed || has_more_[index] != more;
                has_more_[index] = more;
            }
            for (const auto& item : value.items) {
                // The page was requested for one kind; ignore anything else.
                if (item.kind == value.kind) changed = merge_news(value.kind, item) || changed;
            }
            return changed;
        } else if constexpr (std::is_same_v<T, SettingsLoaded>) {
            notify_ = value.notify;
            debug_timing_ = value.debug_timing;
            return true;
        } else if constexpr (std::is_same_v<T, MessageReceived>) {
            auto& target = conversation(value.conversation);
            if (!merge_message(target, value.message)) return false;
            ++target.arrivals;
            const bool mine = me_ && value.message.author.id == me_->id;
            if (value.conversation != global_conversation && !mine && value.conversation != open_dm_) {
                if (auto* entry = friend_entry(value.conversation)) ++entry->unread;
            }
            return true;
        } else if constexpr (std::is_same_v<T, MessagePage>) {
            auto& target = conversation(value.conversation);
            target.loading = false;
            if (value.failed) {
                target.error = value.error;
                return true;
            }
            target.error.clear();
            if (!value.before) target.loaded = true;
            // As with news: only an older page, or a short one, says where history ends.
            if (value.before || value.items.size() < static_cast<std::size_t>(value.limit))
                target.has_more = value.items.size() >= static_cast<std::size_t>(value.limit);
            for (const auto& message : value.items) merge_message(target, message);
            return true;
        } else if constexpr (std::is_same_v<T, RequestFinished>) {
            for (auto& [id, target] : conversations_) {
                auto& entries = target.entries;
                const auto found = std::find_if(entries.begin(), entries.end(),
                                                [&](const ChatEntry& entry) { return entry.nonce == value.nonce; });
                if (found == entries.end()) continue;
                if (!value.ok || !value.message) {
                    found->status = ChatEntry::Status::Failed;
                    found->error = value.ok ? "internal" : value.error;
                    return true;
                }
                entries.erase(found);
                // global.new may have delivered the stored message already.
                merge_message(target, *value.message);
                return true;
            }
            return true; // friend requests: the screen reads the result
        } else if constexpr (std::is_same_v<T, ReadBy>) {
            auto* entry = friend_entry(value.by);
            if (!entry || entry->their_read_up_to >= value.up_to) return false;
            entry->their_read_up_to = value.up_to;
            return true;
        } else if constexpr (std::is_same_v<T, FriendsLoaded>) {
            if (value.failed) return false;
            friends_ = value.list.friends;
            incoming_ = value.list.incoming;
            outgoing_ = value.list.outgoing;
            friends_loaded_ = true;
            if (open_dm_ != 0) clear_unread(open_dm_);
            return true;
        } else if constexpr (std::is_same_v<T, FriendRequested>) {
            if (std::any_of(incoming_.begin(), incoming_.end(), [&](const UserRef& user) { return user.id == value.from.id; }))
                return false;
            incoming_.push_back(value.from);
            set_relation(value.from.id, Relation::Incoming);
            return true;
        } else if constexpr (std::is_same_v<T, FriendUpdated>) {
            const auto id = value.user.id;
            switch (value.status) {
            case FriendStatus::Accepted:
                erase_user(incoming_, id, user_id);
                erase_user(outgoing_, id, user_id);
                // Presence for a friend who is online follows right after.
                if (!friend_entry(id)) friends_.push_back(Friend{value.user});
                set_relation(id, Relation::Friend);
                break;
            case FriendStatus::Declined:
                erase_user(outgoing_, id, user_id);
                set_relation(id, Relation::None);
                break;
            case FriendStatus::Removed:
                erase_user(friends_, id, friend_id);
                set_relation(id, Relation::None);
                break;
            case FriendStatus::Cancelled:
                erase_user(incoming_, id, user_id);
                set_relation(id, Relation::None);
                break;
            }
            return true;
        } else if constexpr (std::is_same_v<T, PresenceChanged>) {
            auto* entry = friend_entry(value.user_id);
            if (!entry || entry->online == value.online) return false;
            entry->online = value.online;
            return true;
        } else if constexpr (std::is_same_v<T, SearchFinished>) {
            if (value.failed) return false;
            search_query_ = value.query;
            search_results_ = value.items;
            return true;
        } else {
            return true; // LoginFinished, SettingsSaveFailed: for the screen, not the store
        }
    }, event);
}

} // namespace medicon::social
