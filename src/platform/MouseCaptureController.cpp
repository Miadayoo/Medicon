#include <medicon/platform/MouseCaptureController.hpp>

namespace medicon::platform {
namespace {

bool invoke(const std::function<bool()>& operation) noexcept {
    if (!operation) return false;
    try { return operation(); }
    catch (...) { return false; }
}

} // namespace

bool MouseSyncSchedule::update(bool open, bool active, std::uint64_t milliseconds) noexcept {
    const bool changed = open != open_;
    open_ = open;
    if (!changed && (!(open || active) || milliseconds < next_poll_)) return false;
    next_poll_ = milliseconds + 100;
    return true;
}

bool MouseCaptureController::update(bool open, bool focused, std::optional<bool> grabbed,
                                    const std::function<bool()>& release,
                                    const std::function<bool()>& grab, bool native_ui_owns_cursor) noexcept {
    // A disconnect screen has already left gameplay and released its cursor.
    // Retire any gameplay restoration from before disconnect when the overlaid
    // account picker closes after authentication.
    if (native_ui_owns_cursor) return reset(false, {});
    if (open) {
        if (!open_) {
            // Preserve an outstanding restore across close/reopen while focus
            // was elsewhere. A free or unknown new snapshot must not erase it.
            restore_grab_ = restore_grab_ || grabbed == true;
            release_pending_ = grabbed != false;
        }
        open_ = true;
        if (grabbed == false) {
            release_pending_ = false;
            return true;
        }
        if (grabbed == true) release_pending_ = true;
        if (!release_pending_) return true;
        if (!invoke(release)) return false;
        release_pending_ = false;
        return true;
    }

    open_ = false;
    release_pending_ = false;
    if (!restore_grab_ || !focused) return true;
    // The host may already have restored capture during its own UI transition.
    if (grabbed != true && !invoke(grab)) return false;
    restore_grab_ = false;
    return true;
}

bool MouseCaptureController::reset(bool focused, const std::function<bool()>& grab) noexcept {
    open_ = false;
    release_pending_ = false;
    if (!restore_grab_) return true;
    if (focused && !invoke(grab)) return false;
    restore_grab_ = false;
    return true;
}

} // namespace medicon::platform
