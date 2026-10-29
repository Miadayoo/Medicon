#include "medicon/modules/SprintModule.hpp"

#include "medicon/sdk/Gameplay.hpp"

namespace medicon {
namespace {

bool wants_sprint(const sdk::GameplayFrame& frame, bool forward_only) noexcept {
    if (!frame.move_input_ready || frame.sneaking || !frame.local.alive ||
        !std::isfinite(frame.move_input.x) || !std::isfinite(frame.move_input.y)) return false;
    constexpr float minimum_input = 0.001F;
    return forward_only ? frame.move_input.y > minimum_input
        : std::hypot(frame.move_input.x, frame.move_input.y) > minimum_input;
}

} // namespace

SprintModule::SprintModule()
    : GameplayModule(module_id, "Sprint", "Automatically sprints while movement input is held.", Category::Movement),
      mode(add_setting({"mode", "Mode", "Strike follows movement input in any direction; Legit requires forward input.",
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
            local_id_ = 0;
            return;
        }
        if (local_id_ != frame.local.id) {
            controlling_sprint_ = requested_sprinting_ = false;
            local_id_ = frame.local.id;
        }

        if (!enabled() || !allow_actions) {
            if (controlling_sprint_ && game.set_sprinting(false))
                controlling_sprint_ = false;
            requested_sprinting_ = false;
            return;
        }

        const auto& selection = mode.get<Choice>();
        const bool should_sprint = wants_sprint(frame, selection.index == 1);
        requested_sprinting_ = false;
        if (!game.set_sprinting(should_sprint)) return;
        requested_sprinting_ = should_sprint;
        controlling_sprint_ = true;
    } catch (...) { requested_sprinting_ = false; }
}

} // namespace medicon
