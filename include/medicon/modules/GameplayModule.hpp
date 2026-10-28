#pragma once

#include "medicon/core/Module.hpp"

namespace medicon::sdk { class GameplayAccess; }

namespace medicon {

// Called from the local player's native game tick. The overlay's on_tick must
// never read or mutate game objects, which belong to the game thread.
class GameplayModule : public Module {
public:
    using Module::Module;
    virtual void game_tick(sdk::GameplayAccess& game, float delta_seconds) noexcept = 0;
};

} // namespace medicon
