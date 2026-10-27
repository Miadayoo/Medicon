#pragma once

#include <algorithm>
#include <cstdint>

namespace medicon::platform {

// RIDEV_NOLEGACY (and hosts that never register a mouse) do not move the OS
// cursor. Integrate WM_INPUT deltas locally and warp the hardware cursor from
// that accumulator so the pointer is not locked to DXGI Present.
[[nodiscard]] inline bool relative_pointer_owns_motion(
    bool overlay_open, bool focused, bool no_legacy, bool absolute_move) noexcept {
    return overlay_open && focused && no_legacy && !absolute_move;
}

// A local UI cursor for relative RAWMOUSE streams. Once activated, an OS cursor
// outside the client (or a stationary OS cursor under RIDEV_NOLEGACY) cannot
// overwrite accumulated motion. The caller resets on focus/overlay/mode changes.
class RelativeMousePointer {
public:
    void reset() noexcept { active_ = false; x_ = y_ = 0; }
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] int x() const noexcept { return x_; }
    [[nodiscard]] int y() const noexcept { return y_; }

    // Returns whether a valid viewport is available, including zero movement.
    [[nodiscard]] bool move(int dx, int dy, int width, int height, int seedX, int seedY) noexcept {
        if (width <= 0 || height <= 0) { reset(); return false; }
        if (!active_) {
            const bool seed_inside = seedX >= 0 && seedX < width && seedY >= 0 && seedY < height;
            x_ = seed_inside ? seedX : width / 2;
            y_ = seed_inside ? seedY : height / 2;
            active_ = true;
        }
        // Clamp before movement as well: a smaller viewport must not retain
        // invisible overshoot that consumes subsequent motion back inwards.
        x_ = std::clamp(x_, 0, width - 1);
        y_ = std::clamp(y_, 0, height - 1);
        x_ = static_cast<int>(std::clamp<std::int64_t>(static_cast<std::int64_t>(x_) + dx, 0, width - 1));
        y_ = static_cast<int>(std::clamp<std::int64_t>(static_cast<std::int64_t>(y_) + dy, 0, height - 1));
        return true;
    }

private:
    bool active_ = false;
    int x_ = 0;
    int y_ = 0;
};

} // namespace medicon::platform
