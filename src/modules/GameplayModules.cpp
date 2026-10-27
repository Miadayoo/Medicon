#include "medicon/modules/GameplayModules.hpp"
#include "medicon/sdk/Gameplay.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace medicon {
namespace {

constexpr float radians_to_degrees = 57.29577951308232F;
constexpr float degrees_to_radians = 1.0F / radians_to_degrees;

bool finite(sdk::Vec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool valid_bounds(const sdk::Bounds& bounds) noexcept {
    return finite(bounds.min) && finite(bounds.max) &&
        bounds.min.x <= bounds.max.x && bounds.min.z <= bounds.max.z &&
        bounds.max.y - bounds.min.y > 0.01F;
}

bool ready(const sdk::GameplayFrame& frame) noexcept {
    return frame.ready && frame.local.id != 0 && frame.local.alive && finite(frame.eye) &&
        std::isfinite(frame.yaw) && std::isfinite(frame.pitch);
}

// No accumulated catch-up after stalls; attacks and placements run once per
// game tick and rotations cannot jump through several seconds of missed ticks.
float tick_delta(float delta) noexcept {
    return std::isfinite(delta) ? std::clamp(delta, 0.0F, 0.25F) : 0.0F;
}

float number(const Setting& setting) {
    const float value = setting.get<float>();
    return std::isfinite(value) ? std::clamp(value, setting.minimum, setting.maximum) : setting.minimum;
}

float wrap_yaw(float yaw) noexcept {
    yaw = std::fmod(yaw + 180.0F, 360.0F);
    if (yaw < 0.0F) yaw += 360.0F;
    return yaw - 180.0F;
}

struct Target {
    const sdk::ActorSnapshot* actor = nullptr;
    float yaw = 0.0F;
    float pitch = 0.0F;
    float distance = std::numeric_limits<float>::max();
    float angle = std::numeric_limits<float>::max();
};

Target find_target(const sdk::GameplayFrame& frame, float range, bool players_only,
                   bool visible_only, float half_fov, bool prefer_angle) noexcept {
    Target best;
    for (const auto& actor : frame.actors) {
        if (actor.id == 0 || actor.id == frame.local.id || actor.local || !actor.alive ||
            (players_only && !actor.player) || (visible_only && !actor.visible) || !finite(actor.position)) continue;

        sdk::Vec3 point = actor.position;
        sdk::Vec3 closest = point;
        if (valid_bounds(actor.bounds)) {
            point = {(actor.bounds.min.x + actor.bounds.max.x) * 0.5F,
                     actor.bounds.min.y + (actor.bounds.max.y - actor.bounds.min.y) * 0.75F,
                     (actor.bounds.min.z + actor.bounds.max.z) * 0.5F};
            closest = {std::clamp(frame.eye.x, actor.bounds.min.x, actor.bounds.max.x),
                       std::clamp(frame.eye.y, actor.bounds.min.y, actor.bounds.max.y),
                       std::clamp(frame.eye.z, actor.bounds.min.z, actor.bounds.max.z)};
        }
        const float distance = std::hypot(std::hypot(closest.x - frame.eye.x, closest.z - frame.eye.z),
                                          closest.y - frame.eye.y);
        if (!std::isfinite(distance) || distance > range) continue;

        const float dx = point.x - frame.eye.x;
        const float dy = point.y - frame.eye.y;
        const float dz = point.z - frame.eye.z;
        if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz)) continue;
        const float horizontal = std::hypot(dx, dz);
        if (horizontal < 0.0001F && std::abs(dy) < 0.0001F) continue;
        const float yaw = horizontal < 0.0001F ? frame.yaw :
            wrap_yaw(std::atan2(-dx, dz) * radians_to_degrees);
        const float pitch = std::clamp(-std::atan2(dy, horizontal) * radians_to_degrees, -90.0F, 90.0F);
        const float current_pitch = frame.pitch * degrees_to_radians;
        const float target_pitch = pitch * degrees_to_radians;
        const float yaw_difference = wrap_yaw(yaw - frame.yaw) * degrees_to_radians;
        const float dot = std::cos(current_pitch) * std::cos(target_pitch) * std::cos(yaw_difference) +
                          std::sin(current_pitch) * std::sin(target_pitch);
        const float angle = std::acos(std::clamp(dot, -1.0F, 1.0F)) * radians_to_degrees;
        if (angle > half_fov) continue;

        const float score = prefer_angle ? angle : distance;
        const float best_score = prefer_angle ? best.angle : best.distance;
        if (!best.actor || score < best_score ||
            (score == best_score && (distance < best.distance ||
             (distance == best.distance && actor.id < best.actor->id)))) {
            best = {&actor, yaw, pitch, distance, angle};
        }
    }
    return best;
}

