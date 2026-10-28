#pragma once

#include <medicon/sdk/Gameplay.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace medicon::sdk {

// Auto Select's switched slot, decided by every capture. The player no longer
// holding it (another slot, player or world) means nothing to undo. While
// Scaffold runs, the slot comes back after a second without a placement from
// it; once Scaffold is off it comes back at once.
enum class SlotRestore { forget, wait, restore };
[[nodiscard]] constexpr SlotRestore slot_restore(bool still_held, bool auto_select_active,
                                                 std::uint64_t idle_ms) noexcept {
    if (!still_held) return SlotRestore::forget;
    return auto_select_active && idle_ms < 1000 ? SlotRestore::wait : SlotRestore::restore;
}

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
    // Auto Select still holds a block slot that a later capture puts back.
    [[nodiscard]] bool slot_restore_pending() const noexcept;
    // Game thread, before capture(): false once Scaffold is off, so the next
    // capture puts a switched slot back without the one-second wait.
    void set_auto_select_active(bool active) noexcept;
    void clear() noexcept;
    [[nodiscard]] const GameplayFrame& frame() const noexcept override;
    bool rotate(float yaw, float pitch) override;
    bool attack(std::uintptr_t actor_id) override;
    bool place_block(BlockPos position, bool auto_select) override;
    [[nodiscard]] bool replaceable(BlockPos position) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace medicon::sdk
