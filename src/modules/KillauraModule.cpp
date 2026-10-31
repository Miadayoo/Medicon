#include "medicon/modules/KillauraModule.hpp"

#include "GameplayModuleUtils.hpp"
#include "medicon/sdk/Gameplay.hpp"

#include <algorithm>
#include <limits>

namespace medicon {

namespace {

constexpr int mode_single = 1;
constexpr int mode_multi = 2;
constexpr int priority_health = 1;

// Lowest known health first; unknown health and ties fall back to distance.
bool better_target(const gameplay_detail::Target& a, const gameplay_detail::Target& b, bool by_health) noexcept {
    if (by_health) {
        const bool a_known = a.actor->health >= 0.0F, b_known = b.actor->health >= 0.0F;
        if (a_known != b_known) return a_known;
        if (a_known && a.actor->health != b.actor->health) return a.actor->health < b.actor->health;
    }
    return a.distance != b.distance ? a.distance < b.distance : a.actor->id < b.actor->id;
}

} // namespace

KillauraModule::KillauraModule()
    : GameplayModule(module_id, "Killaura", "Attack eligible actors within range.", Category::Player),
      mode(add_setting({"mode", "Mode", "Switch picks the best target every tick; Single keeps its target until it is no longer eligible; Multi attacks every eligible actor.",
                        Choice{0, {"Switch", "Single", "Multi"}}})),
      priority(add_setting({"priority", "Priority", "Which actor counts as the best target. Health uses distance when health is unknown.",
                            Choice{0, {"Distance", "Health"}}})),
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
        const int target_mode = mode.get<Choice>().index;
        const bool by_health = priority.get<Choice>().index == priority_health;
        const float reach = gameplay_detail::number(range);
        gameplay_detail::Target best, locked;
        multi_targets_.clear();
        for (const auto& actor : frame.actors) {
            gameplay_detail::Target candidate;
            if (!gameplay_detail::target_candidate(frame, actor, reach, players_only.get<bool>(), visible_only.get<bool>(),
                                                   std::numeric_limits<float>::max(), candidate)) continue;
            if (target_mode == mode_multi) multi_targets_.push_back(actor.id);
            if (actor.id == target_id_) locked = candidate;
            if (!best.actor || better_target(candidate, best, by_health)) best = candidate;
        }
        const auto& target = target_mode == mode_single && locked.actor ? locked : best;
        if (!target.actor) {
            target_id_ = 0;
            diagnostic_target_valid_ = false;
            return;
        }
        target_id_ = target.actor->id;
        diagnostic_target_id_ = target.actor->id;
        diagnostic_yaw_ = target.yaw;
        diagnostic_pitch_ = target.pitch;
        diagnostic_target_valid_ = rotate_to_target;
        diagnostic_rotation_applied_ = rotate_to_target && game.rotate_head_and_body(target.yaw, target.pitch);
        if (cooldown_ > 0.00001F) return;
        // Throttle failed attempts as well, and discard missed intervals.
        cooldown_ = 1.0F / gameplay_detail::number(cps);
        bool attacked = false;
        if (target_mode == mode_multi) {
            for (const auto id : multi_targets_) attacked = game.attack(id) || attacked;
        } else {
            attacked = game.attack(target.actor->id);
        }
        if (attacked && swing.get<bool>()) game.swing();
    } catch (...) {
        cooldown_ = 0.25F;
        diagnostic_target_valid_ = false;
        diagnostic_rotation_applied_ = false;
    }
}

} // namespace medicon
