#include "medicon/social/Backoff.hpp"
#include "medicon/social/Config.hpp"
#include "medicon/social/Events.hpp"
#include "medicon/social/Protocol.hpp"
#include "medicon/social/Store.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace medicon::social;
using namespace std::chrono_literals;

int checks = 0;
void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

NewsItem news(std::int64_t id, NewsKind kind = NewsKind::News) {
    return NewsItem{id, kind, "title " + std::to_string(id), "body", std::nullopt, id};
}

void test_server_frames() {
    auto frame = parse_server_frame(R"({"t":"hello","user":{"id":5,"name":"faokun"},"serverTime":1761500000000})");
    check(frame && std::holds_alternative<HelloFrame>(*frame), "hello parses");
    const auto& hello = std::get<HelloFrame>(*frame);
    check(hello.user == UserRef{5, "faokun"} && hello.server_time == 1761500000000, "hello fields");

    frame = parse_server_frame(R"({"t":"news.new","item":{"id":3,"kind":"changelog","title":"t","body":"b\nc","version":"1.2.0","createdAt":7}})");
    check(frame && std::holds_alternative<NewsFrame>(*frame), "news.new parses");
    const auto& item = std::get<NewsFrame>(*frame).item;
    check(item.id == 3 && item.kind == NewsKind::Changelog && item.body == "b\nc" && item.version == "1.2.0" &&
          item.created_at == 7, "news.new fields");
    frame = parse_server_frame(R"({"t":"news.new","item":{"id":4,"kind":"news","title":"t","body":"b","version":null,"createdAt":8}})");
    check(frame && !std::get<NewsFrame>(*frame).item.version, "a null version is absent");

    check(std::holds_alternative<PongFrame>(*parse_server_frame(R"({"t":"pong"})")), "pong");
    check(std::get<KickedFrame>(*parse_server_frame(R"({"t":"kicked","reason":"banned"})")).reason == "banned", "kicked");
    check(std::get<OtherFrame>(*parse_server_frame(R"({"t":"message.deleted","scope":"global","id":1})")).type ==
              "message.deleted", "frames of later phases pass through as OtherFrame");
    check(!parse_server_frame(R"({"t":"global.new","message":{}})"), "a message without its fields is rejected");

    for (const char* text : {"", "not json", "[]", R"({"t":5})", R"({"x":"hello"})",
                             R"({"t":"hello","user":{"id":"5","name":"a"},"serverTime":1})",
                             R"({"t":"hello","user":{"id":5,"name":"a"}})",
                             R"({"t":"news.new","item":{"id":1,"kind":"other","title":"t","body":"b","createdAt":1}})",
                             R"({"t":"news.new"})", R"({"t":"kicked"})", "{\"t\":\"pong\",\"x\":\"\xFF\"}"}) {
        check(!parse_server_frame(text), std::string("malformed frame accepted: ") + text);
    }
}

