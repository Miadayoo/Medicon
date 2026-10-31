#pragma once

#include "medicon/core/Module.hpp"

#include <cstdint>

namespace medicon::sdk { class ContainerScreen; }

namespace medicon {

// Driven by the chest screen's own tick, not baseTick: the controller it acts
// through only exists there, and the player's mouse is released while it runs.
class ChestStealerModule final : public Module {
public:
    static constexpr const char* module_id = "cheststealer";
    ChestStealerModule();
    void screen_tick(sdk::ContainerScreen& screen, std::uint64_t now_ms) noexcept;

    Setting& delay;
    Setting& auto_close;

protected:
    void on_enable() noexcept override;
    void on_disable() noexcept override;

private:
    std::uint64_t last_tick_ms_ = 0;
    std::uint64_t next_take_ms_ = 0;
    std::uint64_t close_at_ms_ = 0;
    bool took_any_ = false;
};

} // namespace medicon
