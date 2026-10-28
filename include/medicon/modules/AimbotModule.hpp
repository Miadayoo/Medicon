#pragma once

#include "medicon/modules/GameplayModule.hpp"

namespace medicon {

class AimbotModule final : public GameplayModule {
public:
    static constexpr const char* module_id = "aimbot";
    AimbotModule();
    void game_tick(sdk::GameplayAccess& game, float delta_seconds) noexcept override;

    Setting& range;
    Setting& fov;
    Setting& smoothing;
    Setting& players_only;
    Setting& visible_only;
    Setting& require_click;
};

} // namespace medicon
