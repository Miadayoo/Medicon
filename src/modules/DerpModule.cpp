#include "medicon/modules/DerpModule.hpp"

#include "GameplayModuleUtils.hpp"
#include "medicon/sdk/Gameplay.hpp"

namespace medicon {

DerpModule::DerpModule()
    : GameplayModule(module_id, "Derp", "Continuously spin the player's head and body without changing view or movement direction.", Category::Player),
      speed(add_setting({"speed", "Speed", "Head and body rotation speed in degrees per second.", 360.0F, 0.0F, 1080.0F, 15.0F})) {}

void DerpModule::on_enable() noexcept { local_id_ = 0; yaw_ = 0.0F; }
void DerpModule::on_disable() noexcept { on_enable(); }

void DerpModule::game_tick(sdk::GameplayAccess& game, float delta_seconds) noexcept {
    if (!enabled()) return;
    try {
        const auto& frame = game.frame();
        if (!gameplay_detail::ready(frame)) { on_enable(); return; }
        if (local_id_ != frame.local.id) {
            local_id_ = frame.local.id;
            yaw_ = gameplay_detail::wrap_yaw(frame.head_yaw_ready ? frame.head_yaw : frame.yaw);
        }
        const float delta = gameplay_detail::tick_delta(delta_seconds);
        if (delta <= 0.0F) return;
        const float next_yaw = gameplay_detail::wrap_yaw(yaw_ + gameplay_detail::number(speed) * delta);
        if (game.rotate_head_and_body(next_yaw)) yaw_ = next_yaw;
    } catch (...) { on_enable(); }
}

} // namespace medicon
