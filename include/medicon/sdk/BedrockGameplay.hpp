#pragma once

#include <medicon/sdk/Gameplay.hpp>

#include <memory>
#include <string>

namespace medicon::sdk {

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
