#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace medicon::sdk {

struct Vec2 { float x = 0, y = 0; };
struct Vec3 { float x = 0, y = 0, z = 0; };
struct Bounds { Vec3 min, max; };
struct BlockPos { int x = 0, y = 0, z = 0; };

// Values are copied on the game thread. id is an opaque, frame-local handle;
// rendering must never dereference it or retain it across world changes.
struct ActorSnapshot {
    std::uintptr_t id = 0;
    Vec3 position, velocity;
    Bounds bounds;
    bool player = false, alive = false, local = false, visible = false;
};

struct GameplayFrame {
    bool ready = false;
    ActorSnapshot local;
    std::vector<ActorSnapshot> actors;
    Vec3 eye;
    Vec3 camera;
    float yaw = 0, pitch = 0;
    bool attack_down = false;
    std::array<float, 16> view_matrix{};
    Vec2 fov;
    bool projection_ready = false;
};

// Uses the copied Bedrock view rotation, camera origin and perspective scales.
[[nodiscard]] bool world_to_screen(const GameplayFrame& frame, Vec3 world,
                                    Vec2 viewport, Vec2& screen) noexcept;

class GameplayAccess {
public:
    virtual ~GameplayAccess() = default;
    [[nodiscard]] virtual const GameplayFrame& frame() const noexcept = 0;
    virtual bool rotate(float yaw, float pitch) = 0;
    virtual bool attack(std::uintptr_t actor_id) = 0;
    virtual bool place_block(BlockPos position, bool auto_select) = 0;
    [[nodiscard]] virtual bool replaceable(BlockPos position) const = 0;
};

} // namespace medicon::sdk
