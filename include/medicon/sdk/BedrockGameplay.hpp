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
    void clear() noexcept;
    [[nodiscard]] const GameplayFrame& frame() const noexcept override;
    void begin_rotation_control(bool enabled) noexcept override;
    void begin_rotation_control(bool enabled, bool render_enabled) noexcept override;
    void finish_rotation_control() noexcept override;
    bool rotate(float yaw, float pitch) override;
    bool rotate_head_and_body(float yaw) override;
    bool rotate_head_and_body(float yaw, float pitch) override;
    bool attack(std::uintptr_t actor_id) override;
    bool set_sprinting(bool sprinting) override;
    // Updates the sprint state bits in a validated PlayerAuthInputPacket before
    // it is sent. Non-matching packets and unknown input layouts are untouched.
    SprintInputRewrite rewrite_sprint_input(void* packet, bool override_enabled, bool sprinting) noexcept;
    bool swing() override;
    bool place_block(BlockPos position, bool auto_select) override;
    [[nodiscard]] bool replaceable(BlockPos position) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace medicon::sdk
