#pragma once

#include <medicon/sdk/Gameplay.hpp>

#include <array>
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
enum class MoveFixInputRewrite { ignored, rewritten, layout_rejected };
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

struct RotationMovementSample {
    bool recognized = false;
    bool valid = false;
    std::uint8_t validation_stage = 0;
    std::int32_t packet_id = -1;
    std::uint64_t client_tick = 0;
    Vec2 rotation{};
    Vec3 position{};
    Vec2 analog_move{};
    Vec2 move{};
    Vec2 raw_move{};
};

struct OutgoingBlockAction {
    std::int32_t type = -1;
    BlockPos position{};
    std::int32_t face = -1;
};

struct OutgoingBlockActionSample {
    bool recognized = false;
    bool valid = false;
    std::uint8_t validation_stage = 0;
    std::uintptr_t vtable_rva = 0;
    std::uint64_t client_tick = 0;
    bool perform_block_actions = false;
    std::uint32_t count = 0;
    std::array<OutgoingBlockAction, 8> actions{};
};

struct DestroyBlockDiagnostic {
    std::uint8_t stage = 0;
    std::uint8_t profile_stage = 0;
    bool continue_return = false;
    bool native_destroyed = false;
    bool active = false;
    bool block_readable = false;
    std::uint16_t block_id = 0;
    float progress = 0.0F;
    BlockPos position{};
    std::uint8_t face = 0;
    std::uintptr_t requested_mode = 0;
    std::uintptr_t requested_owner = 0;
    std::uintptr_t requested_vtable_rva = 0;
    bool requested_owner_matches = false;
    BlockPos requested_position{};
    std::uint8_t requested_face = 0;
};

struct DestroyModeDiagnostic {
    bool readable = false;
    bool local_mode = false;
    std::uintptr_t owner = 0;
    std::uintptr_t vtable_rva = 0;
    BlockPos position{};
    std::uint8_t face = 0;
    float previous_progress = 0.0F;
    float accumulated_progress = 0.0F;
};

struct InventoryDiagnostic {
    const char* stage = "not-attempted";
    std::uint64_t attempts{}, sent{};
    std::uintptr_t container_vtable_rva{}, getter_rva{}, setter_rva{};
    std::uintptr_t add_action_rva{}, transaction_vtable_rva{};
};

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
    [[nodiscard]] InventoryDiagnostic inventory_diagnostic() const noexcept;
    bool capture(void* client, void* local_actor, bool check_visibility = true);
    void end_tick() noexcept;
    // Present-thread safe: rereads only the camera fields and the local render
    // position located by the last capture (plain copies, no native call) into
    // `frame`, between game ticks. Leaves the camera unchanged and returns false
    // when it fails validation; a render position that fails is dropped.
    bool refresh_view(GameplayFrame& frame) const noexcept;
    [[nodiscard]] std::uintptr_t body_yaw_interpolator_target() const noexcept;
    [[nodiscard]] std::uintptr_t head_yaw_interpolator_target() const noexcept;
    [[nodiscard]] std::uintptr_t destroy_progress_target() const noexcept;
    [[nodiscard]] std::uintptr_t destroy_stop_target() const noexcept;
    [[nodiscard]] float scale_destroy_progress(std::uintptr_t game_mode, float progress) const noexcept;
    [[nodiscard]] bool should_defer_destroy_stop(std::uintptr_t game_mode, const BlockPos* position) const noexcept;
    [[nodiscard]] bool read_outgoing_block_actions(void* packet, OutgoingBlockActionSample& sample) const noexcept;
    [[nodiscard]] DestroyBlockDiagnostic destroy_block_diagnostic() const noexcept;
    [[nodiscard]] DestroyModeDiagnostic destroy_mode_diagnostic(std::uintptr_t game_mode) const noexcept;
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
    KnockbackApplyResult apply_knockback(Vec3 velocity, std::int64_t received_timestamp = 0,
                                        std::uint64_t input_tick = 0) override;
    bool prepare_sprinting(bool sprinting) noexcept;
    // Updates the sprint state bits in a validated PlayerAuthInputPacket before
    // it is sent. Non-matching packets and unknown input layouts are untouched.
    SprintInputRewrite rewrite_sprint_input(void* packet, bool override_enabled, bool sprinting) noexcept;
    // Send-thread safe: uses a copied angle pair, independent of render settings.
    RotationInputRewrite rewrite_rotation_input(void* packet, bool vector_fix = false) noexcept;
    // Runs after rotation and Sprint rewrites, including without a rotation request.
    MoveFixInputRewrite rewrite_move_fix_input(void* packet, bool enabled) noexcept;
    // Send-thread safe snapshot for diagnosing yaw/vector compensation.
    [[nodiscard]] bool read_rotation_movement(void* packet, RotationMovementSample& sample) const noexcept;
    bool swing() override;
    bool mining_swing() override;
    bool place_block(BlockPos position, bool auto_select) override;
    [[nodiscard]] bool replaceable(BlockPos position) const override;
    [[nodiscard]] bool block_id(BlockPos position, std::uint16_t& id) const override;
    [[nodiscard]] bool can_see_block(BlockPos position) const override;
    bool advance_destroy_block(BlockPos position, std::uint8_t face, float destroy_time_scale,
                               float& progress, bool& destroyed) override;
    [[nodiscard]] bool should_switch_before_destroy() const override;
    bool prepare_durability_safe_finish() override;
    void stop_destroy_block() noexcept override;
    [[nodiscard]] int selected_hotbar_slot() const noexcept override;
    [[nodiscard]] bool hotbar_item(int slot, BlockPos target, HotbarItemInfo& info) const override;
    bool select_hotbar_slot(int slot) override;
    [[nodiscard]] bool inventory_item(int slot, InventoryItemInfo& info) const override;
    [[nodiscard]] bool inventory_screen_open() const override;
    bool swap_inventory_slots(int first, int second) override;
    [[nodiscard]] bool armor_item(int slot, InventoryItemInfo& info) const override;
    bool equip_armor(int inventory_slot, int armor_slot) override;
    bool drop_inventory_slot(int slot) override;
    [[nodiscard]] std::uintptr_t container_screen_tick_target() const noexcept;
    // Called from the screen's own tick, which owns `controller` for the call.
    [[nodiscard]] bool container_is_chest() const noexcept;
    [[nodiscard]] bool container_slot_occupied(int slot) const noexcept;
    bool take_container_slot(void* controller, int slot) noexcept;
    bool close_container_screen(void* controller) noexcept;

private:
    struct Impl;
    bool swing_with_source(std::uint8_t source);
    std::unique_ptr<Impl> impl_;
};

class BedrockContainerScreen final : public ContainerScreen {
public:
    BedrockContainerScreen(BedrockGameplay& game, void* controller) noexcept
        : game_(game), controller_(controller) {}
    [[nodiscard]] bool chest() const override { return game_.container_is_chest(); }
    [[nodiscard]] bool slot_occupied(int slot) const override { return game_.container_slot_occupied(slot); }
    bool take(int slot) override { return game_.take_container_slot(controller_, slot); }
    bool close() override { return game_.close_container_screen(controller_); }

private:
    BedrockGameplay& game_;
    void* controller_;
};

} // namespace medicon::sdk