bool block_position(float x, float y, float z, sdk::BlockPos& result) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
    const double bx = std::floor(static_cast<double>(x));
    const double by = std::floor(static_cast<double>(y));
    const double bz = std::floor(static_cast<double>(z));
    const double minimum = std::numeric_limits<int>::min();
    const double maximum = std::numeric_limits<int>::max();
    if (bx < minimum || bx > maximum || by < minimum || by > maximum || bz < minimum || bz > maximum) return false;
    result = {static_cast<int>(bx), static_cast<int>(by), static_cast<int>(bz)};
    return true;
}

bool same_block(sdk::BlockPos a, sdk::BlockPos b) noexcept {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

} // namespace

DerpModule::DerpModule()
    : GameplayModule(module_id, "Derp", "Continuously spin your view with a fixed pitch.", Category::Player),
      speed(add_setting({"speed", "Speed", "Rotation speed in degrees per second.", 360.0F, 0.0F, 1080.0F, 15.0F})),
      pitch(add_setting({"pitch", "Pitch", "Vertical view angle; positive looks down.", 89.0F, -90.0F, 90.0F, 1.0F})) {}

void DerpModule::on_enable() noexcept { local_id_ = 0; yaw_ = 0.0F; }
void DerpModule::on_disable() noexcept { on_enable(); }

void DerpModule::game_tick(sdk::GameplayAccess& game, float delta_seconds) noexcept {
    if (!enabled()) return;
    try {
        const auto& frame = game.frame();
        if (!ready(frame)) { on_enable(); return; }
        if (local_id_ != frame.local.id) { local_id_ = frame.local.id; yaw_ = wrap_yaw(frame.yaw); }
        const float delta = tick_delta(delta_seconds);
        if (delta <= 0.0F) return;
        const float next_yaw = wrap_yaw(yaw_ + number(speed) * delta);
        if (game.rotate(next_yaw, number(pitch))) yaw_ = next_yaw;
    } catch (...) { on_enable(); }
}

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
        const float delta = tick_delta(delta_seconds);
        if (!ready(frame) || delta <= 0.0F || (require_click.get<bool>() && !frame.attack_down)) return;
        const auto target = find_target(frame, number(range), players_only.get<bool>(), visible_only.get<bool>(),
                                        number(fov) * 0.5F, true);
        if (!target.actor) return;
        const float amount = 1.0F - std::exp(-number(smoothing) * delta);
        const float yaw = wrap_yaw(frame.yaw + wrap_yaw(target.yaw - frame.yaw) * amount);
        const float pitch_value = std::clamp(frame.pitch + (target.pitch - frame.pitch) * amount, -90.0F, 90.0F);
        game.rotate(yaw, pitch_value);
    } catch (...) {}
}

KillauraModule::KillauraModule()
    : GameplayModule(module_id, "Killaura", "Attack the nearest eligible actor within range.", Category::Player),
      range(add_setting({"range", "Range", "Maximum distance from your eye to a target's bounds.", 3.5F, 1.0F, 6.0F, 0.1F})),
      cps(add_setting({"cps", "CPS", "Maximum attacks per second; limited to one attack per game tick.", 10.0F, 1.0F, 20.0F, 1.0F})),
      rotate(add_setting({"rotate", "Rotate", "Face the selected target before attacking.", true})),
      players_only(add_setting({"playersOnly", "Players Only", "Ignore non-player actors.", true})),
      visible_only(add_setting({"visibleOnly", "Visible Only", "Require a clear line of sight.", true})) {}

