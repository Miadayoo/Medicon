#include "medicon/modules/InvManagerModule.hpp"

#include "GameplayModuleUtils.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace medicon {
namespace {

using sdk::InventoryItemInfo;
using sdk::InventoryItemKind;

// Wait for both affected slots to settle before planning another operation.
// An unconfirmed operation is retried slowly, not resent every tick.
constexpr float confirm_timeout_ms = 1000.0F;
constexpr float refused_retry_ms = 5000.0F;

constexpr std::array<InventoryItemKind, 6> slot_kinds{
    InventoryItemKind::empty, InventoryItemKind::sword, InventoryItemKind::pickaxe,
    InventoryItemKind::axe, InventoryItemKind::shovel, InventoryItemKind::block};

constexpr std::array<InventoryItemKind, InvManagerModule::armor_slots> armor_kinds{
    InventoryItemKind::helmet, InventoryItemKind::chestplate,
    InventoryItemKind::leggings, InventoryItemKind::boots};

Setting slot_setting(int slot, int selected) {
    const auto number = std::to_string(slot + 1);
    return {"slot" + number, "Slot " + number, "Item kept in hotbar slot " + number + ".",
            Choice{selected, {"None", "Sword", "Pickaxe", "Axe", "Shovel", "Block"}}};
}

float rank(const InventoryItemInfo& item) noexcept {
    return item.kind == InventoryItemKind::block ? static_cast<float>(item.count) : item.value;
}

bool gear(InventoryItemKind kind) noexcept { return kind >= InventoryItemKind::sword; }
bool armor(InventoryItemKind kind) noexcept { return kind >= InventoryItemKind::helmet; }

std::size_t index(int slot) noexcept { return static_cast<std::size_t>(slot); }

} // namespace

InvManagerModule::InvManagerModule()
    : GameplayModule(module_id, "InvManager", "Drops weaker gear, sorts your inventory and keeps your best items in the hotbar slots you choose.", Category::Player),
      mode(add_setting({"mode", "Mode", "On Inventory works while the inventory is open; Always also works during play.",
                        Choice{0, {"On Inventory", "Always"}}})),
      delay(add_setting({"delay", "Delay (ms)", "Time between inventory moves.", 100.0F, 0.0F, 1000.0F, 10.0F})),
      auto_armor(add_setting({"autoArmor", "Auto Armor", "Wear the strongest armor in your inventory.", false})),
      slots{&add_setting(slot_setting(0, 1)), &add_setting(slot_setting(1, 2)), &add_setting(slot_setting(2, 3)),
            &add_setting(slot_setting(3, 5)), &add_setting(slot_setting(4, 0)), &add_setting(slot_setting(5, 0)),
            &add_setting(slot_setting(6, 0)), &add_setting(slot_setting(7, 0)), &add_setting(slot_setting(8, 0))} {}

void InvManagerModule::on_enable() noexcept {
    local_id_ = 0;
    world_id_ = 0;
    action_wait_ms_ = confirm_wait_ms_ = repeat_window_ms_ = 0.0F;
    pending_ = false;
    diagnostic_ = {};
}

void InvManagerModule::on_disable() noexcept { on_enable(); }

int InvManagerModule::assigned_slots(InventoryItemKind kind) const {
    int count = 0;
    for (const auto* slot : slots) {
        const auto selected = slot->get<Choice>().index;
        if (selected > 0 && selected < static_cast<int>(slot_kinds.size()) && slot_kinds[index(selected)] == kind) ++count;
    }
    return count;
}

bool InvManagerModule::assigned_slot(int slot, InventoryItemKind kind) const {
    if (slot < 0 || slot >= hotbar_slots) return false;
    const auto selected = slots[index(slot)]->get<Choice>().index;
    return selected > 0 && selected < static_cast<int>(slot_kinds.size()) && slot_kinds[index(selected)] == kind;
}

