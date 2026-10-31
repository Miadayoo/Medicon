#pragma once

#include <medicon/sdk/BedrockMenu.hpp>
#include <cstdint>
#include <mutex>
#include <string>

namespace medicon::sdk {

struct DisconnectState {
    bool visible = false;
    bool available = false;
    std::uint64_t generation = 0;
    std::string error;
};

// The native UI callback observes a fresh primary client each time. Requests
// carry only a generation; no server descriptor or native pointer crosses threads.
class BedrockReconnect {
public:
    bool initialize(std::string& diagnostic) noexcept;
    std::uintptr_t update_target() const noexcept;
    DisconnectState snapshot() const;
    bool request(std::uint64_t generation) noexcept;
    void update(void* adjusted_client, void* current_client, std::string& diagnostic);
    void stop() noexcept;
private:
    friend struct BedrockReconnectTestAccess;
    bool profile() const noexcept;
    mutable std::mutex mutex_;
    BedrockMenu menu_;
    std::uintptr_t base_ = 0;
    bool ready_ = false;
    DisconnectState state_;
    MenuSessionIdentity scene_{};
    std::uintptr_t owner_ = 0;
    std::uint64_t requested_ = 0, requested_at_ = 0, last_dispatch_ = 0;
};

}
