#include "medicon/modules/ScaffoldModule.hpp"

#include "GameplayModuleUtils.hpp"
#include "medicon/sdk/Gameplay.hpp"

#include <algorithm>
#include <cmath>

namespace medicon {
namespace {

// Auto Select reaches the whole hotbar; without it only the held stack places.
bool placeable_blocks(const sdk::GameplayAccess& game, bool auto_select, int& total) {
    const int held = game.selected_hotbar_slot();
    int blocks = 0;
    for (int slot = 0; slot < 9; ++slot) {
        if (!auto_select && slot != held) continue;
        sdk::InventoryItemInfo item{};
        if (!game.inventory_item(slot, item)) return false;
        if (item.kind == sdk::InventoryItemKind::block) blocks += item.count;
    }
    total = blocks;
    return true;
}

} // namespace

ScaffoldModule::ScaffoldModule()
    : GameplayModule(module_id, "Scaffold", "Place blocks below your feet and extend in your movement direction.", Category::Movement),
      extend(add_setting({"extend", "Extend", "Look ahead along horizontal movement, in blocks.", 1.0F, 0.0F, 4.0F, 0.25F})),
      auto_select(add_setting({"autoSelect", "Auto Select", "Temporarily select a placeable hotbar block, then restore your slot.", true})),
      delay(add_setting({"delay", "Delay", "Minimum milliseconds between placement attempts.", 100.0F, 0.0F, 500.0F, 10.0F})),
      rotate(add_setting({"rotate", "Rotate", "None keeps the current view; Flick uses the existing placement rotation; Simple snaps to a new target and holds it for one second.", Choice{0, {"None", "Flick", "Simple"}}})) {}

void ScaffoldModule::on_enable() noexcept {
    local_id_ = 0;
    cooldown_ = 0.0F;
    rotation_hold_remaining_ = 0.0F;
    rotation_hold_active_ = false;
    rotation_hold_block_valid_ = false;
    block_count_ = -1;
}
void ScaffoldModule::on_disable() noexcept { on_enable(); }

void ScaffoldModule::game_tick(sdk::GameplayAccess& game, float delta_seconds) noexcept {
    if (!enabled()) return;
    try {
        const auto& frame = game.frame();
        if (!gameplay_detail::ready(frame)) { on_enable(); return; }
        // Ahead of the cooldown: the count must follow pickups between placements.
        // A slot that cannot be read this tick keeps the last known count.
        (void)placeable_blocks(game, auto_select.get<bool>(), block_count_);
        view_yaw_ = frame.yaw;
        if (local_id_ != frame.local.id) {
            local_id_ = frame.local.id;
            cooldown_ = 0.0F;
            rotation_hold_remaining_ = 0.0F;
            rotation_hold_active_ = false;
            rotation_hold_block_valid_ = false;
        }
        const float delta = gameplay_detail::tick_delta(delta_seconds);
        if (delta <= 0.0F || !gameplay_detail::valid_bounds(frame.local.bounds) ||
            !gameplay_detail::finite(frame.local.position)) return;
        const unsigned rotation_mode = rotate.get<Choice>().index;
        if (rotation_mode != 2) {
            rotation_hold_remaining_ = 0.0F;
            rotation_hold_active_ = false;
            rotation_hold_block_valid_ = false;
        } else if (rotation_hold_active_) {
            rotation_hold_remaining_ = std::max(0.0F, rotation_hold_remaining_ - delta);
            if (rotation_hold_remaining_ > 0.0F)
                (void)game.rotate_head_and_body_instant(rotation_hold_yaw_, rotation_hold_pitch_);
            else
                rotation_hold_active_ = false;
        }
        cooldown_ = std::max(0.0F, cooldown_ - delta);
        if (cooldown_ > 0.00001F) return;

        const auto position = frame.local.position;
        int level = 0;
        if (!gameplay_detail::support_level(frame.local.bounds.min.y, level)) return;
        // Cell centres of the level, so floor() below picks exactly that level.
        const float y = static_cast<float>(level) + 0.5F;
        const auto velocity = frame.local.velocity;
        const float horizontal = gameplay_detail::finite(velocity) ? std::hypot(velocity.x, velocity.z) : 0.0F;
        const float reach = horizontal > 0.001F ? gameplay_detail::number(extend) : 0.0F;
        const float direction_x = horizontal > 0.001F ? velocity.x / horizontal : 0.0F;
        const float direction_z = horizontal > 0.001F ? velocity.z / horizontal : 0.0F;
        sdk::BlockPos previous{};
        bool have_previous = false;
        const int steps = static_cast<int>(std::ceil(reach * 4.0F));
        for (int step = 0; step <= steps; ++step) {
            const float distance = std::min(reach, static_cast<float>(step) * 0.25F);
            sdk::BlockPos block{};
            if (!gameplay_detail::block_position(position.x + direction_x * distance, y,
                                                  position.z + direction_z * distance, block)) continue;
            if (have_previous && gameplay_detail::same_block(previous, block)) continue;
            previous = block;
            have_previous = true;
            if (!game.replaceable(block)) continue;
            cooldown_ = gameplay_detail::number(delay) / 1000.0F;
            if (rotation_mode == 1 || rotation_mode == 2) {
                const sdk::Vec3 center{static_cast<float>(block.x) + 0.5F,
                                       static_cast<float>(block.y) + 0.5F,
                                       static_cast<float>(block.z) + 0.5F};
                float yaw = 0.0F;
                float pitch = 0.0F;
                if (gameplay_detail::look_angles(frame, center, yaw, pitch)) {
                    if (rotation_mode == 1) {
                        (void)game.rotate_head_and_body(yaw, pitch);
                    } else {
                        const bool same_block = rotation_hold_block_valid_ &&
                            rotation_hold_block_x_ == block.x && rotation_hold_block_y_ == block.y &&
                            rotation_hold_block_z_ == block.z;
                        if (!same_block) {
                            rotation_hold_block_x_ = block.x;
                            rotation_hold_block_y_ = block.y;
                            rotation_hold_block_z_ = block.z;
                            rotation_hold_block_valid_ = true;
                            rotation_hold_yaw_ = yaw;
                            rotation_hold_pitch_ = pitch;
                            rotation_hold_remaining_ = 1.0F;
                            rotation_hold_active_ = true;
                            (void)game.rotate_head_and_body_instant(yaw, pitch);
                        }
                    }
                }
            }
            // The bridge validates a real supporting face, item and reach;
            // placement cannot create blocks in unsupported air.
            gameplay_detail::place_or_support(
                game, block, {position.x, frame.local.bounds.min.y, position.z}, auto_select.get<bool>());
            break;
        }
    } catch (...) { cooldown_ = 0.25F; }
}

} // namespace medicon
