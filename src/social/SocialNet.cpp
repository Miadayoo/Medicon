#include "SocialNet.hpp"

#include "Files.hpp"

#include <medicon/social/LicenseStore.hpp>

#include <Windows.h>

#include <random>
#include <system_error>
#include <type_traits>

namespace medicon::social::detail {
namespace {

using namespace std::chrono_literals;

// Spec 2.4: a ping every 30 s keeps Cloudflare (about 100 s idle limit) and the
// server (75 s) from dropping a quiet connection.
constexpr auto heartbeat = 30s;

SocialConfig load_config(const std::filesystem::path& path, const Core& core) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        // Create the documented defaults once (spec 4.10). CREATE_NEW never replaces
        // a file another process wrote in the meantime.
        std::filesystem::create_directories(path.parent_path(), ec);
        const std::string text = default_config_text();
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
            CloseHandle(file);
            core.report("Social: created social.json with default settings.");
        }
        return parse_config(text).config;
    }
    const auto original = read_file(path);
    auto load = parse_config(original);
    // Like config.json, a broken file is never overwritten; defaults apply for this session.
    if (!load.parsed) core.report("Social: social.json is not a JSON object; using defaults without changing the file.");
    for (const auto& warning : load.warnings) core.report("Social: social.json " + warning + ".");
    if (const auto cleaned = without_legacy_settings(original); cleaned && *cleaned != original) {
        std::string error;
        if (!replace_file(path, *cleaned, error)) core.report("Social: could not remove obsolete settings: " + error + ".");
    }
    return load.config;
}

} // namespace

SocialNet::SocialNet(std::shared_ptr<Core> core, std::filesystem::path directory)
    : core_(std::move(core)), directory_(std::move(directory)) {}

void SocialNet::start() {
    auto self = shared_from_this();
    const auto guarded = [self](const char* name, void (SocialNet::*body)(), bool SocialNet::*done) {
        try {
            (self.get()->*body)();
        } catch (const std::exception& error) {
            self->core_->report(std::string("Social: ") + name + " thread stopped: " + error.what());
        } catch (...) {
            self->core_->report(std::string("Social: ") + name + " thread stopped.");
        }
        self->finished(self.get()->*done);
    };
    connection_ = std::thread(guarded, "connection", &SocialNet::run_connection, &SocialNet::connection_done_);
    sender_ = std::thread(guarded, "send", &SocialNet::run_sender, &SocialNet::sender_done_);
}

void SocialNet::finished(bool& flag) {
    {
        std::lock_guard lock(mutex_);
        flag = true;
    }
    done_.notify_all();
}

void SocialNet::request_stop() noexcept {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    // Wakes the send thread, which sends the close frame on its way out.
    core_->channel().commands.close();
}

bool SocialNet::stop(std::chrono::milliseconds budget) {
    request_stop();
    if (!connection_.joinable() && !sender_.joinable()) return true;
    const auto deadline = std::chrono::steady_clock::now() + budget;
    const auto all_done = [this] { return connection_done_ && sender_done_; };
    std::unique_lock lock(mutex_);
    // Graceful first: the server answers our close frame, which ends receive().
    if (!done_.wait_until(lock, deadline - std::min<std::chrono::milliseconds>(budget / 4, 500ms), all_done)) {
        lock.unlock();
        if (const auto current = socket()) current->abort();
        lock.lock();
        done_.wait_until(lock, deadline, all_done);
    }
    const bool connection_done = connection_done_, sender_done = sender_done_;
    lock.unlock();
    // A thread stuck in a sign-in request cannot be cancelled. Detach it; it holds
    // its own reference to this object and exits after its request times out.
    if (connection_.joinable()) connection_done ? connection_.join() : connection_.detach();
    if (sender_.joinable()) sender_done ? sender_.join() : sender_.detach();
    return connection_done && sender_done;
}

void SocialNet::set_socket(std::shared_ptr<net::WebSocket> socket) {
    std::lock_guard lock(socket_mutex_);
    socket_ = std::move(socket);
}

std::shared_ptr<net::WebSocket> SocialNet::socket() const {
    std::lock_guard lock(socket_mutex_);
    return socket_;
}

void SocialNet::publish(ConnectionState state, std::string reason) {
    // Connection thread only.
    if (published_once_ && state == published_ && reason == published_reason_) return;
    published_once_ = true;
    published_ = state;
    published_reason_ = reason;
    core_->report(std::string("Social: ") + connection_state_name(state) + (reason.empty() ? "" : " (" + reason + ")"));
    core_->channel().events.push(StateChanged{state, std::move(reason)});
}

