#include "medicon/modules/RegenModule.hpp"

#include "GameplayModuleUtils.hpp"
#include "medicon/sdk/Gameplay.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace medicon {
namespace {

bool redstone_ore(std::uint16_t id) noexcept {
    return id == 73 || id == 74 || id == 658 ||
        id == static_cast<std::uint16_t>(-403) || id == static_cast<std::uint16_t>(-404);
}

std::uint8_t hit_face(sdk::Vec3 eye, sdk::BlockPos block) noexcept {
    const float dx = eye.x - (static_cast<float>(block.x) + 0.5F);
    const float dy = eye.y - (static_cast<float>(block.y) + 0.5F);
    const float dz = eye.z - (static_cast<float>(block.z) + 0.5F);
    if (std::abs(dy) >= std::abs(dx) && std::abs(dy) >= std::abs(dz)) return dy >= 0.0F ? 1 : 0;
    if (std::abs(dx) >= std::abs(dz)) return dx >= 0.0F ? 5 : 4;
    return dz >= 0.0F ? 3 : 2;
}

} // namespace

RegenModule::RegenModule()
    : GameplayModule(module_id, "Regen", "Automatically mines nearby redstone ore with the best-tier hotbar pickaxe.", Category::Player),
      range(add_setting({"range", "Range", "Maximum distance to search for redstone ore.", 5.0F, 1.0F, 8.0F, 0.1F})),
      destroy_speed(add_setting({"destroySpeed", "Destroy Speed", "Break-time multiplier: 1.00 is vanilla; 0.50 takes half the vanilla time; 0.00 is nearly instant.", 1.0F, 0.0F, 1.0F, 0.01F})),
      rotation_percentage(add_setting({"rotationPercentage", "Rotation Percentage", "Keep aiming at the target until this fraction of break progress.", 1.0F, 0.01F, 1.0F, 0.01F})),
      infinity_durability(add_setting({"infinityDurability", "Infinity Durability", "Switch to a non-damageable item for the final hit, then restore your original slot.", false})),
      swing(add_setting({"swing", "Swing", "Arm swing while mining.", Choice{0, {"None", "Always", "Breaked"}}})),
      switch_mode(add_setting({"switch", "Switch", "Reactive waits for the target block to change; Fastchange advances at full progress.",
                               Choice{0, {"Reactive", "Fastchange"}}})) {}

void RegenModule::clear_target() noexcept {
    target_ = {};
    face_ = 0;
    target_yaw_ = target_pitch_ = progress_ = 0.0F;
    target_valid_ = false;
    pickaxe_slot_ = safe_slot_ = -1;
    phase_ = Phase::idle;
    mining_started_ = false;
}

void RegenModule::on_enable() noexcept {
    local_id_ = 0;
    scan_cooldown_ = 0.0F;
    if (!release_requested_.load(std::memory_order_acquire)) {
        original_slot_ = -1;
        slot_changed_ = false;
        clear_target();
        completed_target_valid_ = false;
        diagnostic_status_.store(DiagnosticStatus::idle, std::memory_order_release);
    }
}

void RegenModule::on_disable() noexcept {
    release_requested_.store(true, std::memory_order_release);
}

void RegenModule::release(sdk::GameplayAccess& game, DiagnosticStatus final_status) noexcept {
    const auto& frame = game.frame();
    const bool same_player = frame.ready && local_id_ != 0 && local_id_ == frame.local.id;
    if (same_player) {
        game.stop_destroy_block();
        const int selected = game.selected_hotbar_slot();
        if (slot_changed_ && original_slot_ >= 0 &&
            (selected == pickaxe_slot_ || selected == safe_slot_))
            (void)game.select_hotbar_slot(original_slot_);
    }
    local_id_ = 0;
    scan_cooldown_ = 0.0F;
    original_slot_ = -1;
    slot_changed_ = false;
    clear_target();
    release_requested_.store(false, std::memory_order_release);
    diagnostic_status_.store(final_status, std::memory_order_release);
}

