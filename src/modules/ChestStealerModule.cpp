#include "medicon/modules/ChestStealerModule.hpp"

#include "GameplayModuleUtils.hpp"
#include "medicon/sdk/Gameplay.hpp"

#include <algorithm>

namespace medicon {
namespace {

// Screen ticks arrive every frame, so a longer gap is a different chest.
constexpr std::uint64_t new_screen_gap_ms = 500;
// Taken stacks settle before an emptied chest closes.
constexpr std::uint64_t close_settle_ms = 200;

} // namespace

ChestStealerModule::ChestStealerModule()
    : Module(module_id, "ChestStealer", "Takes every item out of an opened chest.", Category::Player),
      delay(add_setting({"delay", "Delay (ms)", "Time between taking items.", 100.0F, 0.0F, 1000.0F, 10.0F})),
      auto_close(add_setting({"autoClose", "Auto Close", "Close the chest once it has been emptied.", true})) {}

void ChestStealerModule::on_enable() noexcept {
    last_tick_ms_ = next_take_ms_ = close_at_ms_ = 0;
    took_any_ = false;
}

void ChestStealerModule::on_disable() noexcept { on_enable(); }

void ChestStealerModule::screen_tick(sdk::ContainerScreen& screen, std::uint64_t now_ms) noexcept {
    if (!enabled()) return;
    try {
        const auto wait = static_cast<std::uint64_t>(gameplay_detail::number(delay));
        if (last_tick_ms_ == 0 || now_ms - last_tick_ms_ > new_screen_gap_ms) {
            next_take_ms_ = now_ms + wait;
            close_at_ms_ = 0;
            took_any_ = false;
        }
        last_tick_ms_ = now_ms;
        if (!screen.chest()) return;
        int slot = -1;
        for (int candidate = 0; candidate < sdk::ContainerScreen::slot_limit; ++candidate) {
            if (screen.slot_occupied(candidate)) { slot = candidate; break; }
        }
        if (slot < 0) {
            // A chest that was never touched stays open: its contents may
            // simply not have arrived yet.
            if (!took_any_ || !auto_close.get<bool>()) return;
            if (close_at_ms_ == 0) close_at_ms_ = std::max(now_ms + close_settle_ms, next_take_ms_);
            if (now_ms < close_at_ms_) return;
            (void)screen.close();
            took_any_ = false;
            close_at_ms_ = 0;
            return;
        }
        close_at_ms_ = 0;
        if (now_ms < next_take_ms_) return;
        if (screen.take(slot)) took_any_ = true;
        next_take_ms_ = now_ms + wait;
    } catch (...) {}
}

} // namespace medicon
