#pragma once

#include "medicon/core/Setting.hpp"
#include "medicon/sdk/Gameplay.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace medicon::gameplay_detail {

inline constexpr float radians_to_degrees = 57.29577951308232F;
inline constexpr float degrees_to_radians = 1.0F / radians_to_degrees;

inline bool finite(sdk::Vec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

inline bool valid_bounds(const sdk::Bounds& bounds) noexcept {
    return finite(bounds.min) && finite(bounds.max) &&
        bounds.min.x <= bounds.max.x && bounds.min.z <= bounds.max.z &&
        bounds.max.y - bounds.min.y > 0.01F;
}

inline bool ready(const sdk::GameplayFrame& frame) noexcept {
    return frame.ready && frame.local.id != 0 && frame.local.alive && finite(frame.eye) &&
        std::isfinite(frame.yaw) && std::isfinite(frame.pitch);
}

// No accumulated catch-up after stalls; attacks and placements run once per
// game tick and rotations cannot jump through several seconds of missed ticks.
inline float tick_delta(float delta) noexcept {
    return std::isfinite(delta) ? std::clamp(delta, 0.0F, 0.25F) : 0.0F;
}

inline float number(const Setting& setting) {
    const float value = setting.get<float>();
    return std::isfinite(value) ? std::clamp(value, setting.minimum, setting.maximum) : setting.minimum;
}

inline float wrap_yaw(float yaw) noexcept {
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

inline Target find_target(const sdk::GameplayFrame& frame, float range, bool players_only,
                          bool visible_only, float half_fov, bool prefer_angle,
                          std::uintptr_t preferred_id = 0) noexcept {
    Target best;
    Target preferred;
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

        const Target candidate{&actor, yaw, pitch, distance, angle};
        if (actor.id == preferred_id) {
            preferred = candidate;
            continue;
        }
        const float score = prefer_angle ? angle : distance;
        const float best_score = prefer_angle ? best.angle : best.distance;
        if (!best.actor || score < best_score ||
            (score == best_score && (distance < best.distance ||
             (distance == best.distance && actor.id < best.actor->id)))) {
            best = candidate;
        }
    }
    return preferred.actor ? preferred : best;
}

inline bool block_position(float x, float y, float z, sdk::BlockPos& result) noexcept {
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

inline bool same_block(sdk::BlockPos a, sdk::BlockPos b) noexcept {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

// The level a bridge goes at: the highest cell whose top is at or just below
// the feet. Between levels (jumping, a slab, the first moment of a fall) the
// cell around the feet would intersect the player and the game refuses it.
// Same as Solstice's startY = position.y - 2.62 (eye height 1.62, minus one).
inline bool support_level(float feet, int& level) noexcept {
    if (!std::isfinite(feet)) return false;
    const double value = std::floor(static_cast<double>(feet) + 0.01) - 1.0;
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) return false;
    level = static_cast<int>(value);
    return true;
}

// Places `target`, or else a cell beside it that has a support face so the
// next attempt can reach it (diagonal steps leave the target on an edge only).
// The beside cells are tried nearest the player first, as Solstice does.
inline void place_or_support(sdk::GameplayAccess& game, sdk::BlockPos target, sdk::Vec3 feet, bool auto_select) {
    if (game.place_block(target, auto_select)) return;
    std::array<sdk::BlockPos, 4> beside{{{target.x + 1, target.y, target.z}, {target.x - 1, target.y, target.z},
                                         {target.x, target.y, target.z + 1}, {target.x, target.y, target.z - 1}}};
    const auto distance = [&](sdk::BlockPos cell) {
        const float dx = static_cast<float>(cell.x) + 0.5F - feet.x, dz = static_cast<float>(cell.z) + 0.5F - feet.z;
        return dx * dx + dz * dz;
    };
    std::sort(beside.begin(), beside.end(), [&](sdk::BlockPos a, sdk::BlockPos b) { return distance(a) < distance(b); });
    for (const auto cell : beside)
        if (game.replaceable(cell) && game.place_block(cell, auto_select)) return;
}

} // namespace medicon::gameplay_detail
