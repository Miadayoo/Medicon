#pragma once

#include <medicon/core/Setting.hpp>
#include <medicon/sdk/Gameplay.hpp>

namespace medicon {

[[nodiscard]] bool should_sprint(const sdk::GameplayFrame& frame, const Choice& selection,
                                 bool sprint_fix, float rotation_yaw) noexcept;
bool apply_sprint(sdk::GameplayAccess& game, bool sprinting) noexcept;

} // namespace medicon
