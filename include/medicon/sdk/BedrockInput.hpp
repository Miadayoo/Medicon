#pragma once

#include <medicon/memory/module_view.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace medicon::sdk::input {

// Windows x64 ABI verified against the 1.26.52.3 executable. Opaque game
// objects are forwarded only; no game input/component layout is accessed.
struct LookDelta { float x, y; };
using MouseFeedFn = void(*)(void*, std::uint8_t, int, std::int16_t, std::int16_t,
                           std::int16_t, std::int16_t, std::uint8_t);
using LookInputFn = void(*)(void*, const void*, void*, const LookDelta*);

inline constexpr std::string_view mouse_feed_pattern =
    "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 48 44 89 CF 44 89 C3 89 D5 48 89 CE "
    "44 0F B7 A4 24 C0 00 00 00 44 0F B7 AC 24 B8 00 00 00 44 0F B7 BC 24 B0 00 00 00";
inline constexpr std::string_view look_input_pattern =
    "56 48 83 EC 30 0F 29 74 24 20 4C 89 C6 F3 0F 10 05 ? ? ? ? "
    "F3 41 0F 10 49 04 F3 0F 59 C8 F3 41 0F 10 50 08 F3 0F 5C D1";

struct Targets {
    std::uintptr_t mouse_feed{}, look_input{};
    explicit operator bool() const noexcept { return mouse_feed && look_input; }
};

Targets resolve_targets(std::uint32_t timestamp, std::uint32_t image_size,
                        std::span<const memory::MemoryRegion> regions, memory::AddressRange image,
                        std::string& diagnostic);
Targets locate_targets(std::string& diagnostic);

// Block at MouseDevice::feed BEFORE it appends/accumulates native events.
// Merely swallowing Win32 messages does not stop other native input producers.
inline void forward_mouse_feed(bool captured, MouseFeedFn original, void* device, std::uint8_t button,
                               int data, std::int16_t x, std::int16_t y, std::int16_t dx,
                               std::int16_t dy, std::uint8_t motionless) {
    if (!captured) original(device, button, data, x, y, dx, dy, motionless);
}

// Also neutralize deltas already queued when the GUI opened. Call the original
// with a temporary zero vector; never modify its borrowed input or camera.
inline void forward_look_input(bool captured, LookInputFn original, void* entity, const void* camera,
                               void* direct_look, const LookDelta* delta) {
    const LookDelta stationary{};
    original(entity, camera, direct_look, captured ? &stationary : delta);
}

} // namespace medicon::sdk::input
