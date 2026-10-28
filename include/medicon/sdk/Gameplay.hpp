#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace medicon::sdk {

struct Vec2 { float x = 0, y = 0; };
struct Vec3 { float x = 0, y = 0, z = 0; };
struct Bounds { Vec3 min, max; };
struct BlockPos { int x = 0, y = 0, z = 0; };

[[nodiscard]] inline float interpolate_yaw(float previous, float current, float alpha) noexcept {
    if (!std::isfinite(previous) || !std::isfinite(current) || !std::isfinite(alpha)) return current;
    alpha = std::clamp(alpha, 0.0F, 1.0F);
    return std::remainder(previous + std::remainder(current - previous, 360.0F) * alpha, 360.0F);
}

// Values are copied on the game thread. id is an opaque, frame-local handle;
// rendering must never dereference it or retain it across world changes.
struct ActorSnapshot {
    std::uintptr_t id = 0;
    Vec3 position, velocity;
    Bounds bounds;
    bool player = false, alive = false, local = false, visible = false;
    // One game tick earlier. Drawing between the two, as the game itself does,
    // keeps overlays on the model at frame rate instead of stepping 20 times a second.
    Vec3 previous_position;
    bool has_previous = false;
    // The local player only: where the game draws position this frame
    // (RenderPositionComponent), reread every Present.
    Vec3 render_position;
    bool has_render_position = false;
};

struct GameplayFrame {
    bool ready = false;
    ActorSnapshot local;
    std::vector<ActorSnapshot> actors;
    Vec3 eye;
    Vec3 camera;
    float yaw = 0, pitch = 0;
    float head_yaw = 0;
    bool head_yaw_ready = false;
    bool attack_down = false;
    bool sprinting = false;
    std::array<float, 16> view_matrix{};
    Vec2 fov;
    bool projection_ready = false;
};

// Uses the copied Bedrock view rotation, camera origin and perspective scales.
[[nodiscard]] bool world_to_screen(const GameplayFrame& frame, Vec3 world,
                                    Vec2 viewport, Vec2& screen) noexcept;
// Where the camera actually is: the render origin moved by the view matrix's
// own translation (the third-person offset, when the matrix carries it).
[[nodiscard]] Vec3 camera_position(const GameplayFrame& frame) noexcept;

class GameplayAccess {
public:
    virtual ~GameplayAccess() = default;
    [[nodiscard]] virtual const GameplayFrame& frame() const noexcept = 0;
    virtual void begin_rotation_control(bool enabled) noexcept {
        (void)enabled;
    }
    virtual void begin_rotation_control(bool enabled, bool render_enabled) noexcept {
        (void)render_enabled;
        begin_rotation_control(enabled);
    }
    virtual void finish_rotation_control() noexcept {}
    // Applies yaw to head/body and retains pitch for third-person rendering;
    // the local camera and first-person arm pose stay at the user's view angle.
    virtual bool rotate(float yaw, float pitch) = 0;
    // Changes local head/body yaw without changing camera yaw or movement input.
    virtual bool rotate_head_and_body(float yaw) = 0;
    // Optional pitch support for aim modules. The default keeps older game
    // adapters source-compatible; native adapters can apply vertical aim.
    virtual bool rotate_head_and_body(float yaw, float pitch) {
        (void)pitch;
        return rotate_head_and_body(yaw);
    }
    virtual bool attack(std::uintptr_t actor_id) = 0;
    // Sets or clears the local player's sprinting actor-data flag.
    virtual bool set_sprinting(bool sprinting) { (void)sprinting; return false; }
    // Starts the local player's native Attack swing, including the game's
    // normal animation notification path when the host supports it.
    virtual bool swing() { return false; }
    virtual bool place_block(BlockPos position, bool auto_select) = 0;
    [[nodiscard]] virtual bool replaceable(BlockPos position) const = 0;
};

} // namespace medicon::sdk
