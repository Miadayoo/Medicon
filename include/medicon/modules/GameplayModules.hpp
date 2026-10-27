#pragma once

#include "medicon/core/Module.hpp"

#include <cstdint>

namespace medicon::sdk { class GameplayAccess; }

namespace medicon {

// Called from the local player's native game tick. The overlay's on_tick must
// never read or mutate game objects, which belong to the game thread.
class GameplayModule : public Module {
public:
    using Module::Module;
    virtual void game_tick(sdk::GameplayAccess& game, float delta_seconds) noexcept = 0;
};

class DerpModule final : public GameplayModule {
public:
    static constexpr const char* module_id = "derp";
    DerpModule();
    void game_tick(sdk::GameplayAccess& game, float delta_seconds) noexcept override;

    Setting& speed;
    Setting& pitch;

protected:
    void on_enable() noexcept override;
    void on_disable() noexcept override;

private:
    std::uintptr_t local_id_ = 0;
    float yaw_ = 0.0F;
};

class AimbotModule final : public GameplayModule {
public:
    static constexpr const char* module_id = "aimbot";
    AimbotModule();
    void game_tick(sdk::GameplayAccess& game, float delta_seconds) noexcept override;

    Setting& range;
    Setting& fov;
    Setting& smoothing;
    Setting& players_only;
    Setting& visible_only;
    Setting& require_click;
};

class KillauraModule final : public GameplayModule {
public:
    static constexpr const char* module_id = "killaura";
    KillauraModule();
    void game_tick(sdk::GameplayAccess& game, float delta_seconds) noexcept override;

    Setting& range;
    Setting& cps;
    Setting& rotate;
    Setting& players_only;
    Setting& visible_only;

protected:
    void on_enable() noexcept override;
    void on_disable() noexcept override;

private:
    std::uintptr_t local_id_ = 0;
    float cooldown_ = 0.0F;
};

class ScaffoldModule final : public GameplayModule {
public:
    static constexpr const char* module_id = "scaffold";
    ScaffoldModule();
    void game_tick(sdk::GameplayAccess& game, float delta_seconds) noexcept override;

    Setting& extend;
    Setting& auto_select;
    Setting& delay;

protected:
    void on_enable() noexcept override;
    void on_disable() noexcept override;

private:
    std::uintptr_t local_id_ = 0;
    float cooldown_ = 0.0F;
};

} // namespace medicon
