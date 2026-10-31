#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace medicon::sdk {

struct Vec2 { float x = 0, y = 0; };
struct Vec3 { float x = 0, y = 0, z = 0; };
struct Bounds { Vec3 min, max; };
struct BlockPos { int x = 0, y = 0, z = 0; };
struct HotbarItemInfo {
    bool occupied = false, pickaxe = false, damageable = false;
    std::int16_t item_id = 0;
    float destroy_speed = 0.0F;
};

// Copied 1.26.52.3 NetworkStackLatency packet. The inspected class contains
// scalar data and static native vtable/dispatcher pointers, with no owned heap
// fields. Native adapters validate identity before borrowing this copy to send.
struct alignas(8) LatencyReply { std::array<std::byte, 0x48> bytes{}; };
enum class KnockbackApplyResult { failed, direct, predicted };

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
    std::int64_t unique_id{};
    std::string model_name;
};

struct GameplayFrame {
    bool ready = false;
    std::uint64_t world_identity{};
    ActorSnapshot local;
    std::vector<ActorSnapshot> actors;
    Vec3 eye;
    Vec3 camera;
    float yaw = 0, pitch = 0;
    float head_yaw = 0;
    bool head_yaw_ready = false;
    bool attack_down = false;
    bool sprinting = false;
    // View-relative native movement input (x: strafe, y: forward).
    Vec2 move_input;
    bool move_input_ready = false;
    bool sneaking = false;
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
    // Only an actual feature request should override outgoing movement angles.
    // Restoring the view (including Rotations Always) leaves native packets alone.
    virtual void finish_rotation_control(bool rotation_requested) noexcept {
        (void)rotation_requested;
        finish_rotation_control();
    }
    // Applies yaw to head/body and retains yaw/pitch for server updates and third-person rendering;
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
    // Requests a one-shot render snap while retaining the same gameplay
    // rotation and packet behavior as rotate_head_and_body(yaw, pitch).
    virtual bool rotate_head_and_body_instant(float yaw, float pitch) {
        return rotate_head_and_body(yaw, pitch);
    }
    virtual bool attack(std::uintptr_t actor_id) = 0;
    // Requests sprint through native movement input so the game applies its
    // normal sprint transition, movement speed and eligibility checks.
    virtual bool set_sprinting(bool sprinting) { (void)sprinting; return false; }
    // Write velocity directly on the local game thread.
    virtual bool set_velocity(Vec3 velocity) { (void)velocity; return false; }
    // Admit a fresh impulse through the host's movement prediction, when present.
    // received_timestamp is the copied native arrival clock, advanced by time held.
    virtual KnockbackApplyResult apply_knockback(Vec3 velocity, std::int64_t received_timestamp = 0) {
        (void)received_timestamp;
        return set_velocity(velocity) ? KnockbackApplyResult::direct : KnockbackApplyResult::failed;
    }
    virtual bool send_latency_reply(const LatencyReply& reply) { (void)reply; return false; }
    // Starts the local player's native Attack swing, including the game's
    // normal animation notification path when the host supports it.
    virtual bool swing() { return false; }
    // Requests the native Mine-source swing, separate from combat Attack.
    virtual bool mining_swing() { return false; }
    virtual bool place_block(BlockPos position, bool auto_select) = 0;
    [[nodiscard]] virtual bool replaceable(BlockPos position) const = 0;
    [[nodiscard]] virtual bool block_id(BlockPos position, std::uint16_t& id) const {
        (void)position; (void)id; return false;
    }
    [[nodiscard]] virtual bool can_see_block(BlockPos position) const {
        (void)position; return false;
    }
    virtual bool advance_destroy_block(BlockPos position, std::uint8_t face, float destroy_time_scale,
                                       float& progress, bool& destroyed) {
        (void)position; (void)face; (void)destroy_time_scale; (void)progress; (void)destroyed; return false;
    }
    [[nodiscard]] virtual bool should_switch_before_destroy(float destroy_time_scale) const {
        (void)destroy_time_scale; return false;
    }
    virtual bool prepare_durability_safe_finish() { return false; }
    virtual void stop_destroy_block() noexcept {}
    [[nodiscard]] virtual int selected_hotbar_slot() const noexcept { return -1; }
    [[nodiscard]] virtual bool hotbar_item(int slot, BlockPos target, HotbarItemInfo& info) const {
        (void)slot; (void)target; (void)info; return false;
    }
    virtual bool select_hotbar_slot(int slot) { (void)slot; return false; }
};

} // namespace medicon::sdk
