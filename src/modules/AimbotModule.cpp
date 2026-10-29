#include "medicon/modules/AimbotModule.hpp"

#include "GameplayModuleUtils.hpp"
#include "medicon/sdk/Gameplay.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace medicon {

AimbotModule::AimbotModule()
    : GameplayModule(module_id, "Aimbot", "Smoothly aim at an eligible target inside your field of view.", Category::Player),
      range(add_setting({"range", "Range", "Maximum distance from your eye to a target's bounds.", 6.0F, 1.0F, 12.0F, 0.1F})),
      fov(add_setting({"fov", "FOV", "Full targeting cone in degrees, centered on your view.", 90.0F, 1.0F, 180.0F, 1.0F})),
      smoothing(add_setting({"smoothing", "Smoothing", "Turn response per second; higher values aim faster.", 8.0F, 1.0F, 30.0F, 0.5F})),
      players_only(add_setting({"playersOnly", "Players Only", "Ignore non-player actors.", true})),
      visible_only(add_setting({"visibleOnly", "Visible Only", "Require a clear line of sight.", true})),
      require_click(add_setting({"requireClick", "Require Click", "Aim while the attack button is held.", true})) {}

void AimbotModule::game_tick(sdk::GameplayAccess& game, float delta_seconds) noexcept {
    if (!enabled()) return;
    try {
        const auto& frame = game.frame();
        const float delta = gameplay_detail::tick_delta(delta_seconds);
        if (!gameplay_detail::ready(frame) || delta <= 0.0F || (require_click.get<bool>() && !frame.attack_down)) return;
        const auto target = gameplay_detail::find_target(frame, gameplay_detail::number(range), players_only.get<bool>(),
                                                         visible_only.get<bool>(), gameplay_detail::number(fov) * 0.5F, true);
        if (!target.actor) return;
        const float amount = 1.0F - std::exp(-gameplay_detail::number(smoothing) * delta);
        const float current_yaw = frame.head_yaw_ready ? frame.head_yaw : frame.yaw;
        const float yaw = gameplay_detail::wrap_yaw(
            current_yaw + gameplay_detail::wrap_yaw(target.yaw - current_yaw) * amount);
        const float pitch = frame.pitch + (target.pitch - frame.pitch) * amount;
        game.rotate_head_and_body(yaw, pitch);
    } catch (...) {}
}

} // namespace medicon
