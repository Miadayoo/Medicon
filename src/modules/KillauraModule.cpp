#include "medicon/modules/KillauraModule.hpp"

#include "GameplayModuleUtils.hpp"
#include "medicon/sdk/Gameplay.hpp"

#include <algorithm>
#include <limits>

namespace medicon {

KillauraModule::KillauraModule()
    : GameplayModule(module_id, "Killaura", "Attack the nearest eligible actor within range.", Category::Player),
      range(add_setting({"range", "Range", "Maximum distance from your eye to a target's bounds.", 3.5F, 1.0F, 6.0F, 0.1F})),
      cps(add_setting({"cps", "CPS", "Maximum attacks per second; limited to one attack per game tick.", 10.0F, 1.0F, 20.0F, 1.0F})),
      rotate(add_setting({"rotate", "Rotate", "Rotation mode used while attacking.", Choice{1, {"None", "Simple"}}})),
      swing(add_setting({"swing", "Swing", "Swing your arm after a successful attack.", true})),
      players_only(add_setting({"playersOnly", "Players Only", "Ignore non-player actors.", true})),
      visible_only(add_setting({"visibleOnly", "Visible Only", "Require a clear line of sight.", true})) {}

void KillauraModule::on_enable() noexcept {
    local_id_ = 0;
    target_id_ = 0;
    cooldown_ = 0.0F;
    diagnostic_target_id_ = 0;
    diagnostic_yaw_ = diagnostic_pitch_ = 0.0F;
    diagnostic_target_valid_ = false;
    diagnostic_rotation_applied_ = false;
}
void KillauraModule::on_disable() noexcept { on_enable(); }

bool KillauraModule::rotation_diagnostic(std::uintptr_t& target_id, float& yaw, float& pitch,
                                         bool& applied) const noexcept {
    if (!diagnostic_target_valid_) return false;
    target_id = diagnostic_target_id_;
    yaw = diagnostic_yaw_;
    pitch = diagnostic_pitch_;
    applied = diagnostic_rotation_applied_;
    return true;
}

void KillauraModule::game_tick(sdk::GameplayAccess& game, float delta_seconds) noexcept {
    if (!enabled()) return;
    try {
        const auto& frame = game.frame();
        if (!gameplay_detail::ready(frame)) { on_enable(); return; }
        if (local_id_ != frame.local.id) { local_id_ = frame.local.id; target_id_ = 0; cooldown_ = 0.0F; }
        const float delta = gameplay_detail::tick_delta(delta_seconds);
        if (delta <= 0.0F) return;
        cooldown_ = std::max(0.0F, cooldown_ - delta);
        const auto& rotate_mode = rotate.get<Choice>();
        const bool rotate_to_target = rotate_mode.index == 1;
        const auto target = gameplay_detail::find_target(frame, gameplay_detail::number(range), players_only.get<bool>(),
                                                         visible_only.get<bool>(), std::numeric_limits<float>::max(), false,
                                                         rotate_to_target ? target_id_ : 0);
        if (!target.actor) {
            target_id_ = 0;
            diagnostic_target_valid_ = false;
            return;
        }
        target_id_ = rotate_to_target ? target.actor->id : 0;
        diagnostic_target_id_ = target.actor->id;
        diagnostic_yaw_ = target.yaw;
        diagnostic_pitch_ = target.pitch;
        diagnostic_target_valid_ = rotate_to_target;
        diagnostic_rotation_applied_ = rotate_to_target && game.rotate_head_and_body(target.yaw, target.pitch);
        if (cooldown_ > 0.00001F) return;
        // Throttle failed attempts as well, and discard missed intervals.
        cooldown_ = 1.0F / gameplay_detail::number(cps);
        if (game.attack(target.actor->id) && swing.get<bool>()) game.swing();
    } catch (...) {
        cooldown_ = 0.25F;
        diagnostic_target_valid_ = false;
        diagnostic_rotation_applied_ = false;
    }
}

} // namespace medicon