void test_chat_frames() {
    const char* message = R"({"id":7,"author":{"id":5,"name":"fao"},"body":"こんにちは","createdAt":9})";
    auto frame = parse_server_frame(std::string(R"({"t":"global.new","message":)") + message + "}");
    check(frame && std::holds_alternative<GlobalNewFrame>(*frame), "global.new parses");
    const auto& global = std::get<GlobalNewFrame>(*frame).message;
    check(global.id == 7 && global.author == UserRef{5, "fao"} && global.body == "こんにちは" && global.to == 0,
          "global.new fields");
    frame = parse_server_frame(R"({"t":"dm.new","message":{"id":8,"author":{"id":5,"name":"fao"},"to":6,"body":"b","createdAt":9}})");
    check(frame && std::get<DmNewFrame>(*frame).message.to == 6, "dm.new carries the recipient");
    frame = parse_server_frame(std::string(R"({"t":"ack","id":"g0-1","ok":true,"message":)") + message + "}");
    check(frame && std::get<AckFrame>(*frame).ok && std::get<AckFrame>(*frame).message->id == 7, "an ack with its message");
    frame = parse_server_frame(R"({"t":"ack","id":"f1","ok":false,"error":"rate_limited"})");
    check(frame && !std::get<AckFrame>(*frame).ok && std::get<AckFrame>(*frame).error == "rate_limited" &&
          !std::get<AckFrame>(*frame).message, "a failed ack");
    frame = parse_server_frame(R"({"t":"dm.read","by":6,"upTo":8})");
    check(frame && std::get<DmReadFrame>(*frame).up_to == 8, "dm.read");
    frame = parse_server_frame(R"({"t":"friend.request","from":{"id":6,"name":"bob"}})");
    check(frame && std::get<FriendRequestFrame>(*frame).from.name == "bob", "friend.request");
    frame = parse_server_frame(R"({"t":"friend.update","user":{"id":6,"name":"bob"},"status":"cancelled"})");
    check(frame && std::get<FriendUpdateFrame>(*frame).status == FriendStatus::Cancelled, "friend.update");
    check(!parse_server_frame(R"({"t":"friend.update","user":{"id":6,"name":"bob"},"status":"maybe"})"),
          "an unknown friend status is rejected");
    frame = parse_server_frame(R"({"t":"presence","userId":6,"online":true})");
    check(frame && std::get<PresenceFrame>(*frame).online, "presence");

    check(global_send_frame("g0-1", "hi") == R"({"body":"hi","id":"g0-1","t":"global.send"})", "global.send");
    check(dm_send_frame("d6-1", 6, "hi") == R"({"body":"hi","id":"d6-1","t":"dm.send","to":6})", "dm.send");
    check(dm_read_frame(6, 8) == R"({"t":"dm.read","upTo":8,"with":6})", "dm.read frame");
    check(friend_respond_frame("f2", 6, true) == R"({"accept":true,"id":"f2","t":"friend.respond","userId":6})",
          "friend.respond");
    check(url_encode("a b_ア") == "a%20b_%E3%82%A2", "query values are percent-encoded");

    const auto messages = parse_messages(std::string(R"({"items":[)") + message + R"(,{"bad":1}]})");
    check(messages && messages->size() == 1, "history skips unreadable entries");
    const auto friends = parse_friends(
        R"({"friends":[{"id":6,"name":"bob","online":true,"unread":2,"theirReadUpTo":5}],"incoming":[{"id":7,"name":"cat"}],"outgoing":[]})");
    check(friends && friends->friends.size() == 1 && friends->friends[0].unread == 2 && friends->friends[0].online &&
          friends->friends[0].their_read_up_to == 5 && friends->incoming.size() == 1, "friend list");
    const auto found = parse_search(R"({"items":[{"id":6,"name":"bob","relation":"incoming"}]})");
    check(found && found->size() == 1 && (*found)[0].relation == Relation::Incoming, "search results");
}