void SocialNet::wait_for_change(std::chrono::steady_clock::duration delay, std::optional<std::uint64_t> expected) {
    std::unique_lock lock(mutex_);
    const auto seen = expected.value_or(generation_);
    const auto ready = [&] { return stopping_.load() || generation_ != seen; };
    if (delay == std::chrono::steady_clock::duration::max()) wake_.wait(lock, ready);
    else wake_.wait_for(lock, delay, ready);
}

bool SocialNet::superseded(std::uint64_t seen) const {
    std::lock_guard lock(mutex_);
    return generation_ != seen;
}

void SocialNet::handle(const Login& command) {
    {
        std::lock_guard lock(mutex_);
        pending_login_ = command.key;
        ++generation_;
    }
    wake_.notify_all();
}

void SocialNet::handle(const Logout&) {
    std::string error;
    {
        std::lock_guard lock(mutex_);
        key_.clear();
        pending_login_.reset();
        ++generation_;
        // Coordinate deletion with loading and saving the credential.
        LicenseStore(directory_ / L"license.bin").clear(error);
        core_->clear_token();
    }
    if (!error.empty()) core_->report("Social: " + error + ".");
    core_->channel().events.push(LogoutFinished{error.empty()});
    // The server answers with its own close frame, which ends the receive loop.
    if (const auto current = socket()) current->shutdown(1000, "logout");
    wake_.notify_all();
}

void SocialNet::handle(const Reconnect&) {
    {
        std::lock_guard lock(mutex_);
        ++generation_;
    }
    wake_.notify_all();
}

void SocialNet::handle(const SaveSettings& command) {
    // File I/O stays here, never on the render thread (spec 4.9).
    const auto path = directory_ / L"social.json";
    std::string error;
    const auto text = with_notify(read_file(path), command.notify);
    if (!text) {
        core_->report("Social: social.json is not a JSON object; settings were not saved.");
        core_->channel().events.push(SettingsSaveFailed{});
    } else if (!replace_file(path, *text, error)) {
        core_->report("Social: " + error + ".");
        core_->channel().events.push(SettingsSaveFailed{});
    }
}

void SocialNet::send_request(const std::string& nonce, const std::string& frame) {
    DWORD error = 0;
    const auto current = socket();
    if (current && current->send_text(frame, error)) return;
    // The ack will never come; answer the UI now (it shows "click to retry").
    core_->channel().events.push(RequestFinished{nonce, false, "offline", std::nullopt});
}

void SocialNet::handle(const SendChat& command) {
    send_request(command.nonce, command.conversation == global_conversation
                                    ? global_send_frame(command.nonce, command.body)
                                    : dm_send_frame(command.nonce, command.conversation, command.body));
}

void SocialNet::handle(const MarkRead& command) {
    // No ack (spec 2.4); a failure only means the next MarkRead repeats it.
    DWORD error = 0;
    if (const auto current = socket()) current->send_text(dm_read_frame(command.with, command.up_to), error);
}

void SocialNet::handle(const RequestFriend& command) {
    send_request(command.nonce, friend_request_frame(command.nonce, command.name));
}

void SocialNet::handle(const RespondFriend& command) {
    send_request(command.nonce, friend_respond_frame(command.nonce, command.user_id, command.accept));
}

void SocialNet::handle(const PostNews& command) {
    send_request(command.nonce, news_post_frame(command.nonce, command.kind, command.title, command.body, command.version));
}

void SocialNet::handle(const RemoveFriend& command) {
    send_request(command.nonce, friend_remove_frame(command.nonce, command.user_id));
}

SocialNet::SignIn SocialNet::sign_in(const Endpoint& endpoint, const std::string& key, std::uint64_t generation) {
    const auto response = core_->http().request(endpoint, L"POST", "/auth/license",
                                                login_body(key, core_->client_version()), {});
    if (response.status == 200) {
        if (auto login = parse_login(response.body)) {
            std::lock_guard lock(mutex_);
            if (generation_ != generation || stopping_) return SignIn::Retry;
            core_->set_token(std::move(login->token));
            return SignIn::Success;
        }
        core_->report("Social: sign-in response could not be read.");
        return SignIn::Retry;
    }
    if (response.status == 401 || response.status == 403) {
        auto code = parse_error_code(response.body);
        if (code.empty()) code = response.status == 401 ? "invalid_key" : "revoked";
        std::lock_guard lock(mutex_);
        if (generation_ != generation || stopping_) return SignIn::Retry;
        publish(ConnectionState::AuthFailed, std::move(code));
        return SignIn::Rejected;
    }
    // 429, 5xx and network errors are temporary.
    core_->report("Social: sign-in failed (" + describe_failure(response.status, response.error) + ").");
    return SignIn::Retry;
}