// As Solstice: of each kind only the best survives, and armor no better than
// what is worn goes too. Two hotbar slots of one kind keep the best two.
bool InvManagerModule::plan_drop(const Items& items, bool armor_known, Move& move) const {
    for (int slot = 0; slot < inventory_slots; ++slot) {
        const auto& item = items[index(slot)];
        if (!gear(item.kind) || (armor(item.kind) && !armor_known)) continue;
        if (armor(item.kind)) {
            const auto piece = static_cast<int>(item.kind) - static_cast<int>(InventoryItemKind::helmet);
            const auto& worn = items[index(inventory_slots + piece)];
            if (worn.kind == item.kind && worn.value >= item.value) { move = {Move::Type::drop, slot, -1}; return true; }
        }
        int better = 0;
        for (int other = 0; other < inventory_slots; ++other) {
            const auto& candidate = items[index(other)];
            if (other != slot && candidate.kind == item.kind &&
                (candidate.value > item.value || (candidate.value == item.value &&
                    (assigned_slot(other, item.kind) != assigned_slot(slot, item.kind)
                        ? assigned_slot(other, item.kind) : other < slot)))) ++better;
        }
        if (better >= std::max(1, assigned_slots(item.kind))) { move = {Move::Type::drop, slot, -1}; return true; }
    }
    return false;
}

bool InvManagerModule::plan_hotbar(const Items& items, Move& move) const {
    std::array<bool, inventory_slots> claimed{};
    for (int slot = 0; slot < hotbar_slots; ++slot) {
        const auto selected = slots[index(slot)]->get<Choice>().index;
        if (selected <= 0 || selected >= static_cast<int>(slot_kinds.size())) continue;
        const auto kind = slot_kinds[index(selected)];
        int best = items[index(slot)].kind == kind ? slot : -1;
        // Any stack of blocks will do; only tools are worth upgrading in place.
        if (best < 0 || kind != InventoryItemKind::block) {
            for (int candidate = 0; candidate < inventory_slots; ++candidate) {
                const auto& item = items[index(candidate)];
                if (claimed[index(candidate)] || item.kind != kind) continue;
                if (best < 0 || rank(item) > rank(items[index(best)])) best = candidate;
            }
        }
        if (best < 0) continue;
        if (best != slot) { move = {Move::Type::swap, slot, best}; return true; }
        claimed[index(slot)] = true;
    }
    return false;
}

bool InvManagerModule::plan_equip(const Items& items, Move& move) const {
    for (int piece = 0; piece < armor_slots; ++piece) {
        const auto kind = armor_kinds[index(piece)];
        const auto& worn = items[index(inventory_slots + piece)];
        int best = -1;
        for (int slot = 0; slot < inventory_slots; ++slot) {
            const auto& item = items[index(slot)];
            if (item.kind != kind || (worn.kind == kind && item.value <= worn.value)) continue;
            if (best < 0 || item.value > items[index(best)].value) best = slot;
        }
        if (best >= 0) { move = {Move::Type::equip, best, piece}; return true; }
    }
    return false;
}

// The main inventory, top to bottom: gear by kind and strength, blocks by stack
// size, everything else with its own kind, then the empty slots.
bool InvManagerModule::plan_sort(const Items& items, Move& move) const {
    std::vector<InventoryItemInfo> order;
    for (int slot = hotbar_slots; slot < inventory_slots; ++slot)
        if (items[index(slot)].kind != InventoryItemKind::empty) order.push_back(items[index(slot)]);
    const auto group = [](InventoryItemKind kind) {
        return gear(kind) ? 0 : kind == InventoryItemKind::block ? 1 : 2;
    };
    std::stable_sort(order.begin(), order.end(), [&](const InventoryItemInfo& a, const InventoryItemInfo& b) {
        if (group(a.kind) != group(b.kind)) return group(a.kind) < group(b.kind);
        if (a.kind != b.kind) return a.kind < b.kind;
        if (a.value != b.value) return a.value > b.value;
        if (a.id != b.id) return a.id < b.id;
        return a.count > b.count;
    });
    for (std::size_t place = 0; place < order.size(); ++place) {
        const int slot = hotbar_slots + static_cast<int>(place);
        if (items[index(slot)] == order[place]) continue;
        for (int candidate = slot + 1; candidate < inventory_slots; ++candidate) {
            if (items[index(candidate)] != order[place]) continue;
            move = {Move::Type::swap, slot, candidate};
            return true;
        }
    }
    return false;
}

