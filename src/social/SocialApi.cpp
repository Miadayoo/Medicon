#include "SocialApi.hpp"

#include <nlohmann/json.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>

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

namespace {

// Reads the DLL for a release upload, bounded. Win32 so a wide path works
// without relying on a toolchain's wide ifstream extension.
bool read_release_dll(const std::wstring& path, std::string& out) {
    constexpr std::uint64_t max_bytes = 64ull * 1024 * 1024;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && static_cast<std::uint64_t>(size.QuadPart) <= max_bytes;
    if (ok) {
        out.resize(static_cast<std::size_t>(size.QuadPart));
        std::size_t offset = 0;
        while (offset < out.size()) {
            DWORD read = 0;
            const DWORD want = static_cast<DWORD>(std::min<std::uint64_t>(out.size() - offset, 1u << 20));
            if (!ReadFile(file, out.data() + offset, want, &read, nullptr) || read == 0) { ok = false; break; }
            offset += read;
        }
    }
    CloseHandle(file);
    if (!ok) out.clear();
    return ok;
}

// "MDCR", u32le metadata length, UTF-8 JSON {version,title,body}, DLL bytes.
std::string build_release_upload(const PublishRelease& request, std::string_view dll) {
    const nlohmann::json metadata{{"version", request.version}, {"title", request.title}, {"body", request.body}};
    const std::string json = metadata.dump();
    std::string out;
    out.reserve(8 + json.size() + dll.size());
    const std::uint32_t magic = 0x5243444d; // "MDCR"
    const auto length = static_cast<std::uint32_t>(json.size());
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((magic >> (i * 8)) & 0xFF));
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((length >> (i * 8)) & 0xFF));
    out.append(json);
    out.append(dll);
    return out;
}

} // namespace

void SocialApi::fetch(const PublishRelease& request) {
    const auto fail = [&](std::string error) {
        core_->channel().events.push(RequestFinished{request.nonce, false, std::move(error), std::nullopt});
    };
    const auto endpoint = core_->endpoint();
    const auto token = core_->token_snapshot();
    if (!endpoint || token.value.empty()) return fail("offline");
    // Fail before reading/sending megabytes when publishing is not configured.
    // Older servers without the status route still accept the original POST.
    const auto status = core_->http().request(*endpoint, L"GET", "/releases/status", {}, token.value);
    if (status.status != 200 && status.status != 404) {
        std::string error = parse_error_code(status.body);
        if (error.empty()) error = status.error == ERROR_WINHTTP_TIMEOUT ? "release_timeout" : "release_network";
        core_->report("Social: release preflight failed (" + describe_failure(status.status, status.error) + ", " + error + ").");
        return fail(std::move(error));
    }
    std::string dll;
    if (!read_release_dll(request.dll_path, dll)) return fail("dll_unreadable");
    const std::string upload = build_release_upload(request, dll);
    const auto response =
        core_->http().upload(*endpoint, "/releases", "application/vnd.medicon.release", upload.data(), upload.size(), token.value);
    if (response.status == 200) {
        core_->report("Social: published Core " + request.version + " (" + std::to_string(dll.size()) + " bytes).");
        core_->channel().events.push(RequestFinished{request.nonce, true, {}, std::nullopt});
        return;
    }
    std::string error = parse_error_code(response.body);
    if (response.status == 413) error = "release_too_large";
    if (response.status == 0) error = response.error == ERROR_WINHTTP_TIMEOUT ? "release_timeout" : "release_network";
    if (error.empty()) error = response.status == 403 ? "forbidden" : "release_failed";
    core_->report("Social: publishing Core " + request.version + " failed (" +
                  describe_failure(response.status, response.error) + ", " + error + ").");
    fail(std::move(error));
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
