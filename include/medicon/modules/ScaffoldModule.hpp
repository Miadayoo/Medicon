#pragma once

#include "medicon/modules/GameplayModule.hpp"

#include <cstdint>

namespace medicon {

class ScaffoldModule final : public GameplayModule {
public:
    static constexpr const char* module_id = "scaffold";
    ScaffoldModule();
    void game_tick(sdk::GameplayAccess& game, float delta_seconds) noexcept override;

    Setting& extend;
    Setting& auto_select;
    Setting& delay;
    Setting& rotate;

protected:
    void on_enable() noexcept override;
    void on_disable() noexcept override;

private:
    std::uintptr_t local_id_ = 0;
    float cooldown_ = 0.0F;
    float rotation_hold_remaining_ = 0.0F;
    float rotation_hold_yaw_ = 0.0F;
    float rotation_hold_pitch_ = 0.0F;
    int rotation_hold_block_x_ = 0;
    int rotation_hold_block_y_ = 0;
    int rotation_hold_block_z_ = 0;
    bool rotation_hold_active_ = false;
    bool rotation_hold_block_valid_ = false;
};

} // namespace medicon