bool SocialNet::try_login(const Endpoint& endpoint, const std::string& key, std::uint64_t generation) {
    // A key typed on the login screen: one attempt, answered with LoginFinished,
    // and stored only once the server accepted it (spec 4.6).
    const auto previous = published_;
    const auto previous_reason = published_reason_;
    publish(ConnectionState::Authenticating);
    const auto response = core_->http().request(endpoint, L"POST", "/auth/license",
                                                login_body(key, core_->client_version()), {});
    if (superseded(generation)) return false;
    std::string error;
    if (response.status == 200) {
        if (auto login = parse_login(response.body)) {
            {
                std::lock_guard lock(mutex_);
                // Serialize saving against Logout: an old response must never
                // recreate credentials after the user has signed out.
                if (generation_ != generation || stopping_) return false;
                std::string store_error;
                if (!LicenseStore(directory_ / L"license.bin").save(key, store_error)) {
                    core_->report("Social: " + store_error + ".");
                    publish(previous, previous_reason);
                    core_->channel().events.push(LoginFinished{false, "credential_save_failed"});
                    return false;
                }
                key_ = key;
                core_->set_token(std::move(login->token));
                core_->channel().events.push(LoginFinished{true, {}});
            }
            return true;
        }
        error = "unreachable";
    } else if (response.status == 401 || response.status == 403 || response.status == 429) {
        error = parse_error_code(response.body);
        if (error.empty()) error = response.status == 401 ? "invalid_key" : response.status == 429 ? "rate_limited" : "revoked";
    } else {
        error = "unreachable";
        core_->report("Social: sign-in failed (" + describe_failure(response.status, response.error) + ").");
    }
    {
        std::lock_guard lock(mutex_);
        if (generation_ != generation || stopping_) return false;
        publish(previous, previous_reason);
        core_->channel().events.push(LoginFinished{false, std::move(error)});
    }
    return false;
}

SocialNet::Ending SocialNet::receive_loop(net::WebSocket& socket, Backoff& backoff, int& quick_retries) {
    Ending ending;
    std::string message;
    while (true) {
        std::uint16_t code = 0;
        DWORD error = 0;
        const auto result = socket.receive(message, code, error);
        if (result == net::WebSocket::Received::Closed) {
            ending.code = code;
            return ending;
        }
        if (result == net::WebSocket::Received::Failed) {
            ending.error = error;
            return ending;
        }
        const auto frame = parse_server_frame(message);
        if (!frame) {
            core_->report("Social: ignored a malformed frame.");
            continue;
        }
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, HelloFrame>) {
                backoff.reset();
                quick_retries = 0;
                publish(ConnectionState::Online);
                core_->channel().events.push(SessionStarted{value.user, value.server_time});
            } else if constexpr (std::is_same_v<T, NewsFrame>) {
                core_->channel().events.push(NewsPosted{value.item});
            } else if constexpr (std::is_same_v<T, KickedFrame>) {
                ending.kicked = value.reason; // close 4003 follows
            } else if constexpr (std::is_same_v<T, AckFrame>) {
                core_->channel().events.push(RequestFinished{value.id, value.ok, value.error, value.message});
            } else if constexpr (std::is_same_v<T, GlobalNewFrame>) {
                core_->channel().events.push(MessageReceived{global_conversation, value.message});
            } else if constexpr (std::is_same_v<T, DmNewFrame>) {
                // dm.new reaches the recipient only: the conversation is the author's.
                core_->channel().events.push(MessageReceived{value.message.author.id, value.message});
            } else if constexpr (std::is_same_v<T, DmReadFrame>) {
                core_->channel().events.push(ReadBy{value.by, value.up_to});
            } else if constexpr (std::is_same_v<T, FriendRequestFrame>) {
                core_->channel().events.push(FriendRequested{value.from});
            } else if constexpr (std::is_same_v<T, FriendUpdateFrame>) {
                core_->channel().events.push(FriendUpdated{value.user, value.status});
            } else if constexpr (std::is_same_v<T, PresenceFrame>) {
                core_->channel().events.push(PresenceChanged{value.user_id, value.online});
            }
        }, *frame);
    }
}

