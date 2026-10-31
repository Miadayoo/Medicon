#include "medicon/modules/SprintModule.hpp"

#include "medicon/sdk/Gameplay.hpp"

namespace medicon {

bool should_sprint(const sdk::GameplayFrame& frame, const Choice& selection,
                   bool sprint_fix, float rotation_yaw) noexcept {
    if (!frame.move_input_ready || frame.sneaking || !frame.local.alive ||
        !std::isfinite(frame.move_input.x) || !std::isfinite(frame.move_input.y)) return false;
    constexpr float minimum_input = 0.001F;
    if (std::hypot(frame.move_input.x, frame.move_input.y) <= minimum_input) return false;
    if (selection.index == 0) return true;
    if (!sprint_fix) return frame.move_input.y > minimum_input;
    if (!std::isfinite(rotation_yaw)) return false;
    const float movement_yaw = frame.yaw +
        std::atan2(frame.move_input.x, frame.move_input.y) * 180.0F / 3.14159265358979323846F;
    return std::abs(std::remainder(movement_yaw - rotation_yaw, 360.0F)) <= 45.0F;
}

bool apply_sprint(sdk::GameplayAccess& game, bool sprinting) noexcept {
    return game.set_sprinting(sprinting);
}

SprintModule::SprintModule()
    : GameplayModule(module_id, "Sprint", "Automatically sprints while movement input is held.", Category::Movement),
      mode(add_setting({"mode", "Mode", "Rage follows movement input in any direction; Legit requires forward input.",
                        Choice{0, {"Rage", "Legit"}}})) {}

void SprintModule::game_tick(sdk::GameplayAccess& game, float delta_seconds) noexcept {
    (void)delta_seconds;
    update(game, true);
}

void SprintModule::update(sdk::GameplayAccess& game, bool allow_actions,
                          bool sprint_fix, float rotation_yaw) noexcept {
    try {
        const auto& frame = game.frame();
        if (!frame.ready) {
            controlling_sprint_ = false;
            requested_sprinting_ = false;
            local_id_ = 0;
            return;
        }
        if (local_id_ != frame.local.id) {
            controlling_sprint_ = requested_sprinting_ = false;
            local_id_ = frame.local.id;
        }

        if (!enabled() || !allow_actions) {
            if (controlling_sprint_ && apply_sprint(game, false))
                controlling_sprint_ = false;
            requested_sprinting_ = false;
            return;
        }

        const auto& selection = mode.get<Choice>();
        const bool should_sprint_now = should_sprint(frame, selection, sprint_fix, rotation_yaw);
        requested_sprinting_ = false;
        if (!apply_sprint(game, should_sprint_now)) return;
        requested_sprinting_ = should_sprint_now;
        controlling_sprint_ = true;
    } catch (...) { requested_sprinting_ = false; }
}

} // namespace medicon
