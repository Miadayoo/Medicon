#pragma once

#include "medicon/modules/GameplayModule.hpp"
#include "medicon/sdk/Gameplay.hpp"

#include <array>
#include <cstdint>

namespace medicon {

class InvManagerModule final : public GameplayModule {
public:
    static constexpr const char* module_id = "invmanager";
    static constexpr int inventory_slots = 36;
    static constexpr int hotbar_slots = 9;
    static constexpr int armor_slots = 4;
    InvManagerModule();
    void game_tick(sdk::GameplayAccess& game, float delta_seconds) noexcept override;
    struct Diagnostic {
        const char* status = "waiting-for-tick";
        std::uint64_t ticks{}, attempts{}, sent{};
        int first = -1, second = -1;
        int move_type = -1; // Move::Type of the last attempt.
        bool armor_known = false;
    };
    [[nodiscard]] const Diagnostic& diagnostic() const noexcept { return diagnostic_; }

    Setting& mode;
    Setting& delay;
    Setting& auto_armor;
    std::array<Setting*, hotbar_slots> slots;

protected:
    void on_enable() noexcept override;
    void on_disable() noexcept override;

private:
    // The inventory, then the worn armor from slot `inventory_slots` on.
    using Items = std::array<sdk::InventoryItemInfo, inventory_slots + armor_slots>;
    struct Move {
        enum class Type : std::uint8_t { swap, equip, drop } type = Type::swap;
        int first = -1, second = -1;
    };
    [[nodiscard]] int assigned_slots(sdk::InventoryItemKind kind) const;
    [[nodiscard]] bool assigned_slot(int slot, sdk::InventoryItemKind kind) const;
    [[nodiscard]] bool plan_drop(const Items& items, bool armor_known, Move& move) const;
    [[nodiscard]] bool plan_hotbar(const Items& items, Move& move) const;
    [[nodiscard]] bool plan_equip(const Items& items, Move& move) const;
    [[nodiscard]] bool plan_sort(const Items& items, Move& move) const;

    std::uintptr_t local_id_ = 0;
    std::uint64_t world_id_ = 0;
    float action_wait_ms_ = 0.0F;
    float confirm_wait_ms_ = 0.0F;
    float repeat_window_ms_ = 0.0F;
    bool pending_ = false;
    Move pending_move_{};
    Move last_move_{};
    sdk::InventoryItemInfo expected_first_{}, expected_second_{};
    Diagnostic diagnostic_{};
};

} // namespace medicon