bool RegenModule::find_target(sdk::GameplayAccess& game, sdk::BlockPos& result,
                              std::uint8_t& face, float& yaw, float& pitch,
                              const sdk::BlockPos* excluded) const {
    const auto& frame = game.frame();
    const float reach = gameplay_detail::number(range);
    const int extent = static_cast<int>(std::ceil(reach));
    const int min_x = static_cast<int>(std::floor(frame.eye.x)) - extent;
    const int max_x = static_cast<int>(std::floor(frame.eye.x)) + extent;
    const int min_y = static_cast<int>(std::floor(frame.eye.y)) - extent;
    const int max_y = static_cast<int>(std::floor(frame.eye.y)) + extent;
    const int min_z = static_cast<int>(std::floor(frame.eye.z)) - extent;
    const int max_z = static_cast<int>(std::floor(frame.eye.z)) + extent;
    float best_angle = std::numeric_limits<float>::max();
    float best_distance = std::numeric_limits<float>::max();
    bool found = false;
    for (int x = min_x; x <= max_x; ++x) for (int y = min_y; y <= max_y; ++y) for (int z = min_z; z <= max_z; ++z) {
        const sdk::BlockPos candidate{x, y, z};
        if (excluded && candidate.x == excluded->x && candidate.y == excluded->y && candidate.z == excluded->z)
            continue;
        const sdk::Vec3 center{static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F,
                               static_cast<float>(z) + 0.5F};
        if (!gameplay_detail::finite(center)) continue;
        const float dx = center.x - frame.eye.x;
        const float dy = center.y - frame.eye.y;
        const float dz = center.z - frame.eye.z;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!std::isfinite(distance) || distance > reach) continue;
        std::uint16_t id{};
        if (!game.block_id(candidate, id) || !redstone_ore(id)) continue;
        float candidate_yaw{}, candidate_pitch{};
        if (!gameplay_detail::look_angles(frame, center, candidate_yaw, candidate_pitch)) continue;
        const float yaw_delta = std::remainder(candidate_yaw - frame.yaw, 360.0F) * gameplay_detail::degrees_to_radians;
        const float pitch_delta = (candidate_pitch - frame.pitch) * gameplay_detail::degrees_to_radians;
        const float angle = std::hypot(yaw_delta * std::cos(frame.pitch * gameplay_detail::degrees_to_radians), pitch_delta);
        if (!std::isfinite(angle)) continue;
        if (!found || distance < best_distance || (distance == best_distance && angle < best_angle)) {
            found = true;
            best_angle = angle;
            best_distance = distance;
            result = candidate;
            face = hit_face(frame.eye, candidate);
            yaw = candidate_yaw;
            pitch = candidate_pitch;
        }
    }
    return found;
}