void test_chat_store() {
    SocialStore store;
    store.apply(SessionStarted{UserRef{5, "fao"}, 1});
    store.apply(StateChanged{ConnectionState::Online, {}});
    const auto message = [](std::int64_t id, std::int64_t author, std::int64_t to = 0) {
        return ChatMessage{id, UserRef{author, author == 5 ? "fao" : "bob"}, "m" + std::to_string(id), id, to};
    };
    // Global: pages merge by id, oldest first.
    store.apply(MessagePage{global_conversation, std::nullopt, {message(3, 6), message(2, 6)}, 50});
    store.apply(MessageReceived{global_conversation, message(4, 6)});
    check(!store.apply(MessageReceived{global_conversation, message(4, 6)}), "a duplicate message changes nothing");
    const auto& global = store.conversation(global_conversation);
    check(global.loaded && global.entries.size() == 3 && global.entries.front().message.id == 2 &&
          global.entries.back().message.id == 4 && !global.has_more, "Global history oldest first; a short page ends it");
    // My message: pending until its ack, then confirmed by the stored copy.
    store.add_pending(global_conversation, "g0-1", "hello");
    check(store.conversation(global_conversation).entries.back().status == ChatEntry::Status::Pending, "pending at once");
    store.apply(RequestFinished{"g0-1", true, {}, message(5, 5)});
    const auto& confirmed = store.conversation(global_conversation).entries.back();
    check(confirmed.status == ChatEntry::Status::Sent && confirmed.message.id == 5, "the ack confirms it");
    check(!store.apply(MessageReceived{global_conversation, message(5, 5)}) &&
          store.conversation(global_conversation).entries.size() == 4, "and global.new for it adds nothing");
    store.add_pending(global_conversation, "g0-2", "again");
    store.apply(RequestFinished{"g0-2", false, "rate_limited", std::nullopt});
    check(store.conversation(global_conversation).entries.back().status == ChatEntry::Status::Failed &&
          store.conversation(global_conversation).entries.back().error == "rate_limited", "a refusal fails it");
    check(store.retry(global_conversation, "g0-2", "g0-3") &&
          store.conversation(global_conversation).entries.back().status == ChatEntry::Status::Pending, "retry");

    // Friends, unread and read marks.
    FriendList list;
    list.friends.push_back(Friend{UserRef{6, "bob"}, false, 0, 0});
    list.incoming.push_back(UserRef{7, "cat"});
    store.apply(FriendsLoaded{list, false});
    check(store.attention(), "a pending request asks for attention");
    store.apply(MessageReceived{6, message(10, 6, 5)});
    check(store.find_friend(6)->unread == 1, "a DM from bob is unread");
    store.set_open_dm(6);
    store.apply(MessageReceived{6, message(11, 6, 5)});
    check(store.find_friend(6)->unread == 1, "but not while his DM is open");
    store.apply(ReadBy{6, 11});
    check(store.find_friend(6)->their_read_up_to == 11, "dm.read moves the mark");
    check(!store.apply(ReadBy{6, 3}), "never backwards");
    store.apply(PresenceChanged{6, true});
    check(store.find_friend(6)->online, "presence");
    store.apply(FriendUpdated{UserRef{7, "cat"}, FriendStatus::Accepted});
    check(store.incoming().empty() && store.find_friend(7), "accepting moves a request to friends");
    store.apply(FriendUpdated{UserRef{6, "bob"}, FriendStatus::Removed});
    check(!store.find_friend(6), "removed");
    store.apply(StateChanged{ConnectionState::NoLicense, {}});
    check(store.friends().empty() && store.conversation(global_conversation).entries.empty(), "signing out forgets it all");
}

void test_rest_bodies() {
    check(ping_frame() == R"({"t":"ping"})", "ping frame");
    check(login_body("MDC-AAAA-BBBB-CCCC-DDDD", "0.1.0") == R"({"clientVersion":"0.1.0","key":"MDC-AAAA-BBBB-CCCC-DDDD"})",
          "login body");
    check(login_body("\xFF", "v").find("\xFF") == std::string::npos, "invalid UTF-8 is replaced, not thrown");

    const auto login = parse_login(R"({"token":"abc","expiresAt":10,"user":{"id":1,"name":"x"}})");
    check(login && login->token == "abc" && login->expires_at == 10 && login->user == UserRef{1, "x"}, "login parses");
    check(!parse_login(R"({"token":"","expiresAt":10,"user":{"id":1,"name":"x"}})"), "an empty token is rejected");
    check(!parse_login(R"({"expiresAt":10,"user":{"id":1,"name":"x"}})"), "a missing token is rejected");

    const auto page = parse_news_page(
        R"({"items":[{"id":2,"kind":"news","title":"a","body":"b","version":null,"createdAt":1},{"id":"bad"}]})");
    check(page && page->size() == 1 && (*page)[0].id == 2, "a page skips entries it cannot read");
    check(!parse_news_page(R"({"items":{}})") && !parse_news_page("oops"), "items must be an array");
    check(parse_error_code(R"({"error":"invalid_key"})") == "invalid_key", "error code");
    check(parse_error_code("<html>").empty(), "a non-JSON error body has no code");
}

