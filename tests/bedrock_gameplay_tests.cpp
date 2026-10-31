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
    medicon::sdk::Vec3 motion{};
    medicon::sdk::MovementReport movement;
    medicon::sdk::LocalPlayerMove move;
    medicon::sdk::LatencyReply reply;
    require(!access.is_latency_reply(reinterpret_cast<void*>(1)) &&
            !access.read_latency_reply(nullptr, nullptr, reply) && !access.send_latency_reply(reply),
            "Latency replay escaped the native profile/tick gate");
    require(!access.motion_dispatcher_target() && !access.prediction_correction_dispatcher_target() &&
            !access.disconnect_dispatcher_target() && !access.move_player_dispatcher_target() &&
            !access.read_movement_report(reinterpret_cast<void*>(1), movement) &&
            !access.read_player_move(reinterpret_cast<void*>(1), move) && !access.is_local_runtime_id(42) &&
            !access.local_player_move(reinterpret_cast<void*>(1), move) &&
            !access.is_client_network_handler(reinterpret_cast<void*>(1)) &&
            !access.local_motion(nullptr, nullptr, motion) &&
            !access.write_local_motion(nullptr, nullptr, motion), "Velocity escaped the native profile gate");
    std::string diagnostic;
    require(!access.locate(diagnostic) && !diagnostic.empty(), "test executable passed Minecraft version gate");
    require(!access.capture(nullptr, nullptr), "absent world produced a ready frame");
    access.end_tick();
    require(!access.rotate(0, 0) && !access.rotate_head_and_body(0) && !access.attack(1) &&
            !access.place_block({}, true) && !access.replaceable({}) && !access.set_velocity({1, 2, 3}) &&
            access.apply_knockback({1, 2, 3}, 1000) == medicon::sdk::KnockbackApplyResult::failed,
            "native action escaped closed tick scope");
    access.finish_rotation_control(true);
    require(access.rewrite_rotation_input(reinterpret_cast<void*>(1)) == medicon::sdk::RotationInputRewrite::ignored,
            "server rotation bypassed the native profile/tick gate");
    access.clear();
    require(access.frame().actors.empty() && !access.frame().ready, "world clear retained actor handles");
}

struct PredictionFixture {
    medicon::sdk::Vec3 motion;
    std::int64_t timestamp{};
    int stage{};
};
struct PredictionControllerFixture { PredictionFixture* state; };
struct PredictionComponentFixture { PredictionFixture* state; };

void enqueue_prediction_fixture(void* controller, const void* argument, const std::int64_t* timestamp) {
    auto& result = *static_cast<PredictionControllerFixture*>(controller)->state;
    require(result.stage == 0 && timestamp, "Native enqueue borrowed invalid arguments or ran after update");
    std::memcpy(&result.motion, static_cast<const std::byte*>(argument) + 0x38, sizeof result.motion);
    result.timestamp = *timestamp;
    result.stage = 1;
}

void update_prediction_fixture(void* component) {
    auto& result = *static_cast<PredictionComponentFixture*>(component)->state;
    require(result.stage == 1, "Native prediction update preceded enqueue");
    result.stage = 2;
}

void predicted_motion_boundary() {
    using namespace medicon::sdk;
    PredictionFixture result;
    PredictionControllerFixture controller{&result};
    PredictionComponentFixture component{&result};
    const Vec3 motion{-1.25F, 0.4F, 2.5F};
    require(detail::enqueue_gameplay_knockback(&component, &controller, motion, 1234567890123LL,
            &enqueue_prediction_fixture, &update_prediction_fixture) && result.stage == 2 &&
            result.motion.x == motion.x && result.motion.y == motion.y && result.motion.z == motion.z &&
            result.timestamp == 1234567890123LL, "Native movement ABI lost controller, component, motion or timestamp");
    result.stage = 0;
    require(!detail::enqueue_gameplay_knockback(nullptr, &controller, motion, 1, &enqueue_prediction_fixture, &update_prediction_fixture) &&
            !detail::enqueue_gameplay_knockback(&component, nullptr, motion, 1, &enqueue_prediction_fixture, &update_prediction_fixture) &&
            !detail::enqueue_gameplay_knockback(&component, &controller, motion, 0, &enqueue_prediction_fixture, &update_prediction_fixture) &&
            !detail::enqueue_gameplay_knockback(&component, &controller, motion, 1, nullptr, &update_prediction_fixture) &&
            !detail::enqueue_gameplay_knockback(&component, &controller, {NAN, 0, 0}, 1, &enqueue_prediction_fixture, &update_prediction_fixture) &&
            result.stage == 0, "Invalid prediction arguments reached native code");
}

