#include <medicon/sdk/BedrockGameplay.hpp>
#include <medicon/sdk/detail/GameplayMemory.hpp>

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<std::size_t N, class T>
void store(std::array<std::byte, N>& bytes, std::size_t offset, T value) {
    require(offset <= N && sizeof(value) <= N - offset, "test fixture overflow");
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

template<std::size_t N, class T>
void vector(std::array<std::byte, N>& bytes, std::size_t offset, T* begin, std::size_t count) {
    store(bytes, offset, reinterpret_cast<std::uintptr_t>(begin));
    store(bytes, offset + 8, reinterpret_cast<std::uintptr_t>(begin + count));
    store(bytes, offset + 16, reinterpret_cast<std::uintptr_t>(begin + count));
}

void components() {
    using medicon::sdk::detail::gameplay_component;
    using medicon::sdk::detail::gameplay_contains;
    alignas(8) std::array<std::byte, 0x80> storage{};
    std::array<std::uint32_t, 2048> sparse{}, second_sparse{};
    sparse.fill(UINT32_MAX);
    second_sparse.fill(UINT32_MAX);
    constexpr std::uint32_t entity = (7U << 18) | (2048U + 13U);
    std::array<std::uint32_t, 130> packed{};
    packed[129] = entity;
    second_sparse[13] = (7U << 18) | 129U;
    std::array<std::uint32_t*, 2> sparse_pages{sparse.data(), second_sparse.data()};
    std::array<float, 128> payload{};
    payload[1] = 42.5F;
    std::array<float*, 2> payload_pages{nullptr, payload.data()};
    vector(storage, 8, sparse_pages.data(), sparse_pages.size());
    vector(storage, 0x20, packed.data(), packed.size());
    vector(storage, 0x50, payload_pages.data(), payload_pages.size());
    const auto address = reinterpret_cast<std::uintptr_t>(storage.data());
    require(gameplay_contains(address, entity), "valid EnTT entity was absent");
    require(gameplay_component(address, entity, sizeof(float)) == reinterpret_cast<std::uintptr_t>(&payload[1]),
            "component slot did not use sparse-to-packed indirection");
    require(gameplay_component(address, entity, medicon::sdk::detail::actor_data_flags_size) ==
                reinterpret_cast<std::uintptr_t>(payload.data()) + 16,
            "actor flags used an 8-byte stride instead of the full 16-byte component");
    require(gameplay_component(address, entity, medicon::sdk::detail::move_input_size) ==
                reinterpret_cast<std::uintptr_t>(payload.data()) + 0x64,
            "movement input used the wrong native component stride");
    require(!gameplay_contains(address, entity + (1U << 18)), "stale entity version was accepted");
    require(!gameplay_component(address, entity, 0), "zero component size accepted");
    require(!gameplay_component(address, entity, 2048), "unbounded component stride accepted");
    second_sparse[13] = 1000;
    require(!gameplay_component(address, entity, 4), "sparse index past packed vector accepted");
    second_sparse[13] = 129;
    store(storage, 0x20 + 8, reinterpret_cast<std::uintptr_t>(packed.data()) + 1);
    require(!gameplay_contains(address, entity), "misaligned packed vector accepted");
    require(!gameplay_component(1, entity, 4) && !gameplay_component(0, entity, 4),
            "unreadable foreign storage accepted");
    require(!gameplay_contains(std::numeric_limits<std::uintptr_t>::max() - 4, entity),
            "overflowing storage pointer accepted");
}

void pools() {
    using medicon::sdk::detail::GameplayPool;
    using medicon::sdk::detail::read_gameplay_pools;
    alignas(8) std::array<std::byte, 0x100> registry{};
    alignas(8) std::array<std::byte, 192> nodes{};
    constexpr std::array<std::uint32_t, 6> hashes{0x1B5D5238, 0xBAC1B3CF, 0x75DF36B7, 0x4F6BA419, 0x85B93800, 0xD15944E2};
    alignas(8) std::array<std::byte, 0x80> storage{};
    for (std::size_t i = 0; i < hashes.size(); ++i) {
        store(nodes, i * 32 + 8, hashes[i]);
        store(nodes, i * 32 + 16, reinterpret_cast<std::uintptr_t>(storage.data()));
    }
    store(registry, 0x68, reinterpret_cast<std::uintptr_t>(nodes.data()));
    store(registry, 0x70, reinterpret_cast<std::uintptr_t>(nodes.data() + nodes.size()));
    store(registry, 0x78, reinterpret_cast<std::uintptr_t>(nodes.data() + nodes.size()));
    std::vector<GameplayPool> copied;
    require(read_gameplay_pools(reinterpret_cast<std::uintptr_t>(registry.data()), copied) && copied.size() == hashes.size(),
            "valid release-x64 registry was not copied");
    store(nodes, 8, hashes[1]);
    require(!read_gameplay_pools(reinterpret_cast<std::uintptr_t>(registry.data()), copied) && copied.empty(),
            "duplicate/missing component pools accepted");
    require(!read_gameplay_pools(1, copied) && copied.empty(), "unreadable registry accepted");
    store(registry, 0x68, std::numeric_limits<std::uintptr_t>::max() - 3);
    require(!read_gameplay_pools(reinterpret_cast<std::uintptr_t>(registry.data()), copied),
            "overflowing foreign vector accepted");
}

void native_boundary() {
    medicon::sdk::BedrockGameplay access;
    require(!access.frame().ready && !access.capture(reinterpret_cast<void*>(1), reinterpret_cast<void*>(1)),
            "native capture ran before profile validation");
    std::string diagnostic;
    require(!access.locate(diagnostic) && !diagnostic.empty(), "test executable passed Minecraft version gate");
    require(!access.capture(nullptr, nullptr), "absent world produced a ready frame");
    access.end_tick();
    require(!access.rotate(0, 0) && !access.rotate_head_and_body(0) && !access.attack(1) &&
            !access.place_block({}, true) && !access.replaceable({}),
            "native action escaped closed tick scope");
    access.finish_rotation_control(true);
    require(access.rewrite_rotation_input(reinterpret_cast<void*>(1)) == medicon::sdk::RotationInputRewrite::ignored,
            "server rotation bypassed the native profile/tick gate");
    access.clear();
    require(access.frame().actors.empty() && !access.frame().ready, "world clear retained actor handles");
}

void movement_input() {
    using namespace medicon::sdk::detail;
    alignas(8) std::array<std::byte, move_input_size> bytes{};
    const auto address = reinterpret_cast<std::uintptr_t>(bytes.data());
    constexpr std::uint32_t original = (1U << 13) | (1U << 7);
    store(bytes, 0, original);
    store(bytes, 0x10, original | (1U << 16));
    store(bytes, 0x24, std::array<float, 2>{0.5F, 1.0F});
    GameplayMoveInput input{};
    require(read_gameplay_move_input(address, input) && input.strafe == 0.5F && input.forward == 1.0F,
            "native movement input did not use the validated offsets");
    const auto before = bytes;
    require(write_gameplay_sprint_input(address, true), "native sprint input write failed");
    auto expected = before;
    store(expected, 0, original | (1U << 8));
    store(expected, 0x10, original | (1U << 16) | (1U << 8));
    require(bytes == expected, "sprint changed unrelated native movement or actor state");
    require(write_gameplay_sprint_input(address, false) && bytes == before, "sprint input release did not restore other bits");
    store(bytes, 0x60, std::uint16_t{1U << 6});
    require(read_gameplay_move_input(address, input) && input.forward == 0 && input.strafe == 0,
            "locked movement was treated as active input");
    store(bytes, 0x60, std::uint16_t{0});
    store(bytes, 0, original | 1U);
    require(read_gameplay_move_input(address, input) && input.sneaking, "native sneak input was ignored");
    store(bytes, 0x24, std::array<float, 2>{0, std::numeric_limits<float>::quiet_NaN()});
    require(!read_gameplay_move_input(address, input), "non-finite native movement accepted");
    store(bytes, 0x10, std::uint32_t{1U << 31});
    const auto malformed = bytes;
    require(!write_gameplay_sprint_input(address, true) && bytes == malformed, "unknown input layout was mutated");
    require(!write_gameplay_sprint_input(1, true) && !read_gameplay_move_input(1, input), "invalid input memory accepted");
}

void render_pitch_scope() {
    using medicon::sdk::detail::RenderPitchScope;
    std::array<float, 2> render{12, 73};
    const auto address = reinterpret_cast<std::uintptr_t>(render.data());
    bool applied = false;
    try {
        RenderPitchScope scope(address, -45);
        applied = scope.applied() && render[0] == -45 && render[1] == 73;
        throw std::runtime_error("draw callback exits");
    } catch (const std::runtime_error&) {}
    require(applied, "model draw did not borrow only the rendered pitch");
    require(render == std::array<float, 2>{12, 73}, "render pitch was not restored after the model callback");
    {
        RenderPitchScope invalid(1, -45);
        RenderPitchScope nonfinite(address, std::numeric_limits<float>::quiet_NaN());
        require(!invalid.applied() && !nonfinite.applied() && render[0] == 12,
                "invalid render pitch scope changed native memory");
    }
}

void server_rotation_packets() {
    using medicon::sdk::detail::write_gameplay_rotation_input;
    for (const int id : {144, 19}) {
        const std::size_t rotation = id == 144 ? 0x30 : 0x44;
        const std::size_t head = id == 144 ? 0x44 : 0x4C;
        std::array<std::byte, 0xE0> packet{};
        packet.fill(std::byte{0x5A});
        store(packet, 0x38, std::array<float, 3>{123, 64, -456});
        store(packet, rotation, std::array<float, 2>{12, 73});
        store(packet, head, 61.0F);
        if (id == 19) store(packet, 0x50, std::array<std::uint8_t, 2>{0, 1});
        const auto address = reinterpret_cast<std::uintptr_t>(packet.data());
        const auto before = packet;
        auto expected = before;
        store(expected, rotation, std::array<float, 2>{-45, -170});
        store(expected, head, -170.0F);
        require(write_gameplay_rotation_input(address, id, 190, -45) && packet == expected,
                "server rotation did not update only packet pitch/yaw/head yaw");
        store(expected, rotation, std::array<float, 2>{90, -170});
        require(write_gameplay_rotation_input(address, id, -170, 120) && packet == expected,
                "pitch-only server rotation or pitch clamp failed");
        const auto rotated = packet;
        require(!write_gameplay_rotation_input(address, id, std::numeric_limits<float>::infinity(), 0) &&
                !write_gameplay_rotation_input(address, id, 0, std::numeric_limits<float>::quiet_NaN()) &&
                !write_gameplay_rotation_input(address, 9, 0, 0) && packet == rotated,
                "invalid angle or unrelated packet was changed");
        store(packet, head, std::numeric_limits<float>::quiet_NaN());
        const auto malformed = packet;
        require(!write_gameplay_rotation_input(address, id, 90, -30) && packet == malformed,
                "malformed movement packet was partially changed");
        packet = before;
        store(packet, 0x38, std::array<float, 3>{0, std::numeric_limits<float>::quiet_NaN(), 0});
        const auto invalid_position = packet;
        require(!write_gameplay_rotation_input(address, id, 90, -30) && packet == invalid_position,
                "invalid movement position was accepted");
        if (id == 19) {
            packet = before;
            store(packet, 0x50, std::array<std::uint8_t, 2>{4, 1});
            const auto invalid_mode = packet;
            require(!write_gameplay_rotation_input(address, id, 90, -30) && packet == invalid_mode,
                    "unknown MovePlayer mode was changed");
        }
    }
    require(!write_gameplay_rotation_input(0, 144, 0, 0) && !write_gameplay_rotation_input(1, 19, 0, 0) &&
            !write_gameplay_rotation_input(UINTPTR_MAX - 8, 144, 0, 0),
            "unreadable or overflowing packet address was accepted");
}
} // namespace

int main() {
    try { components(); pools(); native_boundary(); movement_input(); render_pitch_scope(); server_rotation_packets(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "Bedrock gameplay ABI tests passed\n";
}
