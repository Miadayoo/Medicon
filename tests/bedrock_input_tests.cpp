#include <medicon/sdk/BedrockInput.hpp>
#include <medicon/hooks/HookManager.hpp>
#include <medicon/memory/pattern.hpp>

#include <Windows.h>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
namespace input = medicon::sdk::input;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<std::byte> materialize(std::string_view text) {
    const auto pattern = medicon::memory::Pattern::parse(text);
    require(pattern.has_value(), "Valid native input signature");
    std::vector<std::byte> bytes;
    for (const auto byte : pattern->bytes()) bytes.push_back(std::byte{byte.wildcard ? std::uint8_t{0x12} : byte.value});
    return bytes;
}

void resolution_test() {
    auto feed = materialize(input::mouse_feed_pattern);
    auto look = materialize(input::look_input_pattern);
    constexpr std::uintptr_t base = 0x140000000;
    std::array regions{medicon::memory::MemoryRegion{feed, base + 0x427650},
                       medicon::memory::MemoryRegion{look, base + 0x6EEBA30}};
    std::string diagnostic;
    for (const auto size : {0x1383B000U, 0x1383C000U}) {
        const auto targets = input::resolve_targets(0x6AD79EE4, size, regions, {base, size}, diagnostic);
        require(targets && targets.mouse_feed == regions[0].address && targets.look_input == regions[1].address,
                "Both inspected native input entries resolve for 1.26.60.5 packaging");
    }
    require(!input::resolve_targets(0, 0x1383B000, regions, {base, 0x1383B000}, diagnostic), "Reject another build");
    require(!input::resolve_targets(0x6AB54E37, 0x12C02000, regions, {base, 0x12C02000}, diagnostic),
            "Never mix the 1.26.52 input profile with 1.26.60 signatures");
    require(!input::resolve_targets(0x6AD79EE4, 0x1383D000, regions, {base, 0x1383D000}, diagnostic), "Reject uninspected packaging");
    require(!input::resolve_targets(0x6AD79EE4, 0x1383B000, regions, {base, 0x400000}, diagnostic), "No partial hook set");
    for (auto* bytes : {&feed, &look}) {
        const auto first = (*bytes)[0];
        (*bytes)[0] = std::byte{0xCC};
        require(!input::resolve_targets(0x6AD79EE4, 0x1383B000, regions, {base, 0x1383B000}, diagnostic),
                "A missing entry withholds both hooks");
        (*bytes)[0] = first;
    }
    auto duplicate = feed;
    const std::array ambiguous{regions[0], regions[1], medicon::memory::MemoryRegion{duplicate, base + 0x400000}};
    require(!input::resolve_targets(0x6AD79EE4, 0x1383B000, ambiguous, {base, 0x1383B000}, diagnostic),
            "Duplicate native function matches are rejected");
    regions[0].address += 16;
    require(!input::resolve_targets(0x6AD79EE4, 0x1383B000, regions, {base, 0x1383B000}, diagnostic),
            "A matching byte sequence away from the inspected entry is rejected");
}

struct FeedRecord {
    unsigned calls{};
    void* device{};
    std::uint8_t button{}, motionless{};
    int data{};
    std::int16_t x{}, y{}, dx{}, dy{};
} feed_record;
struct LookRecord {
    unsigned calls{};
    void* entity{};
    const void* camera{};
    void* direct_look{};
    const input::LookDelta* pointer{};
    input::LookDelta delta{};
} look_record;
bool captured{};
input::MouseFeedFn original_feed{};
input::LookInputFn original_look{};

__declspec(noinline) void feed_fixture(void* device, std::uint8_t button, int data, std::int16_t x, std::int16_t y,
                                     std::int16_t dx, std::int16_t dy, std::uint8_t motionless) {
    ++feed_record.calls;
    feed_record.device = device;
    feed_record.button = button;
    feed_record.data = data;
    feed_record.x = x; feed_record.y = y; feed_record.dx = dx; feed_record.dy = dy;
    feed_record.motionless = motionless;
}
__declspec(noinline) void look_fixture(void* entity, const void* camera, void* direct_look, const input::LookDelta* delta) {
    ++look_record.calls;
    look_record.entity = entity; look_record.camera = camera; look_record.direct_look = direct_look;
    look_record.pointer = delta;
    look_record.delta = delta ? *delta : input::LookDelta{};
}
void feed_detour(void* device, std::uint8_t button, int data, std::int16_t x, std::int16_t y,
                 std::int16_t dx, std::int16_t dy, std::uint8_t motionless) {
    input::forward_mouse_feed(captured, original_feed, device, button, data, x, y, dx, dy, motionless);
}
void look_detour(void* entity, const void* camera, void* direct_look, const input::LookDelta* delta) {
    input::forward_look_input(captured, original_look, entity, camera, direct_look, delta);
}

void actual_detour_test() {
    medicon::HookManager hooks;
    std::string error;
    require(hooks.create("input feed fixture", reinterpret_cast<void*>(&feed_fixture), reinterpret_cast<void*>(&feed_detour),
                          reinterpret_cast<void**>(&original_feed), error), "Create native feed detour");
    require(hooks.create("camera look fixture", reinterpret_cast<void*>(&look_fixture), reinterpret_cast<void*>(&look_detour),
                          reinterpret_cast<void**>(&original_look), error), "Create native camera detour");
    require(hooks.enable_all(error), "Enable both native input detours");
    input::MouseFeedFn volatile call_feed = &feed_fixture;
    input::LookInputFn volatile call_look = &look_fixture;
    int device{}, entity{}, camera{}, direct_look{};
    const input::LookDelta queued{87.5F, -42.25F};
    call_feed(&device, 4, -120, 1920, 1080, -321, 678, 2);
    require(feed_record.calls == 1 && feed_record.device == &device && feed_record.button == 4 &&
            feed_record.data == -120 && feed_record.x == 1920 && feed_record.y == 1080 &&
            feed_record.dx == -321 && feed_record.dy == 678 && feed_record.motionless == 2,
            "All eight register/stack arguments survive the uncaptured feed trampoline");
    call_look(&entity, &camera, &direct_look, &queued);
    require(look_record.calls == 1 && look_record.pointer == &queued && look_record.delta.x == queued.x,
            "Normal game camera input preserves the original vector/pointer");
    captured = true; // ClickGUI, Social, and pending cursor restoration use the same native gate.
    for (const auto button : {0, 1, 2, 4}) call_feed(&device, static_cast<std::uint8_t>(button), 1, 500, 600, 31, -49, 0);
    require(feed_record.calls == 1, "GUI motion/buttons/wheel never reach the native event queue");
    call_look(&entity, &camera, &direct_look, &queued);
    require(look_record.calls == 2 && look_record.entity == &entity && look_record.camera == &camera &&
            look_record.direct_look == &direct_look && look_record.delta.x == 0 && look_record.delta.y == 0 &&
            queued.x == 87.5F && queued.y == -42.25F,
            "Already-buffered look input becomes zero without mutating game data or opaque camera arguments");
    captured = false;
    call_look(&entity, &camera, &direct_look, &queued);
    call_feed(&device, 0, 0, 640, 400, 2, -3, 0);
    require(look_record.pointer == &queued && look_record.delta.y == queued.y && feed_record.calls == 2 &&
            feed_record.dx == 2 && feed_record.dy == -3, "New camera motion resumes after GUI close");
    require(hooks.disable_all(error), "Disable native input hooks before teardown");
}
}

int main() {
    try {
        resolution_test();
        actual_detour_test();
        std::cout << "Native input profile, mouse-feed and camera detour tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