void incoming_motion() {
    using namespace medicon::sdk;
    alignas(8) std::array<std::byte, 0x50> packet{};
    store(packet, 0x30, std::uint64_t{42});
    store(packet, 0x38, Vec3{2, 4, -6});
    store(packet, 0x48, std::uint64_t{12345});
    const auto address = reinterpret_cast<std::uintptr_t>(packet.data());
    Vec3 motion{7, 8, 9};
    const auto before = packet;
    require(!detail::read_gameplay_motion(address, 24, motion) && motion.x == 7 && packet == before,
            "Velocity matched another actor's runtime ID");
    require(!detail::read_gameplay_motion(address, 0, motion), "Zero local runtime ID accepted");
    require(detail::read_gameplay_motion(address, 42, motion) && motion.x == 2 && motion.y == 4 && motion.z == -6,
            "SetActorMotion layout was not read correctly");
    require(detail::write_gameplay_motion(address, {0.5F, 3, -1.5F}), "Incoming motion write failed");
    auto expected = before;
    store(expected, 0x38, Vec3{0.5F, 3, -1.5F});
    require(packet == expected, "Velocity changed the runtime ID, tick or unrelated packet fields");
    const auto valid = packet;
    require(!detail::write_gameplay_motion(address, {0, std::numeric_limits<float>::infinity(), 0}) && packet == valid,
            "Non-finite motion was written");
    store(packet, 0x38, Vec3{0, std::numeric_limits<float>::quiet_NaN(), 0});
    require(!detail::read_gameplay_motion(address, 42, motion), "Malformed incoming motion accepted");
    require(!detail::read_gameplay_motion(1, 42, motion) && !detail::read_gameplay_motion(UINTPTR_MAX - 8, 42, motion) &&
            !detail::write_gameplay_motion(1, {}) && !detail::write_gameplay_motion(UINTPTR_MAX - 8, {}),
            "Invalid packet memory accepted");
}

void velocity_packet_observations() {
    using namespace medicon::sdk;
    alignas(8) std::array<std::byte, 0xB0> auth{};
    store(auth, 0x38, Vec3{10, 64, -20});
    store(auth, 0x48, Vec3{0.1F, -0.2F, 0.3F});
    store(auth, 0x88, std::uint64_t{1ULL << 44});
    store(auth, 0x90, std::uint64_t{3});
    store(auth, 0xA8, std::uint64_t{987654321});
    const auto auth_address = reinterpret_cast<std::uintptr_t>(auth.data());
    const auto before = auth;
    MovementReport report;
    require(detail::read_gameplay_movement_report(auth_address, report) && auth == before &&
            report.input_tick == 987654321 && report.position.y == 64 && report.delta.y == -0.2F &&
            report.flags[0] == (1ULL << 44) && report.flags[1] == 3,
            "Read-only PlayerAuthInput observation lost tick, position, delta or flags");
    store(auth, 0x90, std::uint64_t{4});
    require(!detail::read_gameplay_movement_report(auth_address, report) && report.flags[1] == 3,
            "Unknown movement flags were accepted or published");
    auth = before;
    store(auth, 0x48, Vec3{0, std::numeric_limits<float>::quiet_NaN(), 0});
    require(!detail::read_gameplay_movement_report(auth_address, report) && report.delta.y == -0.2F,
            "Non-finite movement delta was accepted or published");
    require(!detail::read_gameplay_movement_report(1, report) &&
            !detail::read_gameplay_movement_report(UINTPTR_MAX - 8, report), "Invalid movement packet memory accepted");

    alignas(8) std::array<std::byte, 0x58> packet{};
    store(packet, 0x30, std::uint64_t{42});
    store(packet, 0x38, Vec3{-3, 70, 5});
    store(packet, 0x50, std::uint8_t{2});
    store(packet, 0x51, std::uint8_t{1});
    const auto address = reinterpret_cast<std::uintptr_t>(packet.data());
    const auto valid = packet;
    LocalPlayerMove move;
    require(detail::read_gameplay_local_move(address, 42, move) && packet == valid &&
            move.position.y == 70 && move.mode == 2 && move.on_ground && move.runtime_id == 42,
            "Local MovePlayer observation lost fields or runtime identity");
    require(!detail::read_gameplay_local_move(address, 24, move) &&
            !detail::read_gameplay_local_move(address, 0, move), "MovePlayer observation matched a foreign/absent actor");
    store(packet, 0x50, std::uint8_t{4});
    require(!detail::read_gameplay_local_move(address, 42, move) && move.mode == 2, "Unknown MovePlayer mode accepted");
    packet = valid;
    store(packet, 0x51, std::uint8_t{2});
    require(!detail::read_gameplay_local_move(address, 42, move) && move.on_ground, "Invalid MovePlayer boolean accepted");
    packet = valid;
    store(packet, 0x38, Vec3{0, std::numeric_limits<float>::infinity(), 0});
    require(!detail::read_gameplay_local_move(address, 42, move) && move.position.y == 70,
            "Invalid MovePlayer position was accepted or published");
    require(!detail::read_gameplay_local_move(1, 42, move) &&
            !detail::read_gameplay_local_move(UINTPTR_MAX - 8, 42, move), "Invalid MovePlayer memory accepted");
}

