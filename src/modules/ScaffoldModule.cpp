#include "medicon/modules/ScaffoldModule.hpp"

#include "GameplayModuleUtils.hpp"
#include "medicon/sdk/Gameplay.hpp"

#include <algorithm>
#include <cmath>

namespace medicon {

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
        if (!gameplay_detail::ready(frame)) { on_enable(); return; }
        if (local_id_ != frame.local.id) { local_id_ = frame.local.id; cooldown_ = 0.0F; }
        const float delta = gameplay_detail::tick_delta(delta_seconds);
        if (delta <= 0.0F || !gameplay_detail::valid_bounds(frame.local.bounds) ||
            !gameplay_detail::finite(frame.local.position)) return;
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
            // The bridge validates a real supporting face, item and reach;
            // placement cannot create blocks in unsupported air.
            gameplay_detail::place_or_support(
                game, block, {position.x, frame.local.bounds.min.y, position.z}, auto_select.get<bool>());
            break;
        }
    } catch (...) { cooldown_ = 0.25F; }
}

} // namespace medicon
