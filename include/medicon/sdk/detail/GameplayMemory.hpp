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
bool write_gameplay_sprinting_flag(std::uintptr_t component, bool sprinting) noexcept;
bool write_gameplay_sprint_speed(std::uintptr_t component, bool sprinting) noexcept;
// Validated 1.26.60.5 movement packet layouts; touches pitch/yaw/head yaw,
// and optionally PlayerAuthInput's interact rotation and movement vectors.
bool write_gameplay_rotation_input(std::uintptr_t packet, int packet_id, float yaw, float pitch,
                                   bool vector_fix = false) noexcept;
// Flareon MoveFix for the validated PlayerAuthInput layout. Sprint history is
// committed only after all packet writes succeed; idle packets are unchanged.
bool write_gameplay_move_fix_input(std::uintptr_t packet, bool& sprinting) noexcept;
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
// ItemStackBase: the Item sits behind the WeakPtr at +8, count at +0x22.
// Zero for an empty stack; readable layouts only.
bool read_gameplay_stack_item(std::uintptr_t stack, std::uintptr_t& item, std::uint8_t& count) noexcept;
// 1.26.60.5 AttributesComponent: ascending attribute ids at +0 and their
// 0x88-byte AttributeInstances at +0x18. Instance max +0x78, current +0x7C.
inline constexpr std::size_t attributes_component_size = 0x50;
bool read_gameplay_attribute(std::uintptr_t component, std::uint32_t attribute_id, float& value) noexcept;

// 1.26.60.5 InventoryAction owns complete copies of both ItemStacks, including
// their tags and network identifiers. No DLL STL object crosses this ABI.
struct NativeInventorySource {
    std::int32_t type = 0;
    std::int8_t container = 0;
    std::uint32_t flags = 0;
};
struct NativeInventoryChange {
    NativeInventorySource source;
    std::uint32_t slot;
    std::uintptr_t from, to;
};
struct NativeInventoryCalls {
    void (*construct)(void*, const NativeInventorySource*, std::uint32_t, std::uintptr_t, std::uintptr_t);
    void (*destroy)(void*);
    void (*add_action)(void*, const void*, bool);
    void* (*create_transaction)(void*);
    void (*destroy_transaction)(void*, void*);
    bool (*send_transaction)(void*, void**);
};
// Solstice builds an independent Normal transaction, adds both actions to its
// data at +0x10, and sends it without the player's InventoryTransactionManager.
// Successful send transfers the transaction; the caller then updates local slots.
bool send_gameplay_inventory_changes(const std::array<NativeInventoryChange, 2>& changes,
    void* context, const NativeInventoryCalls& calls);

struct NativeInventoryPacketCalls {
    void (*construct_payload)(void*, void**, bool);
    void (*construct_packet)(void*, void*);
    void (*destroy_packet)(void*, unsigned);
    void (*send)(void*, void*);
};
// Native 1.26.60.5 constructors consume the unique_ptr and payload respectively.
// The independent packet must have mIsClientSide=false, like Solstice's factory
// packet. LocalPlayer::sendComplexInventoryTransaction instead sets it from the
// level's server-authoritative-inventory flag.
bool send_gameplay_inventory_packet(void** transaction, void* sender, const NativeInventoryPacketCalls& calls);

// An MSVC release std::string in its small-buffer form, borrowed by a native
// call that takes a const reference. Text must fit the 15-character buffer.
struct alignas(8) NativeSmallString {
    char text[16]{};
    std::uint64_t size = 0, capacity = 15;
};
inline constexpr std::size_t native_small_string_size = 32;

// A local model-render callback borrows the requested pitch only for its
// duration. Only RotationComponent pitch/current-previous fields are touched.
class RenderPitchScope {
public:
    RenderPitchScope(std::uintptr_t rotation_component, float pitch) noexcept;
    ~RenderPitchScope();
    RenderPitchScope(const RenderPitchScope&) = delete;
    RenderPitchScope& operator=(const RenderPitchScope&) = delete;
    [[nodiscard]] bool applied() const noexcept { return component_ != 0; }
private:
    std::uintptr_t component_{};
    float original_pitch_{};
    float original_previous_pitch_{};
};

} // namespace medicon::sdk::detail