void delayed_velocity_memory() {
    using namespace medicon::sdk;
    alignas(8) std::array<std::byte, 48> component{};
    store(component, 0, Vec3{1, 64, 3});
    store(component, 12, Vec3{0.9F, 64, 2.9F});
    store(component, 24, Vec3{0.1F, -0.2F, 0.3F});
    store(component, 36, std::uint64_t{0x123456789ABCDEF0ULL});
    const auto address = reinterpret_cast<std::uintptr_t>(component.data());
    const auto before = component;
    require(detail::write_gameplay_velocity(address, {2, 4, -6}), "Delayed velocity component write failed");
    auto expected = before;
    store(expected, 24, Vec3{2, 4, -6});
    require(component == expected, "Delayed velocity changed position, previous position or adjacent data");
    const auto valid = component;
    require(!detail::write_gameplay_velocity(address, {0, std::numeric_limits<float>::quiet_NaN(), 0}) && component == valid,
            "Non-finite delayed velocity was written");
    store(component, 0, Vec3{0, std::numeric_limits<float>::infinity(), 0});
    const auto malformed = component;
    require(!detail::write_gameplay_velocity(address, {}) && component == malformed, "Invalid native state was mutated");
    require(!detail::write_gameplay_velocity(0, {}) && !detail::write_gameplay_velocity(1, {}) &&
            !detail::write_gameplay_velocity(UINTPTR_MAX - 8, {}), "Invalid native state pointer accepted");
}

void latency_packet_copy() {
    using namespace medicon::sdk;
    alignas(8) std::array<std::byte, 0x50> packet{};
    store(packet, 0, std::uintptr_t{0x1122334455667788ULL});
    store(packet, 0x20, std::uintptr_t{0x8877665544332211ULL});
    store(packet, 0x30, std::uint64_t{123456789012345ULL});
    store(packet, 0x38, std::uint8_t{0});
    store(packet, 0x40, std::uint32_t{1});
    store(packet, 0x48, std::uint64_t{0xDEADBEEFDEADBEEFULL});
    const auto before = packet;
    LatencyReply reply;
    const auto address = reinterpret_cast<std::uintptr_t>(packet.data());
    require(detail::copy_gameplay_latency(address, reply) && packet == before &&
            std::equal(reply.bytes.begin(), reply.bytes.end(), packet.begin()),
            "Latency clone changed native fields or included memory beyond the inspected 0x48-byte object");
    store(packet, 0x38, std::uint8_t{2});
    const auto valid_reply = reply;
    require(!detail::copy_gameplay_latency(address, reply) && reply.bytes == valid_reply.bytes,
            "Malformed latency flag was accepted or modified the result");
    require(!detail::copy_gameplay_latency(1, reply) && !detail::copy_gameplay_latency(UINTPTR_MAX - 8, reply),
            "Unreadable or overflowing latency object accepted");
    // A server can use this field as an ID, including values that look like
    // clocks in different units. Neither the ID nor any other byte may change.
    store(packet, 0x38, std::uint8_t{0});
    for (const auto id : {0ULL, 1000ULL, 20000000ULL, 20000000000ULL,
                          123456789012345ULL, 0xFEDCBA9876543210ULL, UINT64_MAX}) {
        store(packet, 0x30, static_cast<std::uint64_t>(id));
        require(detail::copy_gameplay_latency(address, reply) &&
                std::memcmp(reply.bytes.data(), packet.data(), reply.bytes.size()) == 0,
                "Latency copy altered the opaque server acknowledgment");
    }
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
        // PlayerAuthInput interact rotation; MovePlayer has no such field.
        if (id == 144) store(packet, 0x6C, std::array<float, 2>{12, 73});
        if (id == 19) store(packet, 0x50, std::array<std::uint8_t, 2>{0, 1});
        const auto address = reinterpret_cast<std::uintptr_t>(packet.data());
        const auto before = packet;
        auto expected = before;
        store(expected, rotation, std::array<float, 2>{-45, -170});
        store(expected, head, -170.0F);
        if (id == 144) store(expected, 0x6C, std::array<float, 2>{-45, -170});
        require(write_gameplay_rotation_input(address, id, 190, -45) && packet == expected,
                "server rotation did not update only packet pitch/yaw/head yaw/interact rotation");
        store(expected, rotation, std::array<float, 2>{90, -170});
        if (id == 144) store(expected, 0x6C, std::array<float, 2>{90, -170});
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
        if (id == 144) {
            packet = before;
            store(packet, 0x6C, std::array<float, 2>{std::numeric_limits<float>::quiet_NaN(), 73});
            const auto invalid_interact = packet;
            require(!write_gameplay_rotation_input(address, id, 90, -30) && packet == invalid_interact,
                    "unknown PlayerAuthInput interact rotation layout was changed");
        }
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
    try { components(); pools(); native_boundary(); predicted_motion_boundary(); incoming_motion(); velocity_packet_observations(); delayed_velocity_memory(); latency_packet_copy(); movement_input(); render_pitch_scope(); server_rotation_packets(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "Bedrock gameplay ABI tests passed\n";
}
