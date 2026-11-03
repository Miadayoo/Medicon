#pragma once

#include "Core.hpp"

#include <medicon/social/Backoff.hpp>
#include <medicon/social/AccountStore.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace medicon::social::detail {

// The WebSocket side (spec 4.2): a connection thread that signs in, connects
// and then blocks in WinHttpWebSocketReceive, and a send thread that waits on a
// condition variable for UI commands and the 30 s heartbeat.
class SocialNet final : public std::enable_shared_from_this<SocialNet> {
public:
    SocialNet(std::shared_ptr<Core> core, std::filesystem::path directory);

    void start();
    // Non-blocking: wakes both threads and asks the server to close.
    void request_stop() noexcept;
    // True when both threads ended within the budget. Otherwise the stragglers are
    // detached; they keep this object alive through shared_from_this().
    bool stop(std::chrono::milliseconds budget);

private:
    struct Ending {
        std::uint16_t code = 1006; // RFC 6455: closed without a close frame
        unsigned long error = 0;
        std::string kicked;        // reason of a preceding "kicked" frame
    };
    enum class SignIn { Success, Rejected, Retry, RateLimited };

    void run_connection();
    void run_sender();
    // Send thread: UI commands (spec 4.3).
    void handle(const Login& command);
    void handle(const Logout& command);
    void handle(const Reconnect& command);
    void handle(const SaveSettings& command);
    void handle(const SendChat& command);
    void handle(const MarkRead& command);
    void handle(const RequestFriend& command);
    void handle(const RespondFriend& command);
    void handle(const RemoveFriend& command);
    void handle(const PostNews& command);
    void handle(const SyncModels& command);
    // Sends a request frame; answers `nonce` with "offline" when there is no connection.
    void send_request(const std::string& nonce, const std::string& frame);
    // Connection thread.
    SignIn sign_in(const Endpoint& endpoint, const AccountCredential& account, std::uint64_t generation);
    bool try_login(const Endpoint& endpoint, const std::string& username, std::uint64_t generation);
    Ending receive_loop(net::WebSocket& socket, Backoff& backoff, int& quick_retries);
    void publish(ConnectionState state, std::string reason = {});
    // Sleeps until stop, or until a command changes what to connect with.
    void wait_for_change(std::chrono::steady_clock::duration delay = std::chrono::steady_clock::duration::max(),
                         std::optional<std::uint64_t> expected = std::nullopt);
    // A command arrived since `seen`: the attempt in progress is no longer wanted.
    bool superseded(std::uint64_t seen) const;
    void set_socket(std::shared_ptr<net::WebSocket> socket);
    std::shared_ptr<net::WebSocket> socket() const;
    void finished(bool& flag);

    std::shared_ptr<Core> core_;
    std::filesystem::path directory_;
    std::thread connection_, sender_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;
    std::atomic_bool stopping_{false};
    std::optional<AccountCredential> account_;
    std::optional<std::string> pending_login_;
    std::uint64_t generation_ = 0; // bumped by every command that changes the connection
    bool connection_done_ = false, sender_done_ = false;
    // Connection thread only.
    ConnectionState published_ = ConnectionState::NoLicense;
    std::string published_reason_;
    bool published_once_ = false;

    mutable std::mutex socket_mutex_;
    std::shared_ptr<net::WebSocket> socket_;
};

} // namespace medicon::social::detail
