#pragma once

#include "medicon/modules/GameplayModule.hpp"
#include "medicon/sdk/Sprint.hpp"

#include <cstdint>

namespace medicon {

class SprintModule final : public GameplayModule {
public:
    static constexpr const char* module_id = "sprint";

    SprintModule();
    void game_tick(sdk::GameplayAccess& game, float delta_seconds) noexcept override;
    void update(sdk::GameplayAccess& game, bool allow_actions,
                bool sprint_fix = false, float rotation_yaw = 0.0F) noexcept;
    [[nodiscard]] bool needs_restore() const noexcept { return controlling_sprint_; }
    [[nodiscard]] bool packet_sprint_override() const noexcept { return enabled() && controlling_sprint_; }
    [[nodiscard]] bool requested_sprinting() const noexcept { return requested_sprinting_; }

    Setting& mode;

private:
    bool controlling_sprint_{};
    bool requested_sprinting_{};
    std::uintptr_t local_id_{};
};

} // namespace medicon
