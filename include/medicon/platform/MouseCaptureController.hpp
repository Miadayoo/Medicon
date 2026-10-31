#pragma once

#include <cstdint>
#include <functional>
#include <optional>

namespace medicon::platform {

// Native client discovery is a fallback check, not per-frame work. Window focus
// and native-hook installation request their own immediate synchronization.
class MouseSyncSchedule {
public:
    bool update(bool open, bool active, std::uint64_t milliseconds) noexcept;

private:
    bool open_ = false;
    std::uint64_t next_poll_ = 0;
};

// Transition policy only: the caller supplies a fresh native grabbed-state
// snapshot and invokes this controller on the game's mouse-owning thread.
class MouseCaptureController {
public:
    // A known original grab is restored after close once focus permits it.
    // An unknown original state is released once but never forces a later grab.
    // Returns false only when a required callback is missing, fails or throws;
    // a deferred restoration while unfocused is successful pending work.
    // A native UI screen owns its own cursor: cancel any gameplay restoration
    // and perform no native transition while native_ui_owns_cursor is true.
    bool update(bool open, bool focused, std::optional<bool> grabbed,
                const std::function<bool()>& release,
                const std::function<bool()>& grab, bool native_ui_owns_cursor = false) noexcept;

    // End management during shutdown. Restore an owed grab only while focused.
    // Unfocused shutdown cancels restoration, so it cannot steal desktop input.
    // A failed focused restore remains pending and can be retried with reset.
    bool reset(bool focused, const std::function<bool()>& grab) noexcept;

    // Includes a closed GUI whose original grab still awaits focus or retry.
    // Reopening before restoration preserves that existing obligation.
    bool active() const noexcept { return open_ || restore_grab_ || release_pending_; }

private:
    bool open_ = false;
    bool restore_grab_ = false;
    bool release_pending_ = false;
};

} // namespace medicon::platform