void SocialNet::run_connection() {
    // File I/O and DPAPI happen here, never on the render thread (spec 4.9).
    const auto config = load_config(directory_ / L"social.json", *core_);
    core_->channel().events.push(SettingsLoaded{config.notify, config.debug_timing, config.debug_toast_slow_motion});
    const auto endpoint = parse_server_url(server_url);
    if (!endpoint) {
        core_->report("Social: the server URL embedded in this client is invalid.");
        publish(ConnectionState::AuthFailed, "bad_server_url");
        while (!stopping_) {
            wait_for_change();
            std::lock_guard lock(mutex_);
            // Answer the login screen so it can say why nothing happens.
            if (std::exchange(pending_login_, std::nullopt))
                core_->channel().events.push(LoginFinished{false, "bad_server_url"});
        }
        return;
    }
    core_->set_endpoint(*endpoint);
    core_->report("Social: server " + websocket_url(*endpoint));
    {
        std::string error;
        std::lock_guard lock(mutex_);
        auto stored = LicenseStore(directory_ / L"license.bin").load(error);
        if (!error.empty()) core_->report("Social: " + error + ".");
        if (stored) key_ = std::move(*stored);
    }

    Backoff backoff;
    std::mt19937 random(std::random_device{}());
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    const auto retry_later = [&](std::uint64_t expected) {
        publish(ConnectionState::Reconnecting);
        wait_for_change(backoff.next(unit(random)), expected);
    };
    // A rejected token is replaced at once, but only once in a row: a server that
    // keeps rejecting fresh tokens must not be hammered.
    int quick_retries = 0;
    while (!stopping_) {
        std::string key;
        std::optional<std::string> login;
        std::uint64_t seen = 0;
        {
            std::lock_guard lock(mutex_);
            key = key_;
            login = std::exchange(pending_login_, std::nullopt);
            seen = generation_;
        }
        if (login) {
            if (!try_login(*endpoint, *login, seen)) wait_for_change(std::chrono::steady_clock::duration::max(), seen);
            continue;
        }
        if (key.empty()) {
            publish(ConnectionState::NoLicense);
            wait_for_change(std::chrono::steady_clock::duration::max(), seen);
            continue;
        }
        if (core_->token().empty()) {
            publish(ConnectionState::Authenticating);
            const auto outcome = sign_in(*endpoint, key, seen);
            if (superseded(seen)) {
                // Logged out or signed in with another key meanwhile.
                core_->clear_token();
                continue;
            }
            if (outcome == SignIn::Rejected) {
                wait_for_change(std::chrono::steady_clock::duration::max(), seen); // until a new key, a reconnect or stop
                continue;
            }
            if (outcome == SignIn::Retry) {
                retry_later(seen);
                continue;
            }
        }
        if (stopping_) break;

        publish(ConnectionState::Connecting);
        auto upgrade = core_->http().open_websocket(*endpoint, "/ws", core_->token());
        if (!upgrade.socket) {
            if (upgrade.status == 401) {
                core_->clear_token();
                if (quick_retries++ == 0) continue;
            }
            core_->report("Social: WebSocket connection failed (" + describe_failure(upgrade.status, upgrade.error) + ").");
            retry_later(seen);
            continue;
        }
        const std::shared_ptr<net::WebSocket> socket = std::move(upgrade.socket);
        {
            // Publish the socket atomically with the generation check so a
            // concurrent logout can always find and close it.
            std::lock_guard lock(mutex_);
            if (generation_ != seen || stopping_) {
                socket->abort();
                continue;
            }
            set_socket(socket);
        }
        // Closes our handle even if the send thread still holds a reference.
        const auto ending = [&] {
            const auto result = receive_loop(*socket, backoff, quick_retries);
            set_socket(nullptr);
            socket->abort();
            return result;
        }();
        if (stopping_) break;
        if (superseded(seen)) continue; // a logout or another key ended this connection

        switch (ending.code) {
        case close_token_invalid:
            core_->clear_token();
            if (quick_retries++ == 0) continue;
            break;
        case close_kicked:
            core_->clear_token();
            publish(ConnectionState::AuthFailed, ending.kicked.empty() ? "revoked" : ending.kicked);
            wait_for_change(std::chrono::steady_clock::duration::max(), seen);
            continue;
        case close_replaced:
            publish(ConnectionState::Replaced);
            wait_for_change(std::chrono::steady_clock::duration::max(), seen);
            continue;
        default:
            break;
        }
        core_->report("Social: disconnected (" + (ending.error ? describe_failure(0, ending.error) :
                                                  "close " + std::to_string(ending.code)) + ").");
        retry_later(seen);
    }
}

void SocialNet::run_sender() {
    auto& commands = core_->channel().commands;
    auto next_ping = std::chrono::steady_clock::now() + heartbeat;
    while (!stopping_) {
        auto command = commands.pop(next_ping);
        if (stopping_) break;
        if (command) {
            std::visit([this](const auto& value) { handle(value); }, *command);
            continue;
        }
        if (std::chrono::steady_clock::now() < next_ping) continue;
        // A failed ping is not handled here: the receive side reports the broken connection.
        DWORD error = 0;
        if (const auto current = socket()) current->send_text(ping_frame(), error);
        next_ping = std::chrono::steady_clock::now() + heartbeat;
    }
    if (const auto current = socket()) current->shutdown(close_going_away, "client_shutdown");
}

} // namespace medicon::social::detail