void InvManagerModule::game_tick(sdk::GameplayAccess& game, float delta_seconds) noexcept {
    if (!enabled()) return;
    try {
        const auto& frame = game.frame();
        if (!gameplay_detail::ready(frame)) { on_enable(); return; }
        if (local_id_ != frame.local.id || world_id_ != frame.world_identity) {
            on_enable();
            local_id_ = frame.local.id;
            world_id_ = frame.world_identity;
        }
        ++diagnostic_.ticks;
        const float elapsed = gameplay_detail::tick_delta(delta_seconds) * 1000.0F;
        action_wait_ms_ = std::max(0.0F, action_wait_ms_ - elapsed);
        confirm_wait_ms_ = std::max(0.0F, confirm_wait_ms_ - elapsed);
        repeat_window_ms_ = std::max(0.0F, repeat_window_ms_ - elapsed);
        if (mode.get<Choice>().index == 0 && !game.inventory_screen_open()) {
            diagnostic_.status = "inventory-screen-closed";
            return;
        }
        Items items{};
        for (int slot = 0; slot < inventory_slots; ++slot)
            if (!game.inventory_item(slot, items[index(slot)])) {
                diagnostic_.status = "inventory-read-failed";
                diagnostic_.first = slot;
                return;
            }
        bool armor_known = true;
        for (int piece = 0; piece < armor_slots; ++piece)
            armor_known = game.armor_item(piece, items[index(inventory_slots + piece)]) && armor_known;
        if (pending_) {
            const int second = pending_move_.type == Move::Type::equip
                ? inventory_slots + pending_move_.second : pending_move_.second;
            const bool confirmed = items[index(pending_move_.first)] == expected_first_ &&
                (pending_move_.type == Move::Type::drop ||
                    ((pending_move_.type != Move::Type::equip || armor_known) && items[index(second)] == expected_second_));
            if (!confirmed && confirm_wait_ms_ > 0.0F) { diagnostic_.status = "waiting-for-slot-update"; return; }
            pending_ = false;
            if (!confirmed) { diagnostic_.status = "slot-update-timeout"; action_wait_ms_ = refused_retry_ms; return; }
        }
        if (action_wait_ms_ > 0.0F) { diagnostic_.status = "delay-or-retry-backoff"; return; }
        Move move;
        const bool planned = plan_drop(items, armor_known, move) || plan_hotbar(items, move) ||
            (auto_armor.get<bool>() && armor_known && plan_equip(items, move)) || plan_sort(items, move);
        diagnostic_.armor_known = armor_known;
        if (!planned) { diagnostic_.status = "inventory-already-managed"; return; }
        // Local slots update on send, so a server refusal arrives after the move
        // was confirmed. Planning the same move again must not become a resend loop.
        if (repeat_window_ms_ > 0.0F && move.type == last_move_.type && move.first == last_move_.first && move.second == last_move_.second) {
            diagnostic_.status = "server-reverted-backoff";
            repeat_window_ms_ = 0.0F;
            action_wait_ms_ = refused_retry_ms;
            return;
        }
        ++diagnostic_.attempts;
        diagnostic_.move_type = static_cast<int>(move.type);
        diagnostic_.first = move.first;
        diagnostic_.second = move.second;
        action_wait_ms_ = gameplay_detail::number(delay);
        const bool sent = move.type == Move::Type::equip ? game.equip_armor(move.first, move.second)
            : move.type == Move::Type::drop ? game.drop_inventory_slot(move.first)
            : game.swap_inventory_slots(move.first, move.second);
        if (!sent) { diagnostic_.status = "native-action-failed"; action_wait_ms_ = refused_retry_ms; return; }
        ++diagnostic_.sent;
        diagnostic_.status = "sent";
        pending_ = true;
        pending_move_ = move;
        last_move_ = move;
        repeat_window_ms_ = refused_retry_ms;
        const int second = move.type == Move::Type::equip ? inventory_slots + move.second : move.second;
        expected_first_ = move.type == Move::Type::drop ? InventoryItemInfo{} : items[index(second)];
        expected_second_ = items[index(move.first)];
        confirm_wait_ms_ = confirm_timeout_ms;
    } catch (...) { on_enable(); }
}

} // namespace medicon
