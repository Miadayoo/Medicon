#pragma once

#include <medicon/sdk/Gameplay.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace medicon::sdk::detail {

// Physical 1.26.60.5 ABI at 6BD63AB..6BD643D. The game creates the event,
// owns both allocations, and consumes the two-word owner in admit(). No DLL
// shared_ptr or allocator crosses this boundary.
struct ReplayMotionOwner { std::uintptr_t object{}, control{}; };
struct ReplayMotionCalls {
    void* context{};
    void* (*allocate)(void*, std::size_t){};
    void* (*create)(std::uintptr_t*, const Vec3*){};
    void (*release)(ReplayMotionOwner*){};
    int (*admit)(void*, void*, ReplayMotionOwner*, std::uint64_t){};
    std::uintptr_t control_table{};
};

inline bool enqueue_replay_motion(const ReplayMotionCalls& calls, void* component,
                                  void* actor, Vec3 velocity, std::uint64_t tick) {
    if (!component || !actor || !tick || tick == UINT64_MAX || !calls.allocate ||
        !calls.create || !calls.release || !calls.admit || !calls.control_table) return false;
    auto* control = calls.allocate(calls.context, 24);
    if (!control) return false;
    const std::array<std::uint64_t, 3> initial{calls.control_table, 0x100000001ULL, 0};
    std::memcpy(control, initial.data(), sizeof(initial));
    ReplayMotionOwner owner{0, reinterpret_cast<std::uintptr_t>(control)};
    struct Cleanup {
        const ReplayMotionCalls& calls;
        ReplayMotionOwner& owner;
        bool transferred{};
        ~Cleanup() { if (!transferred) calls.release(&owner); }
    } cleanup{calls, owner};
    calls.create(&owner.object, &velocity);
    std::memcpy(static_cast<std::byte*>(control) + 16, &owner.object, sizeof(owner.object));
    if (!owner.object) return false;
    // admit consumes the owner on every return, including its no-op result.
    // It does not clear all input fields, so caller cleanup would double-free.
    cleanup.transferred = true;
    const auto result = calls.admit(component, actor, &owner, tick);
    return result >= 0 && result <= 2;
}

} // namespace medicon::sdk::detail