void test_config() {
    auto load = parse_config(default_config_text());
    check(load.parsed && load.warnings.empty(), "the default document parses cleanly");
    check(load.config.notify == NotifySettings{} && !load.config.debug_timing && !load.config.debug_toast_slow_motion,
          "settings defaults");
    check(default_config_text().find("debugLicenseKey") == std::string::npos, "the default file holds no key");
    check(default_config_text().find("serverUrl") == std::string::npos, "the connection destination is not saved to settings");

    load = parse_config(R"({"serverUrl":"http://127.0.0.1:8787","notify":{"dm":false,"inGame":false,"inGameOffsetY":40},
                           "debugLicenseKey":"MDC-1","debugTiming":true,"debugToastSlowMotion":true,"unknown":1})");
    const auto& notify = load.config.notify;
    check(load.parsed && load.warnings.size() == 1 && load.warnings[0].find("MDC-1") == std::string::npos,
          "legacy key is ignored and its value is never reported");
    check(!notify.dm && !notify.in_game && notify.in_game_offset_y == 40.0F && notify.news && notify.friend_requests &&
          notify.sound, "notify fields");
    check(load.config.debug_timing, "debug timing is configurable");
    check(load.config.debug_toast_slow_motion, "notification slow motion is configurable");
    const std::string old = R"({"serverUrl":"https://example.com","debugLicenseKey":"secret","other":42})";
    const auto cleaned = without_legacy_settings(old);
    check(cleaned && cleaned->find("secret") == std::string::npos && cleaned->find("debugLicenseKey") == std::string::npos &&
          cleaned->find("serverUrl") == std::string::npos &&
          cleaned->find("\"other\": 42") != std::string::npos, "legacy cleanup retains unrelated settings");
    const auto saved = with_notify(old, notify);
    check(saved && saved->find("secret") == std::string::npos && saved->find("serverUrl") == std::string::npos &&
          saved->find("\"other\": 42") != std::string::npos, "saving settings also removes legacy fields");
    const auto url_only = without_legacy_settings(R"({"serverUrl":"http://127.0.0.1:8787","notify":{"sound":false}})");
    check(url_only && url_only->find("serverUrl") == std::string::npos && !parse_config(*url_only).config.notify.sound,
          "a legacy URL alone is removed while notification settings survive");
    check(!without_legacy_settings("{broken"), "malformed configuration is never overwritten");

    load = parse_config(R"({"notify":{"sound":"secret-value","inGameOffsetY":99999},"serverUrl":3})");
    check(load.parsed && load.warnings.size() == 2, "obsolete URLs are ignored; invalid notification settings are reported");
    check(load.config.notify.sound && load.config.notify.in_game_offset_y == 0.0F, "wrong fields keep their defaults");
    for (const auto& warning : load.warnings) check(warning.find("secret") == std::string::npos, "warnings never echo values");
    check(!parse_config("{oops").parsed && !parse_config("[1]").parsed, "a non-object document is not parsed");
}

void test_urls() {
    check(parse_server_url(server_url).has_value(), "the embedded server URL is valid");
    auto endpoint = parse_server_url("https://social.example.com");
    check(endpoint && endpoint->secure && endpoint->host == "social.example.com" && endpoint->port == 443 &&
          endpoint->base_path.empty(), "https defaults");
    check(websocket_url(*endpoint) == "wss://social.example.com/ws", "https becomes wss + /ws");
    endpoint = parse_server_url("http://127.0.0.1:8787/");
    check(endpoint && !endpoint->secure && endpoint->port == 8787 && endpoint->base_path.empty() &&
          websocket_url(*endpoint) == "ws://127.0.0.1:8787/ws", "a local http server");
    endpoint = parse_server_url("https://example.com:8443/social/");
    check(endpoint && endpoint->port == 8443 && endpoint->base_path == "/social" &&
          websocket_url(*endpoint) == "wss://example.com:8443/social/ws", "a base path");
    for (const char* bad : {"", "social.example.com", "ftp://x", "https://", "https://:443", "https://a:0",
                            "https://a:65536", "https://a:12x", "https://u@a", "https://a/?q", "https://a/#x",
                            "https://[::1]"}) {
        check(!parse_server_url(bad), std::string("invalid URL accepted: ") + bad);
    }
}

void test_backoff() {
    Backoff backoff;
    for (const int expected : {1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000})
        check(backoff.next(0.5).count() == expected, "spec 2.6 delays at the jitter midpoint");
    backoff.reset();
    check(backoff.next(0.0).count() == 800, "-20% bound");
    check(backoff.next(0.999999).count() == 2400, "+20% bound");
    check(backoff.attempt() == 2, "attempts counted");
}

