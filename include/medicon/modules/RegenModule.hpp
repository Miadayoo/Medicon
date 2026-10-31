#pragma once

#include "medicon/modules/GameplayModule.hpp"
#include "medicon/sdk/Gameplay.hpp"

#include <atomic>
#include <cstdint>

namespace medicon {

class RegenModule final : public GameplayModule {
public:
    static constexpr const char* module_id = "regen";
    enum class DiagnosticStatus : std::uint8_t {
        idle, scanning, no_target, target_found, no_pickaxe, pickaxe_fallback, switching_pickaxe,
        waiting_for_pickaxe, starting_mine, mining, switching_safe_item,
        waiting_for_safe_item, mine_failed, broken
    };
    RegenModule();
    void game_tick(sdk::GameplayAccess& game, float delta_seconds) noexcept override;
    void release(sdk::GameplayAccess& game, DiagnosticStatus final_status = DiagnosticStatus::idle) noexcept;
    [[nodiscard]] DiagnosticStatus diagnostic_status() const noexcept {
        return diagnostic_status_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool needs_restore() const noexcept {
        return release_requested_.load(std::memory_order_acquire);
    }

    Setting& range;
    Setting& destroy_speed;
    Setting& rotation_percentage;
    Setting& infinity_durability;
    Setting& swing;
    Setting& switch_mode;

protected:
    void on_enable() noexcept override;
    void on_disable() noexcept override;

private:
    enum class Phase : std::uint8_t { idle, wait_pickaxe, mining, wait_safe_item, wait_break };
    [[nodiscard]] bool find_target(sdk::GameplayAccess& game, sdk::BlockPos& position,
                                   std::uint8_t& face, float& yaw, float& pitch,
                                   const sdk::BlockPos* excluded = nullptr) const;
    void clear_target() noexcept;
    void finish_target(sdk::GameplayAccess& game) noexcept;

    std::uintptr_t local_id_ = 0;
    sdk::BlockPos target_{};
    sdk::BlockPos completed_target_{};
    std::uint8_t face_ = 0;
    float target_yaw_ = 0.0F;
    float target_pitch_ = 0.0F;
    float progress_ = 0.0F;
    float scan_cooldown_ = 0.0F;
    float break_wait_ = 0.0F;
    float completed_wait_ = 0.0F;
    int original_slot_ = -1;
    int pickaxe_slot_ = -1;
    int safe_slot_ = -1;
    Phase phase_ = Phase::idle;
    bool target_valid_ = false;
    bool slot_changed_ = false;
    bool mining_started_ = false;
    bool completed_target_valid_ = false;
    std::atomic_bool release_requested_{};
    std::atomic<DiagnosticStatus> diagnostic_status_{DiagnosticStatus::idle};
};

} // namespace medicon
