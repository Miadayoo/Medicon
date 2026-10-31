#pragma once

#include <medicon/sdk/Gameplay.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace medicon::sdk { struct MovementReport; struct LocalPlayerMove; }

namespace medicon::sdk::detail {

// Read-only release-x64 EnTT ABI readers. These never call, mutate, or destroy
// foreign C++ containers. Kept separate so malformed layouts can be tested.
struct GameplayPool { std::uint32_t hash; std::uintptr_t storage; };
bool read_gameplay_pools(std::uintptr_t registry, std::vector<GameplayPool>& output) noexcept;
std::uintptr_t gameplay_component(std::uintptr_t storage, std::uint32_t entity,
                                  std::size_t element_size) noexcept;
bool gameplay_contains(std::uintptr_t storage, std::uint32_t entity) noexcept;

inline constexpr std::size_t move_input_size = 0x64;
inline constexpr std::size_t actor_data_flags_size = 16;
struct GameplayMoveInput { float strafe{}, forward{}; bool sneaking{}; };
bool read_gameplay_move_input(std::uintptr_t component, GameplayMoveInput& input) noexcept;
bool write_gameplay_sprint_input(std::uintptr_t component, bool sprinting) noexcept;
// Validated 1.26.52.3 movement packet layouts; touches only pitch/yaw/head yaw
// (and PlayerAuthInput's interact rotation).
bool write_gameplay_rotation_input(std::uintptr_t packet, int packet_id, float yaw, float pitch) noexcept;
bool read_gameplay_motion(std::uintptr_t packet, std::uint64_t local_runtime_id, Vec3& motion) noexcept;
bool write_gameplay_motion(std::uintptr_t packet, Vec3 motion) noexcept;
// StateVectorComponent is position / previous position / velocity (3 x Vec3).
bool write_gameplay_velocity(std::uintptr_t component, Vec3 velocity) noexcept;
// Copy the timestamp/acknowledgment as an opaque value, without unit conversion.
bool copy_gameplay_latency(std::uintptr_t packet, LatencyReply& reply) noexcept;
bool read_gameplay_movement_report(std::uintptr_t packet, MovementReport& report) noexcept;
bool read_gameplay_local_move(std::uintptr_t packet, std::uint64_t runtime_id, LocalPlayerMove& move) noexcept;
using EnqueuePredictedMotion = void(*)(void*, const void*, const std::int64_t*);
using UpdatePredictedMotion = void(*)(void*);
// Caller validates current component/controller and both native entry points.
// Native code copies the vector and timestamp before these borrowed arguments expire.
bool enqueue_gameplay_knockback(void* component, void* controller, Vec3 velocity, std::int64_t timestamp,
                                EnqueuePredictedMotion enqueue, UpdatePredictedMotion update);

// A model-render callback borrows the render pitch only for its duration.
// Camera RotationComponent and adjacent render yaw are never changed.
class RenderPitchScope {
public:
    RenderPitchScope(std::uintptr_t render_component, float pitch) noexcept;
    ~RenderPitchScope();
    RenderPitchScope(const RenderPitchScope&) = delete;
    RenderPitchScope& operator=(const RenderPitchScope&) = delete;
    [[nodiscard]] bool applied() const noexcept { return component_ != 0; }
private:
    std::uintptr_t component_{};
    float original_{};
};

} // namespace medicon::sdk::detail
