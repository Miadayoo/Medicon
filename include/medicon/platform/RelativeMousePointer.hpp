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
// sampled between packets cannot overwrite accumulated motion. Bounds may be
// the desktop in client coordinates, allowing the pointer to leave the window.
// The caller resets on focus/overlay/mode/window geometry changes.
class RelativeMousePointer {
public:
    void reset() noexcept { active_ = false; x_ = y_ = 0; }
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] int x() const noexcept { return x_; }
    [[nodiscard]] int y() const noexcept { return y_; }

    // Returns whether a valid viewport is available, including zero movement.
    [[nodiscard]] bool move(int dx, int dy, int width, int height, int seedX, int seedY) noexcept {
        return move_within(dx, dy, 0, 0, width, height, seedX, seedY);
    }

    // Exclusive right/bottom bounds; desktop coordinates can extend past every
    // edge of the client, including negative positions on adjacent monitors.
    [[nodiscard]] bool move_within(int dx, int dy, int left, int top, int right, int bottom,
                                   int seedX, int seedY) noexcept {
        if (right <= left || bottom <= top) { reset(); return false; }
        if (!active_) {
            const bool seed_inside = seedX >= left && seedX < right && seedY >= top && seedY < bottom;
            x_ = seed_inside ? seedX : static_cast<int>((static_cast<std::int64_t>(right) - left) / 2 + left);
            y_ = seed_inside ? seedY : static_cast<int>((static_cast<std::int64_t>(bottom) - top) / 2 + top);
            active_ = true;
        }
        // Clamp before movement as well: a smaller viewport must not retain
        // invisible overshoot that consumes subsequent motion back inwards.
        x_ = std::clamp(x_, left, right - 1);
        y_ = std::clamp(y_, top, bottom - 1);
        x_ = static_cast<int>(std::clamp<std::int64_t>(static_cast<std::int64_t>(x_) + dx, left, right - 1));
        y_ = static_cast<int>(std::clamp<std::int64_t>(static_cast<std::int64_t>(y_) + dy, top, bottom - 1));
        return true;
    }

private:
    bool active_ = false;
    int x_ = 0;
    int y_ = 0;
};

} // namespace medicon::platform