void test_store() {
    SocialStore store;
    check(store.state() == ConnectionState::NoLicense && !store.me(), "initial state");
    check(!store.state_known(), "the NoLicense default is not an observation");
    {
        // The first report may repeat the default; it still makes the state known.
        SocialStore fresh;
        check(!fresh.apply(StateChanged{ConnectionState::NoLicense, {}}) && fresh.state_known(),
              "an unchanged first state is known");
    }
    store.apply(SessionStarted{UserRef{9, "old"}, 0});
    check(store.apply(StateChanged{ConnectionState::NoLicense, {}}) && !store.me(),
          "logout clears account even if the connection state was already NoLicense");
    check(store.apply(StateChanged{ConnectionState::Online, {}}), "a new state is a change");
    check(!store.apply(StateChanged{ConnectionState::Online, {}}), "the same state is not");
    check(store.apply(SessionStarted{UserRef{5, "fao"}, 1}) && store.me()->name == "fao", "hello sets the account");

    check(store.apply(NewsPosted{news(2)}), "news.new inserts");
    check(!store.apply(NewsPosted{news(2)}), "a duplicate id changes nothing");
    check(store.apply(NewsPage{NewsKind::News, std::nullopt, {news(2), news(1)}}), "a page merges older items");
    const auto& list = store.news(NewsKind::News);
    check(list.size() == 2 && list[0].id == 2 && list[1].id == 1, "newest first without duplicates");
    check(store.apply(NewsPosted{news(3, NewsKind::Changelog)}) && store.news(NewsKind::Changelog).size() == 1 &&
          store.news(NewsKind::News).size() == 2, "kinds are kept apart");
    check(!store.apply(NewsPage{NewsKind::News, std::nullopt, {news(4, NewsKind::Changelog)}}),
          "a page ignores items of the other kind");
    store.set_loading(NewsKind::News, true);
    check(store.apply(NewsPage{NewsKind::News, 1, {}, 20, true}) && !store.loading(NewsKind::News),
          "a failed page clears its loading state");
    store.apply(NewsPage{NewsKind::Changelog, 2, {news(1, NewsKind::Changelog)}, 1});
    check(store.has_more(NewsKind::Changelog) && store.news(NewsKind::Changelog).back().id == 1,
          "full older pages retain pagination and are merged in order");
    store.apply(NewsPage{NewsKind::Changelog, 1, {}, 1});
    check(!store.has_more(NewsKind::Changelog), "empty older page ends pagination");
    for (std::int64_t id = 10; id < 10 + static_cast<std::int64_t>(SocialStore::max_news) + 5; ++id)
        store.apply(NewsPosted{news(id)});
    check(store.news(NewsKind::News).size() == SocialStore::max_news && store.news(NewsKind::News).front().id > 200,
          "the list is bounded and keeps the newest");

    check(store.apply(StateChanged{ConnectionState::AuthFailed, "revoked"}) && !store.me() &&
          store.state_reason() == "revoked", "a failed sign-in forgets the account");
    NotifySettings settings;
    settings.dm = false;
    store.apply(SettingsLoaded{settings, true});
    check(!store.notify().dm && store.debug_timing(), "settings");
}

void test_queues() {
    Inbox<int> inbox(3);
    std::vector<int> out;
    check(!inbox.drain(out), "an empty inbox drains nothing");
    for (int i = 0; i < 5; ++i) inbox.push(i);
    check(inbox.drain(out) && out == std::vector<int>{2, 3, 4}, "a full inbox keeps the newest");
    out.clear();
    check(!inbox.drain(out), "drained once");

    Outbox<int> outbox;
    std::thread producer([&] {
        std::this_thread::sleep_for(20ms);
        outbox.push(7);
    });
    const auto value = outbox.pop(std::chrono::steady_clock::now() + 5s);
    producer.join();
    check(value == 7, "pop wakes for a push");
    check(!outbox.pop(std::chrono::steady_clock::now() + 10ms), "pop returns at the deadline");
    std::thread closer([&] {
        std::this_thread::sleep_for(20ms);
        outbox.close();
    });
    check(!outbox.pop(), "close wakes a blocked pop");
    closer.join();
    outbox.push(1);
    check(!outbox.pop(std::chrono::steady_clock::now()), "a closed outbox drops pushes");
}

} // namespace

int main() {
    try {
        test_server_frames();
        test_chat_frames();
        test_chat_store();
        test_rest_bodies();
        test_config();
        test_urls();
        test_backoff();
        test_store();
        test_queues();
        std::cout << "social tests passed (" << checks << " checks)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "social tests FAILED: " << error.what() << '\n';
        return 1;
    }
}
