#pragma once

#include "medicon/modules/GameplayModule.hpp"

#include <cstdint>

namespace medicon {

class KillauraModule final : public GameplayModule {
public:
    static constexpr const char* module_id = "killaura";
    KillauraModule();
    void game_tick(sdk::GameplayAccess& game, float delta_seconds) noexcept override;
    [[nodiscard]] bool rotation_diagnostic(std::uintptr_t& target_id, float& yaw, float& pitch,
                                           bool& applied) const noexcept;

    Setting& range;
    Setting& cps;
    Setting& rotate;
    Setting& swing;
    Setting& players_only;
    Setting& visible_only;

protected:
    void on_enable() noexcept override;
    void on_disable() noexcept override;

private:
    std::uintptr_t local_id_ = 0;
    std::uintptr_t target_id_ = 0;
    float cooldown_ = 0.0F;
    std::uintptr_t diagnostic_target_id_ = 0;
    float diagnostic_yaw_ = 0.0F;
    float diagnostic_pitch_ = 0.0F;
    bool diagnostic_target_valid_ = false;
    bool diagnostic_rotation_applied_ = false;
};

} // namespace medicon
