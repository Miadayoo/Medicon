#include "SocialApi.hpp"

#include <algorithm>

namespace medicon::social::detail {

void SocialApi::start() {
    auto self = shared_from_this();
    worker_ = std::thread([self] {
        try {
            self->run();
        } catch (const std::exception& error) {
            self->core_->report(std::string("Social: REST thread stopped: ") + error.what());
        } catch (...) {
            self->core_->report("Social: REST thread stopped.");
        }
        {
            std::lock_guard lock(self->mutex_);
            self->finished_ = true;
        }
        self->done_.notify_all();
    });
}

bool SocialApi::stop(std::chrono::milliseconds budget) {
    request_stop();
    if (!worker_.joinable()) return true;
    std::unique_lock lock(mutex_);
    const bool finished = done_.wait_for(lock, budget, [this] { return finished_; });
    lock.unlock();
    // An in-flight request cannot be cancelled; the detached thread keeps its
    // own reference and ends when WinHTTP times out.
    finished ? worker_.join() : worker_.detach();
    return finished;
}

void SocialApi::run() {
    while (auto request = core_->channel().api.pop()) {
        std::visit([this](const auto& value) { fetch(value); }, *request);
    }
}

void SocialApi::fetch(const FetchNews& request) {
    const int limit = std::clamp(request.limit, 1, 100);
    NewsPage page{request.kind, request.before, {}, limit, true};
    const auto endpoint = core_->endpoint();
    const auto token = core_->token_snapshot();
    // Offline: the page is reported as failed; the UI asks again after the next hello.
    if (!endpoint || token.value.empty()) {
        core_->channel().events.push(std::move(page));
        return;
    }
    std::string path = std::string("/news?kind=") + news_kind_name(request.kind) + "&limit=" + std::to_string(limit);
    if (request.before) path += "&before=" + std::to_string(*request.before);
    const auto response = core_->http().request(*endpoint, L"GET", path, {}, token.value);
    if (response.status == 200) {
        if (auto items = parse_news_page(response.body)) {
            page.items = std::move(*items);
            page.failed = false;
            if (!core_->publish_for_token(token, page)) {
                // Still complete the request so the render-side loading flag
                // cannot remain set after logout or a token refresh.
                page.items.clear();
                page.failed = true;
                core_->channel().events.push(std::move(page));
            }
            return;
        }
    }
    // 401 means the token expired; the server closes the WebSocket with 4001 at
    // the same moment and the connection thread signs in again.
    core_->report("Social: loading " + std::string(news_kind_name(request.kind)) + " failed (" +
                  describe_failure(response.status, response.error) + ").");
    core_->channel().events.push(std::move(page));
}

void SocialApi::fetch(const FetchMessages& request) {
    const int limit = std::clamp(request.limit, 1, 100);
    MessagePage page{request.conversation, request.before, {}, limit, true, {}};
    const auto endpoint = core_->endpoint();
    const auto token = core_->token_snapshot();
    if (!endpoint || token.value.empty()) {
        core_->channel().events.push(std::move(page));
        return;
    }
    std::string path = request.conversation == global_conversation
                           ? std::string("/global/messages")
                           : "/dm/" + std::to_string(request.conversation) + "/messages";
    path += "?limit=" + std::to_string(limit);
    if (request.before) path += "&before=" + std::to_string(*request.before);
    const auto response = core_->http().request(*endpoint, L"GET", path, {}, token.value);
    if (response.status == 200) {
        if (auto items = parse_messages(response.body)) {
            page.items = std::move(*items);
            page.failed = false;
            if (!core_->publish_for_token(token, page)) {
                page.items.clear();
                page.failed = true;
                core_->channel().events.push(std::move(page));
            }
            return;
        }
    }
    if (response.status == 403) page.error = parse_error_code(response.body);
    core_->report(std::string("Social: loading ") + (request.conversation == global_conversation ? "Global" : "a DM") +
                  " failed (" + describe_failure(response.status, response.error) + ").");
    core_->channel().events.push(std::move(page));
}

void SocialApi::fetch(const FetchFriends&) {
    FriendsLoaded loaded{{}, true};
    const auto endpoint = core_->endpoint();
    const auto token = core_->token_snapshot();
    if (endpoint && !token.value.empty()) {
        const auto response = core_->http().request(*endpoint, L"GET", "/friends", {}, token.value);
        if (response.status == 200) {
            if (auto list = parse_friends(response.body)) {
                loaded.list = std::move(*list);
                loaded.failed = false;
                if (core_->publish_for_token(token, std::move(loaded))) return;
                loaded = FriendsLoaded{{}, true};
            }
        } else {
            core_->report("Social: loading friends failed (" + describe_failure(response.status, response.error) + ").");
        }
    }
    core_->channel().events.push(std::move(loaded));
}

void SocialApi::fetch(const SearchUsers& request) {
    SearchFinished finished{request.query, {}, true};
    const auto endpoint = core_->endpoint();
    const auto token = core_->token_snapshot();
    if (endpoint && !token.value.empty() && !request.query.empty()) {
        const auto response =
            core_->http().request(*endpoint, L"GET", "/users/search?q=" + url_encode(request.query), {}, token.value);
        if (response.status == 200) {
            if (auto items = parse_search(response.body)) {
                finished.items = std::move(*items);
                finished.failed = false;
                if (core_->publish_for_token(token, finished)) return;
                finished.items.clear();
                finished.failed = true;
            }
        }
    }
    core_->channel().events.push(std::move(finished));
}

} // namespace medicon::social::detail