void RegenModule::game_tick(sdk::GameplayAccess& game, float delta_seconds) noexcept {
    (void)delta_seconds;
    if (release_requested_.load(std::memory_order_acquire)) {
        release(game);
        if (!enabled()) return;
    }
    if (!enabled()) { release(game); return; }
    try {
        const auto& frame = game.frame();
        if (!gameplay_detail::ready(frame)) { release(game); return; }
        const int swing_mode = swing.get<Choice>().index;
        if (local_id_ != frame.local.id) {
            release(game);
            local_id_ = frame.local.id;
        }
        if (completed_target_valid_) {
            std::uint16_t completed_id{};
            if (game.block_id(completed_target_, completed_id) && !redstone_ore(completed_id))
                completed_target_valid_ = false;
        }
        const float delta = gameplay_detail::tick_delta(delta_seconds);
        if (delta <= 0.0F) return;
        scan_cooldown_ = std::max(0.0F, scan_cooldown_ - delta);

        if (target_valid_) {
            std::uint16_t current_id{};
            // In a local integrated world the server may remove the block
            // before the client's native GameMode reports `destroyed=true`.
            // Treat the confirmed block-state change as the break edge so the
            // Breaked swing is emitted once for that completed target.
            if (game.block_id(target_, current_id) && !redstone_ore(current_id)) {
                if (mining_started_ && swing_mode == 2) (void)game.mining_swing();
                completed_target_ = target_;
                completed_target_valid_ = true;
                game.stop_destroy_block();
                target_valid_ = false;
                diagnostic_status_.store(DiagnosticStatus::broken, std::memory_order_release);
                if (slot_changed_) release_requested_.store(true, std::memory_order_release);
                else clear_target();
                scan_cooldown_ = 0.0F;
                return;
            }
            const sdk::Vec3 center{static_cast<float>(target_.x) + 0.5F,
                                   static_cast<float>(target_.y) + 0.5F,
                                   static_cast<float>(target_.z) + 0.5F};
            const float dx = center.x - frame.eye.x;
            const float dy = center.y - frame.eye.y;
            const float dz = center.z - frame.eye.z;
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (distance > gameplay_detail::number(range)) {
                release(game);
                local_id_ = frame.local.id;
                return;
            }
        } else if (scan_cooldown_ <= 0.0F) {
            diagnostic_status_.store(DiagnosticStatus::scanning, std::memory_order_release);
            if (find_target(game, target_, face_, target_yaw_, target_pitch_,
                            completed_target_valid_ ? &completed_target_ : nullptr)) {
                target_valid_ = true;
                progress_ = 0.0F;
                phase_ = Phase::idle;
                diagnostic_status_.store(DiagnosticStatus::target_found, std::memory_order_release);
            } else {
                diagnostic_status_.store(DiagnosticStatus::no_target, std::memory_order_release);
            }
            scan_cooldown_ = 0.2F;
        }
        if (!target_valid_) return;
        const sdk::Vec3 current_target_center{static_cast<float>(target_.x) + 0.5F,
                                              static_cast<float>(target_.y) + 0.5F,
                                              static_cast<float>(target_.z) + 0.5F};
        if (!gameplay_detail::look_angles(frame, current_target_center, target_yaw_, target_pitch_)) {
            release(game);
            local_id_ = frame.local.id;
            return;
        }
        if (progress_ < gameplay_detail::number(rotation_percentage))
            (void)game.rotate_head_and_body(target_yaw_, target_pitch_);

        if (original_slot_ < 0) original_slot_ = game.selected_hotbar_slot();
        if (original_slot_ < 0) return;

        if (pickaxe_slot_ < 0) {
            float best_speed = 0.0F;
            for (int slot = 0; slot < 9; ++slot) {
                sdk::HotbarItemInfo info{};
                if (game.hotbar_item(slot, target_, info) && info.occupied && info.pickaxe &&
                    info.destroy_speed > best_speed) {
                    best_speed = info.destroy_speed;
                    pickaxe_slot_ = slot;
                }
            }
            if (pickaxe_slot_ < 0) {
                // Item metadata is not readable on every supported client
                // layout. Keep mining with the already selected item instead
                // of abandoning a valid redstone target.
                pickaxe_slot_ = original_slot_;
                diagnostic_status_.store(DiagnosticStatus::pickaxe_fallback, std::memory_order_release);
            }
        }
        const int selected = game.selected_hotbar_slot();
        if (phase_ == Phase::idle && selected != pickaxe_slot_) {
            if (game.select_hotbar_slot(pickaxe_slot_)) {
                slot_changed_ = true;
                phase_ = Phase::wait_pickaxe;
                diagnostic_status_.store(DiagnosticStatus::switching_pickaxe, std::memory_order_release);
            }
            return;
        }
        if (phase_ == Phase::wait_pickaxe) {
            if (selected != pickaxe_slot_) return;
            phase_ = Phase::mining;
            diagnostic_status_.store(DiagnosticStatus::waiting_for_pickaxe, std::memory_order_release);
        }
        if (phase_ == Phase::idle && selected == pickaxe_slot_) phase_ = Phase::mining;
        if (phase_ == Phase::wait_safe_item) {
            if (selected != safe_slot_) return;
            phase_ = Phase::mining;
            diagnostic_status_.store(DiagnosticStatus::waiting_for_safe_item, std::memory_order_release);
        }
        if (phase_ == Phase::mining && selected != pickaxe_slot_ && selected != safe_slot_) {
            if (game.select_hotbar_slot(pickaxe_slot_)) {
                slot_changed_ = true;
                phase_ = Phase::wait_pickaxe;
                diagnostic_status_.store(DiagnosticStatus::switching_pickaxe, std::memory_order_release);
            }
            return;
        }

        if (infinity_durability.get<bool>() && phase_ == Phase::mining && selected == pickaxe_slot_ &&
            game.should_switch_before_destroy(gameplay_detail::number(destroy_speed))) {
            for (int slot = 0; slot < 9; ++slot) {
                if (slot == pickaxe_slot_) continue;
                sdk::HotbarItemInfo info{};
                if (!game.hotbar_item(slot, target_, info) || !info.occupied || info.damageable) continue;
                if (game.prepare_durability_safe_finish() && game.select_hotbar_slot(slot)) {
                    safe_slot_ = slot;
                    slot_changed_ = true;
                    phase_ = Phase::wait_safe_item;
                    diagnostic_status_.store(DiagnosticStatus::switching_safe_item, std::memory_order_release);
                    return;
                }
                break;
            }
        }

        bool destroyed = false;
        if (!mining_started_)
            diagnostic_status_.store(DiagnosticStatus::starting_mine, std::memory_order_release);
        if (!game.advance_destroy_block(target_, face_, gameplay_detail::number(destroy_speed), progress_, destroyed)) {
            release(game, DiagnosticStatus::mine_failed);
            local_id_ = frame.local.id;
            return;
        }
        mining_started_ = true;
        const bool fastchange_complete = switch_mode.get<Choice>().index == 1 && progress_ >= 1.0F;
        if (swing_mode == 1) (void)game.mining_swing();
        if (!destroyed) diagnostic_status_.store(DiagnosticStatus::mining, std::memory_order_release);
        if (destroyed || fastchange_complete) {
            if (swing_mode == 2) (void)game.mining_swing();
            game.stop_destroy_block();
            completed_target_ = target_;
            completed_target_valid_ = true;
            target_valid_ = false;
            diagnostic_status_.store(DiagnosticStatus::broken, std::memory_order_release);
            if (slot_changed_) release_requested_.store(true, std::memory_order_release);
            else clear_target();
            scan_cooldown_ = 0.0F;
        }
    } catch (...) {
        release(game);
        local_id_ = game.frame().local.id;
    }
}

} // namespace medicon
