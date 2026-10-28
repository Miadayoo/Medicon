#pragma once

#include "medicon/modules/GameplayModule.hpp"

#include <cstdint>

namespace medicon {

class DerpModule final : public GameplayModule {
public:
    static constexpr const char* module_id = "derp";
    DerpModule();
    void game_tick(sdk::GameplayAccess& game, float delta_seconds) noexcept override;

    Setting& speed;

protected:
    void on_enable() noexcept override;
    void on_disable() noexcept override;

private:
    std::uintptr_t local_id_ = 0;
    float yaw_ = 0.0F;
};

} // namespace medicon
