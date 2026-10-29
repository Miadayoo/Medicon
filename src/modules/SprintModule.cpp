#include "medicon/modules/SprintModule.hpp"

#include "medicon/sdk/Gameplay.hpp"

namespace medicon {
namespace {

bool moving_forward(const sdk::GameplayFrame& frame) noexcept {
    constexpr float minimum_speed = 0.015F;
    constexpr float degrees_to_radians = 0.01745329251994329577F;
    const float yaw = frame.yaw * degrees_to_radians;
    const float forward_x = -std::sin(yaw);
    const float forward_z = std::cos(yaw);
    const float forward_speed = frame.local.velocity.x * forward_x +
                                frame.local.velocity.z * forward_z;
    return std::isfinite(forward_speed) && forward_speed > minimum_speed;
}

bool moving_horizontally(const sdk::GameplayFrame& frame) noexcept {
    constexpr float minimum_speed = 0.015F;
    const float x = frame.local.velocity.x;
    const float z = frame.local.velocity.z;
    return std::isfinite(x) && std::isfinite(z) && std::hypot(x, z) > minimum_speed;
}

} // namespace

SprintModule::SprintModule()
    : GameplayModule(module_id, "Sprint", "Requests sprint from horizontal movement direction.", Category::Movement),
      mode(add_setting({"mode", "Mode", "Strike follows horizontal movement; Legit follows movement toward the view direction.",
                        Choice{0, {"Strike", "Legit"}}})) {}

void SprintModule::game_tick(sdk::GameplayAccess& game, float delta_seconds) noexcept {
    (void)delta_seconds;
    update(game, true);
}

void SprintModule::update(sdk::GameplayAccess& game, bool allow_actions) noexcept {
    try {
        const auto& frame = game.frame();
        if (!frame.ready) {
            controlling_sprint_ = false;
            requested_sprinting_ = false;
            return;
        }

        if (!enabled() || !allow_actions) {
            if (controlling_sprint_ && game.set_sprinting(original_sprinting_))
                controlling_sprint_ = false;
            requested_sprinting_ = original_sprinting_;
            return;
        }

        const auto& selection = mode.get<Choice>();
        const bool should_sprint = selection.index == 1
            ? moving_forward(frame) : moving_horizontally(frame);
        const bool starting_control = !controlling_sprint_;
        const bool original_sprinting = starting_control ? frame.sprinting : false;
        if (!game.set_sprinting(should_sprint)) return;
        requested_sprinting_ = should_sprint;

        if (starting_control) {
            original_sprinting_ = original_sprinting;
            controlling_sprint_ = true;
        }
    } catch (...) {}
}

} // namespace medicon
