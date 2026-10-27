#pragma once

#include "WinHttp.hpp"

#include <medicon/social/Events.hpp>

#include <functional>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace medicon::social::detail {

using Logger = std::function<void(std::string_view)>;

// Shared by every Social thread. Held through shared_ptr, so a thread that has
// to be detached at shutdown never outlives the objects it uses.
class Core {
public:
    Core(std::string client_version, Logger log)
        : client_version_(std::move(client_version)), http_(client_version_), log_(std::move(log)) {}

    const std::string& client_version() const noexcept { return client_version_; }
    const net::HttpClient& http() const noexcept { return http_; }
    Channel& channel() noexcept { return channel_; }
    // Never pass keys, tokens or message bodies (spec 1).
    void report(std::string_view message) const {
        if (log_) log_(message);
    }

    // Written by the connection thread, read by the REST worker.
    void set_endpoint(Endpoint endpoint) {
        std::lock_guard lock(mutex_);
        endpoint_ = std::move(endpoint);
    }
    std::optional<Endpoint> endpoint() const {
        std::lock_guard lock(mutex_);
        return endpoint_;
    }
    void set_token(std::string token) {
        std::lock_guard lock(mutex_);
        token_ = std::move(token);
        ++token_generation_;
    }
    std::string token() const {
        std::lock_guard lock(mutex_);
        return token_;
    }
    void clear_token() {
        std::lock_guard lock(mutex_);
        token_.clear();
        ++token_generation_;
    }
    struct TokenSnapshot { std::string value; std::uint64_t generation; };
    TokenSnapshot token_snapshot() const {
        std::lock_guard lock(mutex_);
        return {token_, token_generation_};
    }
    // Deliver a response only while it still belongs to the active session.
    // The check and enqueue are atomic with respect to logout/token changes.
    bool publish_for_token(const TokenSnapshot& token, Event event) {
        std::lock_guard lock(mutex_);
        if (token.value.empty() || token_generation_ != token.generation) return false;
        channel_.events.push(std::move(event));
        return true;
    }

private:
    std::string client_version_;
    net::HttpClient http_;
    Logger log_;
    Channel channel_;
    mutable std::mutex mutex_;
    std::optional<Endpoint> endpoint_;
    std::string token_;
    std::uint64_t token_generation_ = 0;
};

// "HTTP 401" or "WinHTTP error 12029" for logs.
inline std::string describe_failure(int status, unsigned long error) {
    if (status != 0) return "HTTP " + std::to_string(status);
    return "WinHTTP error " + std::to_string(error);
}

} // namespace medicon::social::detail