void KillauraModule::on_enable() noexcept { local_id_ = 0; cooldown_ = 0.0F; }
void KillauraModule::on_disable() noexcept { on_enable(); }

void KillauraModule::game_tick(sdk::GameplayAccess& game, float delta_seconds) noexcept {
    if (!enabled()) return;
    try {
        const auto& frame = game.frame();
        if (!ready(frame)) { on_enable(); return; }
        if (local_id_ != frame.local.id) { local_id_ = frame.local.id; cooldown_ = 0.0F; }
        const float delta = tick_delta(delta_seconds);
        if (delta <= 0.0F) return;
        cooldown_ = std::max(0.0F, cooldown_ - delta);
        const auto target = find_target(frame, number(range), players_only.get<bool>(), visible_only.get<bool>(),
                                        std::numeric_limits<float>::max(), false);
        if (!target.actor) return;
        if (rotate.get<bool>()) game.rotate(target.yaw, target.pitch);
        if (cooldown_ > 0.00001F) return;
        // Throttle failed attempts as well, and discard missed intervals.
        cooldown_ = 1.0F / number(cps);
        game.attack(target.actor->id);
    } catch (...) { cooldown_ = 0.25F; }
}

ScaffoldModule::ScaffoldModule()
    : GameplayModule(module_id, "Scaffold", "Place blocks below your feet and extend in your movement direction.", Category::Movement),
      extend(add_setting({"extend", "Extend", "Look ahead along horizontal movement, in blocks.", 1.0F, 0.0F, 4.0F, 0.25F})),
      auto_select(add_setting({"autoSelect", "Auto Select", "Temporarily select a placeable hotbar block, then restore your slot.", true})),
      delay(add_setting({"delay", "Delay", "Minimum milliseconds between placement attempts.", 100.0F, 0.0F, 500.0F, 10.0F})) {}

void ScaffoldModule::on_enable() noexcept { local_id_ = 0; cooldown_ = 0.0F; }
void ScaffoldModule::on_disable() noexcept { on_enable(); }

void ScaffoldModule::game_tick(sdk::GameplayAccess& game, float delta_seconds) noexcept {
    if (!enabled()) return;
    try {
        const auto& frame = game.frame();
        if (!ready(frame)) { on_enable(); return; }
        if (local_id_ != frame.local.id) { local_id_ = frame.local.id; cooldown_ = 0.0F; }
        const float delta = tick_delta(delta_seconds);
        if (delta <= 0.0F || !valid_bounds(frame.local.bounds) || !finite(frame.local.position)) return;
        cooldown_ = std::max(0.0F, cooldown_ - delta);
        if (cooldown_ > 0.00001F) return;

        const auto position = frame.local.position;
        // A small downward inset chooses the supporting cell both at integer
        // ground heights and while the player's feet are between block levels.
        const float y = frame.local.bounds.min.y - 0.05F;
        const auto velocity = frame.local.velocity;
        const float horizontal = finite(velocity) ? std::hypot(velocity.x, velocity.z) : 0.0F;
        const float reach = horizontal > 0.001F ? number(extend) : 0.0F;
        const float direction_x = horizontal > 0.001F ? velocity.x / horizontal : 0.0F;
        const float direction_z = horizontal > 0.001F ? velocity.z / horizontal : 0.0F;
        sdk::BlockPos previous{};
        bool have_previous = false;
        const int steps = static_cast<int>(std::ceil(reach * 4.0F));
        for (int step = 0; step <= steps; ++step) {
            const float distance = std::min(reach, static_cast<float>(step) * 0.25F);
            sdk::BlockPos block{};
            if (!block_position(position.x + direction_x * distance, y, position.z + direction_z * distance, block)) continue;
            if (have_previous && same_block(previous, block)) continue;
            previous = block;
            have_previous = true;
            if (!game.replaceable(block)) continue;
            cooldown_ = number(delay) / 1000.0F;
            // The bridge validates a real supporting face, item and reach;
            // placement cannot create blocks in unsupported air.
            game.place_block(block, auto_select.get<bool>());
            break;
        }
    } catch (...) { cooldown_ = 0.25F; }
}

} // namespace medicon
