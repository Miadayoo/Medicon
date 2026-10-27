#pragma once

#include "Core.hpp"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace medicon::social::detail {

// REST calls requested by the UI (spec 4.2): one worker thread draining
// Channel::api. Results go back through Channel::events.
class SocialApi final : public std::enable_shared_from_this<SocialApi> {
public:
    explicit SocialApi(std::shared_ptr<Core> core) : core_(std::move(core)) {}

    void start();
    void request_stop() noexcept { core_->channel().api.close(); }
    bool stop(std::chrono::milliseconds budget);

private:
    void run();
    void fetch(const FetchNews& request);
    void fetch(const FetchMessages& request);
    void fetch(const FetchFriends& request);
    void fetch(const SearchUsers& request);
    void fetch(const PublishRelease& request);

    std::shared_ptr<Core> core_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable done_;
    bool finished_ = false;
};

} // namespace medicon::social::detail
