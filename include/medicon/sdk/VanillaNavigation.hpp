#pragma once

#include <medicon/memory/pattern.hpp>
#include <medicon/sdk/BedrockMenu.hpp>
#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace medicon::sdk {

enum class NavigationAction { Play, Settings, Quit };

// Only local Action/epoch values are queued. The host pumps after the original
// native MainMenu controller update, under its callback admission/drain guard.
// No native event, callable copy or foreign STL object is constructed.
class VanillaNavigation final {
public:
    [[nodiscard]] bool initialize(std::string& diagnostic) noexcept;
    [[nodiscard]] std::uintptr_t update_hook_target() const noexcept;
    // False before initialization, after stop(), or once a pump finds the
    // loaded profile changed. No button can reach the game while false.
    [[nodiscard]] bool ready() const noexcept { return ready_.load(std::memory_order_acquire); }
    [[nodiscard]] bool has_pending() const noexcept;
    [[nodiscard]] bool openPlay(std::uint64_t epoch) noexcept;
    [[nodiscard]] bool openSettings(std::uint64_t epoch) noexcept;
    [[nodiscard]] bool quitGame(std::uint64_t epoch) noexcept;
    void cancel(std::uint64_t retired_epoch = std::numeric_limits<std::uint64_t>::max()) noexcept;
    void stop() noexcept;
    [[nodiscard]] bool pump(void* native_controller, std::uintptr_t return_address,
                            const MenuSessionIdentity& fresh_session,
                            const std::atomic<std::uint64_t>& current_epoch,
                            const std::atomic_bool& stopping, std::string& diagnostic) noexcept;

private:
    friend struct VanillaNavigationTestAccess;
    struct Request { NavigationAction action; std::uint64_t epoch, tick; };
    [[nodiscard]] bool request(NavigationAction action, std::uint64_t epoch) noexcept;
    [[nodiscard]] bool profile(bool include_update_entry) const noexcept;
    [[nodiscard]] bool executable(std::uintptr_t address, std::size_t size) const noexcept;
    mutable std::mutex mutex_;
    bool initialized_ = false;
    std::atomic_bool ready_{false}, pending_{false};
    std::atomic_flag pumping_ = ATOMIC_FLAG_INIT;
    std::optional<Request> request_;
    std::uint64_t last_dispatch_tick_ = 0, last_dispatch_epoch_ = 0;
    bool dispatched_ = false;
    std::string terminal_diagnostic_;
    memory::AddressRange image_;
    std::vector<memory::AddressRange> executable_regions_;
};

} // namespace medicon::sdk
