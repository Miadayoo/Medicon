#pragma once

#include <medicon/sdk/Gameplay.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace medicon::sdk {

struct RotationDiagnostic {
    float requested_pitch = 0;
    float previous_pitch = 0;
    float view_pitch = 0;
    float view_previous_pitch = 0;
    float render_pitch = 0;
    float render_yaw = 0;
    std::uintptr_t render_component = 0;
    std::uintptr_t rotation_component = 0;
};

enum class SprintInputRewrite { ignored, rewritten, layout_rejected };
enum class RotationInputRewrite { ignored, rewritten, layout_rejected };
struct MovementReport {
    Vec3 position, delta;
    std::uint64_t input_tick{}, flags[2]{};
};
struct LocalPlayerMove {
    Vec3 position;
    std::uint8_t mode{};
    bool on_ground{};
    std::uint64_t runtime_id{};
};
enum class VelocityTracePhase { apply_begin, apply_end, latency_submit };
struct NativeVelocityTrace {
    VelocityTracePhase phase{};
    KnockbackApplyResult result{};
    Vec3 position, velocity;
    std::uint64_t capture_serial{}, latency_id{};
    const char* path = "unknown";
    bool state_matches_cached{};
};
using VelocityTraceObserver = void(*)(void*, const NativeVelocityTrace&) noexcept;

// Native calls and ECS reads are confined to the local-player baseTick thread.
// Call capture() with the client/player validated by BedrockChat, dispatch
// gameplay modules synchronously, then end_tick(). Present consumes copies.
class BedrockGameplay final : public GameplayAccess {
public:
    BedrockGameplay();
    ~BedrockGameplay() override;
    BedrockGameplay(const BedrockGameplay&) = delete;
    BedrockGameplay& operator=(const BedrockGameplay&) = delete;

    bool locate(std::string& diagnostic);
    [[nodiscard]] const std::string& diagnostic() const noexcept;
    bool capture(void* client, void* local_actor, bool check_visibility = true);
    void end_tick() noexcept;
    // Present-thread safe: rereads only the camera fields and the local render
    // position located by the last capture (plain copies, no native call) into
    // `frame`, between game ticks. Leaves the camera unchanged and returns false
    // when it fails validation; a render position that fails is dropped.
    bool refresh_view(GameplayFrame& frame) const noexcept;
    [[nodiscard]] std::uintptr_t body_yaw_interpolator_target() const noexcept;
    [[nodiscard]] std::uintptr_t head_yaw_interpolator_target() const noexcept;
    [[nodiscard]] bool validates_body_yaw_interpolator(void* actor) const noexcept;
    [[nodiscard]] bool validates_head_yaw_interpolator(void* actor) const noexcept;
    [[nodiscard]] std::string rotation_getter_diagnostic(void* actor) const;
    [[nodiscard]] bool rotation_override(float& previous_yaw, float& yaw) const noexcept;
    [[nodiscard]] bool rotation_diagnostic(RotationDiagnostic& diagnostic) const noexcept;
    // Auto Select still holds a block slot that a later capture puts back.
    [[nodiscard]] bool slot_restore_pending() const noexcept;
    [[nodiscard]] std::uintptr_t motion_dispatcher_target() const noexcept;
    [[nodiscard]] std::uintptr_t prediction_correction_dispatcher_target() const noexcept;
    [[nodiscard]] std::uintptr_t disconnect_dispatcher_target() const noexcept;
    [[nodiscard]] std::uintptr_t move_player_dispatcher_target() const noexcept;
    [[nodiscard]] bool is_client_network_handler(void* handler) const noexcept;
    [[nodiscard]] bool read_movement_report(void* packet, MovementReport& report) const noexcept;
    // Packet-only reader is safe without a frame lock; copies no native ownership.
    [[nodiscard]] bool read_player_move(void* packet_reference, LocalPlayerMove& move) const noexcept;
    // Caller serializes against capture/clear before checking current identity.
    [[nodiscard]] bool is_local_runtime_id(std::uint64_t runtime_id) const noexcept;
    [[nodiscard]] bool local_player_move(void* packet_reference, LocalPlayerMove& move) const noexcept;
    // Bind before enabling callbacks. Observations contain copied scalar values.
    void bind_velocity_trace(VelocityTraceObserver observer, void* context) noexcept;
    // The dispatcher borrows a native shared_ptr by reference; ownership stays in the game.
    // Resolve the packet and match its runtime ID against the current local actor.
    [[nodiscard]] bool local_motion(void* packet_reference, void* local_actor, Vec3& motion,
                                    std::uint64_t* input_tick = nullptr, std::int64_t* received_timestamp = nullptr) const noexcept;
    bool write_local_motion(void* packet_reference, void* local_actor, Vec3 motion) const noexcept;
    // Bind the original sendToServer (slot 4), not the general/chat send.
    void bind_latency_sender(std::uintptr_t target, void(*original)(void*, void*)) noexcept;
    [[nodiscard]] bool is_latency_reply(void* packet) const noexcept;
    [[nodiscard]] bool read_latency_reply(void* sender, void* packet, LatencyReply& reply) const noexcept;
    // Replay the server's timestamp/acknowledgment value unchanged.
    bool send_latency_reply(const LatencyReply& reply) override;
    void clear() noexcept;
    [[nodiscard]] const GameplayFrame& frame() const noexcept override;
    void begin_rotation_control(bool enabled) noexcept override;
    void begin_rotation_control(bool enabled, bool render_enabled) noexcept override;
    void finish_rotation_control() noexcept override;
    void finish_rotation_control(bool rotation_requested) noexcept override;
    bool rotate(float yaw, float pitch) override;
    bool rotate_head_and_body(float yaw) override;
    bool rotate_head_and_body(float yaw, float pitch) override;
    bool attack(std::uintptr_t actor_id) override;
    bool set_sprinting(bool sprinting) override;
    bool set_velocity(Vec3 velocity) override;
    KnockbackApplyResult apply_knockback(Vec3 velocity, std::int64_t received_timestamp = 0) override;
    // Updates the sprint state bits in a validated PlayerAuthInputPacket before
    // it is sent. Non-matching packets and unknown input layouts are untouched.
    SprintInputRewrite rewrite_sprint_input(void* packet, bool override_enabled, bool sprinting) noexcept;
    // Send-thread safe: uses a copied angle pair, independent of render settings.
    RotationInputRewrite rewrite_rotation_input(void* packet) noexcept;
    bool swing() override;
    bool place_block(BlockPos position, bool auto_select) override;
    [[nodiscard]] bool replaceable(BlockPos position) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace medicon::sdk
