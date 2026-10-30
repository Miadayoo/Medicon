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
    bool prepare_sprinting(bool sprinting) noexcept;
    // Updates the sprint state bits in a validated PlayerAuthInputPacket before
    // it is sent. Non-matching packets and unknown input layouts are untouched.
    SprintInputRewrite rewrite_sprint_input(void* packet, bool override_enabled, bool sprinting) noexcept;
    // Send-thread safe: uses a copied angle pair, independent of render settings.
    RotationInputRewrite rewrite_rotation_input(void* packet, bool vector_fix = false) noexcept;
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
    [[nodiscard]] bool should_switch_before_destroy(float destroy_time_scale) const override;
    bool prepare_durability_safe_finish() override;
    void stop_destroy_block() noexcept override;
    [[nodiscard]] int selected_hotbar_slot() const noexcept override;
    [[nodiscard]] bool hotbar_item(int slot, BlockPos target, HotbarItemInfo& info) const override;
    bool select_hotbar_slot(int slot) override;

private:
    struct Impl;
    bool swing_with_source(std::uint8_t source);
    std::unique_ptr<Impl> impl_;
};

} // namespace medicon::sdk
