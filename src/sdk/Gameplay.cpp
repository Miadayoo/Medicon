#include <medicon/sdk/BedrockGameplay.hpp>
#include <medicon/sdk/BedrockChat.hpp>
#include <medicon/custom_model/PlayerName.hpp>
#include <medicon/sdk/detail/GameplayMemory.hpp>
#include <medicon/memory/module_view.hpp>
#include <medicon/memory/signature_registry.hpp>

#include <Windows.h>
#include <winver.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <climits>
#include <cstring>
#include <cstdio>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace medicon::sdk {
namespace {

// Release x64 layout reference, inspected at the pinned revision. No upstream
// SDK code or game-owned STL object is linked into this bridge.
// https://github.com/surukotonai/solstice/tree/1d7808f52ef4ba6e34a7fbcc65294907a7761611
// SDK/EnttHashs.hpp, OffsetProvider.hpp, Actor/EntityId.hpp,
// include/entt/entity/{registry,sparse_set,storage}.hpp.
constexpr std::uint32_t state_hash = 0x1B5D5238, bounds_hash = 0xBAC1B3CF;
constexpr std::uint32_t rotation_hash = 0x75DF36B7, render_rotation_hash = 0xD15944E2;
constexpr std::uint32_t actor_data_flag_hash = 0xC67426F3;
constexpr std::uint32_t move_input_hash = 0x018B1887;
constexpr std::uint32_t type_hash = 0x4F6BA419;
constexpr std::uint32_t owner_hash = 0x85B93800, dead_hash = 0x710A62C4;
constexpr std::uint32_t head_hash = 0xBABE7211, body_hash = 0xD7F64BBA;
constexpr std::uint32_t render_position_hash = 0xE53C7221;
constexpr std::uint32_t attributes_hash = 0xFD3B0613;
// SharedAttributes::HEALTH: constructed at 0x31BD2F1, id at +4, name at +0x10.
constexpr std::size_t health_attribute_rva = 0x11DC54F0;
constexpr std::uint32_t runtime_id_hash = 0xFC0DBBB5;
constexpr std::uint32_t predicted_movement_hash = 0x18513471;
// PlayerAuthInputPacket/InputData values in the 1.26.50 protocol profile.
// https://mojang.github.io/bedrock-protocol-docs/1.26.50/types/player-auth-input-data/
constexpr int player_auth_input_packet_id = 144;
constexpr int move_player_packet_id = 19;
constexpr float degrees_to_radians = 0.01745329251994329577F;
// Confirmed against 1.26.52.3 packet validation/serialization routines.
constexpr std::size_t player_auth_input_rotation_offset = 0x30;
constexpr std::size_t player_auth_input_head_yaw_offset = 0x44;
constexpr std::size_t player_auth_input_analog_move_offset = 0x5C;
constexpr std::size_t player_auth_input_move_offset = 0x64;
constexpr std::size_t player_auth_input_raw_move_offset = 0x80;
constexpr std::size_t move_player_rotation_offset = 0x44;
constexpr std::size_t move_player_head_yaw_offset = 0x4C;
constexpr std::uint64_t no_server_rotation = 0x7FC000007FC00000ULL;
// PlayerAuthInputPacketPayload follows the 0x30-byte Packet base:
// rot +0x30, pos +0x38, head yaw +0x44, pos delta +0x48, vehicle rot +0x54,
// analog move +0x5C, move +0x64, interact rot +0x6C, camera +0x74,
// raw move +0x80, then the 16-byte InputData set at +0x88 and InputMode +0x98.
// The rotation/position offsets above (and MovePlayer's) are the validated
// ones; this order is LeviLamina's 26.51 payload, whose field set matches the
// 1.26.52 protocol schema. Servers take interactions (attacks, block use)
// from the interact rotation, so it must follow the requested rotation too.
// https://github.com/LiteLDev/LeviLamina/blob/a0d87a55ea2593d28e63fa1eb6879d373ce95899/src/mc/network/packet/PlayerAuthInputPacketPayload.h
constexpr std::size_t player_auth_input_interact_rotation_offset = 0x6C;
// InputData's reserved upper 62 bits are checked before changing any bit, so
// an unknown layout is passed through unchanged.
constexpr std::size_t player_auth_input_flags_offset = 0x88;
constexpr std::size_t player_auth_input_delta_offset = 0x48;
constexpr std::size_t player_auth_input_vehicle_rotation_offset = 0x54;
constexpr std::size_t player_auth_input_mode_offset = 0x98;
constexpr std::uint32_t invisible_flag = 5; // ActorFlags::Invisible
constexpr std::uint32_t sprinting_input_bit = 20;
constexpr std::uint32_t start_sprinting_input_bit = 25;
constexpr std::uint32_t stop_sprinting_input_bit = 26;
constexpr std::uint32_t entity_mask = 0x3FFFF;
// Item's std::string identifier, assigned by its constructor at 0x24B19CB.
constexpr std::size_t item_identifier_offset = 0xD8;
constexpr std::size_t maximum_entities = 65536;

bool copy_bytes(std::uintptr_t address, void* output, std::size_t count) noexcept {
    if (!address || !output || !count || count - 1 > UINTPTR_MAX - address) return false;
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                             output, count, &copied) && copied == count;
}

template<class T> bool read_at(std::uintptr_t address, std::size_t offset, T& output) noexcept {
    return address && offset <= UINTPTR_MAX - address && copy_bytes(address + offset, &output, sizeof(T));
}

template<class T> bool write_at(std::uintptr_t address, std::size_t offset, const T& value) noexcept {
    if (!address || offset > UINTPTR_MAX - address || sizeof(T) - 1 > UINTPTR_MAX - (address + offset)) return false;
    SIZE_T written = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address + offset),
                              &value, sizeof(T), &written) && written == sizeof(T);
}

struct ForeignVector { std::uintptr_t begin, end, capacity; };
bool vector_at(std::uintptr_t base, std::size_t offset, ForeignVector& vector,
               std::size_t stride, std::size_t maximum) noexcept {
    if (!read_at(base, offset, vector) || !stride || vector.begin > vector.end ||
        vector.end > vector.capacity || (vector.end - vector.begin) % stride ||
        (vector.capacity - vector.begin) % stride ||
        (vector.capacity - vector.begin) / stride > maximum) return false;
    return vector.begin || (!vector.end && !vector.capacity);
}

std::optional<std::size_t> component_index(std::uintptr_t storage, std::uint32_t entity) noexcept {
    ForeignVector sparse{}, packed{};
    if (!storage || !vector_at(storage, 8, sparse, 8, 128) ||
        !vector_at(storage, 0x20, packed, 4, maximum_entities)) return std::nullopt;
    const auto index = entity & entity_mask;
    const std::size_t page_index = index / 2048;
    if (page_index >= (sparse.end - sparse.begin) / 8) return std::nullopt;
    std::uintptr_t page = 0;
    std::uint32_t slot = 0, stored = 0;
    if (!read_at(sparse.begin, page_index * 8, page) ||
        !read_at(page, (index % 2048) * 4, slot)) return std::nullopt;
    const auto packed_index = slot & entity_mask;
    if (packed_index >= (packed.end - packed.begin) / 4 ||
        !read_at(packed.begin, packed_index * 4, stored) || stored != entity) return std::nullopt;
    return packed_index;
}

bool finite(Vec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool valid_position(Vec3 value) noexcept {
    return finite(value) && std::abs(value.x) <= 32000000 && std::abs(value.z) <= 32000000 &&
           std::abs(value.y) <= 1000000;
}

float distance_squared(Vec3 a, Vec3 b) noexcept {
    const auto x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
    return x * x + y * y + z * z;
}

bool supported_version(std::string& diagnostic) {
    std::array<wchar_t, 32768> path{};
    const auto count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count == path.size()) return false;
    const std::wstring_view full(path.data(), count);
    const auto separator = full.find_last_of(L"\\/");
    const auto name = full.substr(separator == std::wstring_view::npos ? 0 : separator + 1);
    if (_wcsicmp(std::wstring(name).c_str(), L"Minecraft.Windows.exe")) {
        diagnostic = "Gameplay SDK requires Minecraft.Windows.exe 1.26.52.3.";
        return false;
    }
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.data(), &ignored);
    if (!size || size > 16 * 1024 * 1024) return false;
    std::vector<std::byte> bytes(size);
    if (!GetFileVersionInfoW(path.data(), 0, size, bytes.data())) return false;
    VS_FIXEDFILEINFO* info = nullptr;
    UINT info_size = 0;
    if (!VerQueryValueW(bytes.data(), L"\\", reinterpret_cast<void**>(&info), &info_size) ||
        !info || info_size < sizeof(*info) || info->dwSignature != 0xFEEF04BD ||
        HIWORD(info->dwFileVersionMS) != 1 || LOWORD(info->dwFileVersionMS) != 26 ||
        HIWORD(info->dwFileVersionLS) != 52 || LOWORD(info->dwFileVersionLS) != 3) {
        diagnostic = "Gameplay SDK withheld: executable is not Minecraft 1.26.52.3.";
        return false;
    }
    return true;
}

} // namespace

namespace detail {

bool send_gameplay_inventory_packet(void** transaction, void* sender, const NativeInventoryPacketCalls& calls) {
    if (!transaction || !*transaction || !sender || !calls.construct_payload || !calls.construct_packet ||
        !calls.destroy_packet || !calls.send) return false;
    alignas(8) std::array<std::byte, 0x150> payload;
    alignas(8) std::array<std::byte, 0x188> packet;
    calls.construct_payload(payload.data(), transaction, false);
    // This move constructor also destroys its moved-from payload argument.
    calls.construct_packet(packet.data(), payload.data());
    struct Cleanup {
        void* packet;
        const NativeInventoryPacketCalls& calls;
        ~Cleanup() { calls.destroy_packet(packet, 0); }
    } cleanup{packet.data(), calls};
    calls.send(sender, packet.data());
    return true;
}

bool send_gameplay_inventory_changes(const std::array<NativeInventoryChange, 2>& changes,
    void* context, const NativeInventoryCalls& calls) {
    static_assert(sizeof(NativeInventorySource) == 12 && offsetof(NativeInventorySource, flags) == 8);
    if (!context || !calls.construct || !calls.destroy || !calls.add_action || !calls.create_transaction ||
        !calls.destroy_transaction || !calls.send_transaction) return false;
    if (changes[0].source.type != 0 || changes[0].source.container != 0 || changes[0].slot >= 36) return false;
    for (const auto& change : changes) {
        if (!change.from || !change.to || change.source.flags != 0) return false;
        if (change.source.type == 0 && change.source.container == 0 && change.slot < 36) continue;
        if (change.source.type == 0 && change.source.container == 120 && change.slot < 4) continue;
        if (change.source.type == 2 && change.source.container == 0 && change.slot == 0) continue;
        return false;
    }
    void* transaction = calls.create_transaction(context);
    if (!transaction) return false;
    struct Cleanup {
        void*& transaction;
        void* context;
        const NativeInventoryCalls& calls;
        ~Cleanup() { if (transaction) calls.destroy_transaction(context, transaction); }
    } cleanup{transaction, context, calls};
    for (const auto& change : changes) {
        alignas(16) std::array<std::byte, 0x200> action;
        calls.construct(action.data(), &change.source, change.slot, change.from, change.to);
        struct DestroyAction {
            void* action;
            const NativeInventoryCalls& calls;
            ~DestroyAction() { calls.destroy(action); }
        } destroy{action.data(), calls};
        calls.add_action(static_cast<std::byte*>(transaction) + 0x10, action.data(), false);
    }
    const bool sent = calls.send_transaction(context, &transaction);
    return sent && transaction == nullptr;
}

bool read_gameplay_motion(std::uintptr_t packet, std::uint64_t local_runtime_id, Vec3& motion) noexcept {
    std::uint64_t runtime_id{};
    Vec3 incoming{};
    if (!local_runtime_id || !read_at(packet, 0x30, runtime_id) || runtime_id != local_runtime_id ||
        !read_at(packet, 0x38, incoming) || !finite(incoming)) return false;
    motion = incoming;
    return true;
}

bool write_gameplay_motion(std::uintptr_t packet, Vec3 motion) noexcept {
    return finite(motion) && write_at(packet, 0x38, motion);
}

bool write_gameplay_velocity(std::uintptr_t component, Vec3 velocity) noexcept {
    struct State { Vec3 position, previous_position, velocity; } state{};
    if (!finite(velocity) || !read_at(component, 0, state) ||
        !valid_position(state.position) || !finite(state.velocity)) return false;
    return write_at(component, offsetof(State, velocity), velocity);
}

bool copy_gameplay_latency(std::uintptr_t packet, LatencyReply& reply) noexcept {
    LatencyReply copied;
    if (!copy_bytes(packet, copied.bytes.data(), copied.bytes.size()) ||
        std::to_integer<unsigned>(copied.bytes[0x38]) > 1) return false;
    reply = copied;
    return true;
}

bool read_gameplay_movement_report(std::uintptr_t packet, MovementReport& report) noexcept {
    MovementReport copied;
    if (!read_at(packet, 0x38, copied.position) || !read_at(packet, 0x48, copied.delta) ||
        !read_at(packet, 0x88, copied.flags) || !read_at(packet, 0xA8, copied.input_tick) ||
        !valid_position(copied.position) || !finite(copied.delta) || (copied.flags[1] & ~std::uint64_t{3})) return false;
    report = copied;
    return true;
}

bool read_gameplay_local_move(std::uintptr_t packet, std::uint64_t runtime_id, LocalPlayerMove& move) noexcept {
    LocalPlayerMove copied;
    std::uint64_t target{};
    std::uint8_t ground{};
    if (!runtime_id || !read_at(packet, 0x30, target) || target != runtime_id ||
        !read_at(packet, 0x38, copied.position) || !read_at(packet, 0x50, copied.mode) ||
        !read_at(packet, 0x51, ground) || copied.mode > 3 || ground > 1 || !valid_position(copied.position)) return false;
    copied.on_ground = ground != 0;
    copied.runtime_id = target;
    move = copied;
    return true;
}

bool enqueue_gameplay_knockback(void* component, void* controller, Vec3 velocity, std::int64_t timestamp,
                                EnqueuePredictedMotion enqueue, UpdatePredictedMotion update) {
    if (!component || !controller || !enqueue || !update || timestamp <= 0 || !finite(velocity)) return false;
    // Inspected queue function reads only the vector at packet +0x38 and
    // the timestamp passed separately. This is a scalar argument envelope,
    // not a constructed game Packet or a cross-runtime shared_ptr.
    alignas(8) std::array<std::byte, 0x48> argument{};
    std::memcpy(argument.data() + 0x38, &velocity, sizeof velocity);
    enqueue(controller, argument.data(), &timestamp);
    update(component);
    return true;
}

bool read_gameplay_stack_item(std::uintptr_t stack, std::uintptr_t& item, std::uint8_t& count) noexcept {
    item = 0;
    count = 0;
    std::uintptr_t reference{}, candidate{};
    std::uint8_t copies{};
    if (!read_at(stack, 8, reference) || !read_at(stack, 0x22, copies)) return false;
    if (reference && !read_at(reference, 0, candidate)) return false;
    if (candidate && copies) { item = candidate; count = copies; }
    return true;
}

bool read_gameplay_attribute(std::uintptr_t component, std::uint32_t attribute_id, float& value) noexcept {
    constexpr std::size_t instance_size = 0x88, maximum_attributes = 64;
    ForeignVector ids{}, instances{};
    if (!attribute_id || !vector_at(component, 0, ids, 4, maximum_attributes) ||
        !vector_at(component, 0x18, instances, instance_size, maximum_attributes)) return false;
    const auto count = (ids.end - ids.begin) / 4;
    std::array<std::uint32_t, maximum_attributes> known{};
    if (count != (instances.end - instances.begin) / instance_size ||
        (count && !copy_bytes(ids.begin, known.data(), count * 4))) return false;
    for (std::size_t i = 0; i < count; ++i) {
        if (known[i] != attribute_id) continue;
        float maximum{}, current{};
        if (!read_at(instances.begin, i * instance_size + 0x78, maximum) ||
            !read_at(instances.begin, i * instance_size + 0x7C, current) ||
            !std::isfinite(maximum) || !std::isfinite(current) ||
            maximum <= 0.0F || current < 0.0F || current > maximum) return false;
        value = current;
        return true;
    }
    return false;
}

bool read_gameplay_pools(std::uintptr_t registry, std::vector<GameplayPool>& output) noexcept {
    output.clear();
    try {
        // Verified on 1.26.52.3: the pool map's packed node vector is +0x68.
        // The reference EnTT source alone would incorrectly suggest +0x50.
        // Each dense-map node: next[8], hash[4]+padding[4], shared_ptr[16].
        ForeignVector packed{};
        if (!vector_at(registry, 0x68, packed, 32, 8192)) return false;
        const auto count = (packed.end - packed.begin) / 32;
        if (count < 5) return false;
        std::vector<std::byte> bytes(count * 32);
        if (!copy_bytes(packed.begin, bytes.data(), bytes.size())) return false;
        output.reserve(count);
        unsigned required = 0;
        for (std::size_t i = 0; i < count; ++i) {
            GameplayPool entry{};
            std::memcpy(&entry.hash, bytes.data() + i * 32 + 8, 4);
            std::memcpy(&entry.storage, bytes.data() + i * 32 + 16, 8);
            if (!entry.storage || entry.storage % 8) continue;
            if (entry.hash == state_hash) required |= 1;
            if (entry.hash == bounds_hash) required |= 2;
            if (entry.hash == rotation_hash) required |= 4;
            if (entry.hash == type_hash) required |= 8;
            if (entry.hash == owner_hash) required |= 16;
            if (entry.hash == render_rotation_hash) required |= 32;
            output.push_back(entry);
        }
        std::ranges::sort(output, {}, &GameplayPool::hash);
        for (std::size_t i = 1; i < output.size(); ++i)
            if (output[i - 1].hash == output[i].hash) { output.clear(); return false; }
        if (required != 63) { output.clear(); return false; }
        return true;
    } catch (...) { output.clear(); return false; }
}

std::uintptr_t gameplay_component(std::uintptr_t storage, std::uint32_t entity,
                                  std::size_t element_size) noexcept {
    if (!element_size || element_size > 1024) return 0;
    const auto index = component_index(storage, entity);
    if (!index) return 0;
    ForeignVector payload{};
    if (!vector_at(storage, 0x50, payload, 8, maximum_entities / 128 + 1) ||
        *index / 128 >= (payload.end - payload.begin) / 8) return 0;
    std::uintptr_t page = 0;
    if (!read_at(payload.begin, (*index / 128) * 8, page) || !page ||
        (*index % 128) * element_size > UINTPTR_MAX - page) return 0;
    return page + (*index % 128) * element_size;
}

bool gameplay_contains(std::uintptr_t storage, std::uint32_t entity) noexcept {
    return component_index(storage, entity).has_value();
}

bool read_gameplay_move_input(std::uintptr_t component, GameplayMoveInput& input) noexcept {
    input = {};
    std::uint32_t flags{};
    std::uint16_t control{};
    Vec2 move{};
    // 1.26.52.3: lookup at 0x283481B uses a 0x64-byte stride; reset at
    // 0x449810 accesses input +0/+0x10, move +0x24 and control +0x60.
    if (!read_at(component, 0, flags) || (flags >> 27) != 0 ||
        !read_at(component, 0x24, move) || !read_at(component, 0x60, control) ||
        !std::isfinite(move.x) || !std::isfinite(move.y) ||
        std::abs(move.x) > 1.01F || std::abs(move.y) > 1.01F) return false;
    input.sneaking = (flags & 1U) != 0 || (control & 1U) != 0;
    if ((control & (1U << 6)) == 0) {
        input.strafe = move.x;
        input.forward = move.y;
    }
    return true;
}

bool write_gameplay_sprint_input(std::uintptr_t component, bool sprinting) noexcept {
    std::uint32_t input{}, raw{};
    if (!read_at(component, 0, input) || !read_at(component, 0x10, raw) ||
        (input >> 27) != 0 || (raw >> 27) != 0) return false;
    constexpr std::uint32_t sprint_down = 1U << 8;
    const auto update = [=](std::uint32_t flags) { return sprinting ? flags | sprint_down : flags & ~sprint_down; };
    if (!write_at(component, 0, update(input))) return false;
    if (write_at(component, 0x10, update(raw))) return true;
    (void)write_at(component, 0, input);
    return false;
}

bool write_gameplay_sprinting_flag(std::uintptr_t component, bool sprinting) noexcept {
    std::uint64_t flags{};
    if (!read_at(component, 0, flags)) return false;
    constexpr std::uint64_t sprinting_flag = std::uint64_t{1} << 3;
    flags = sprinting ? flags | sprinting_flag : flags & ~sprinting_flag;
    return write_at(component, 0, flags);
}

bool write_gameplay_sprint_speed(std::uintptr_t component, bool sprinting) noexcept {
    // 1.26.52.3 GetSpeedInAirWithSprint writes the selected speed at +0x0C.
    constexpr float walking_speed = 0.020F;
    constexpr float sprint_speed = 0.026F;
    return write_at(component, 0x0C, sprinting ? sprint_speed : walking_speed);
}

bool write_gameplay_rotation_input(std::uintptr_t packet, int packet_id, float yaw, float pitch,
                                  bool vector_fix) noexcept {
    if (!std::isfinite(yaw) || !std::isfinite(pitch)) return false;
    const bool auth_input = packet_id == player_auth_input_packet_id;
    if (!auth_input && packet_id != move_player_packet_id) return false;
    const auto rotation_offset = auth_input ? player_auth_input_rotation_offset : move_player_rotation_offset;
    const auto head_offset = auth_input ? player_auth_input_head_yaw_offset : move_player_head_yaw_offset;
    Vec2 original_rotation{}, original_interact{};
    Vec2 original_analog_move{}, original_move{}, original_raw_move{};
    float original_head{};
    Vec3 position{};
    const auto valid_angle = [](float value) { return std::isfinite(value) && std::abs(value) <= 360000; };
    if (!read_at(packet, rotation_offset, original_rotation) || !read_at(packet, head_offset, original_head) ||
        !valid_angle(original_rotation.x) || !valid_angle(original_rotation.y) || !valid_angle(original_head) ||
        !read_at(packet, 0x38, position) || !valid_position(position)) return false;
    if (auth_input && vector_fix) {
        if (!read_at(packet, player_auth_input_interact_rotation_offset, original_interact) ||
            !valid_angle(original_interact.x) || !valid_angle(original_interact.y)) return false;
        if (!read_at(packet, player_auth_input_analog_move_offset, original_analog_move) ||
            !read_at(packet, player_auth_input_move_offset, original_move) ||
            !read_at(packet, player_auth_input_raw_move_offset, original_raw_move)) return false;
        const auto valid_move = [](Vec2 value) {
            return std::isfinite(value.x) && std::isfinite(value.y) &&
                std::abs(value.x) <= 1.01F && std::abs(value.y) <= 1.01F;
        };
        if (!valid_move(original_analog_move) || !valid_move(original_move) || !valid_move(original_raw_move))
            return false;
    } else if (!auth_input) {
        std::array<std::uint8_t, 2> mode_and_ground{};
        if (!read_at(packet, 0x50, mode_and_ground) || mode_and_ground[0] > 3 || mode_and_ground[1] > 1)
            return false;
    }
    yaw = std::remainder(yaw, 360.0F);
    const Vec2 rotation{std::clamp(pitch, -90.0F, 90.0F), yaw};
    Vec2 analog_move = original_analog_move;
    Vec2 move = original_move;
    Vec2 raw_move = original_raw_move;
    if (auth_input && vector_fix) {
        // Move vectors are local to the packet's player yaw. Rotate them by
        // viewYaw - serverYaw so changing silent server rotation does not
        // redirect the player's world-space movement.
        const float delta = std::remainder(original_rotation.y - yaw, 360.0F) * degrees_to_radians;
        const float cosine = std::cos(delta), sine = std::sin(delta);
        const auto rotate_move = [cosine, sine](Vec2 value) {
            return Vec2{value.x * cosine - value.y * sine,
                        value.x * sine + value.y * cosine};
        };
        analog_move = rotate_move(original_analog_move);
        move = rotate_move(original_move);
        raw_move = rotate_move(original_raw_move);
    }

    const auto restore = [&] {
        (void)write_at(packet, rotation_offset, original_rotation);
        (void)write_at(packet, head_offset, original_head);
        if (auth_input && vector_fix) {
            (void)write_at(packet, player_auth_input_interact_rotation_offset, original_interact);
            (void)write_at(packet, player_auth_input_analog_move_offset, original_analog_move);
            (void)write_at(packet, player_auth_input_move_offset, original_move);
            (void)write_at(packet, player_auth_input_raw_move_offset, original_raw_move);
        }
    };
    if (!write_at(packet, rotation_offset, rotation)) return false;
    if (!write_at(packet, head_offset, yaw)) { restore(); return false; }
    if (auth_input && vector_fix &&
        !write_at(packet, player_auth_input_interact_rotation_offset, rotation)) {
        restore();
        return false;
    }
    if (auth_input && vector_fix && (!write_at(packet, player_auth_input_analog_move_offset, analog_move) ||
        !write_at(packet, player_auth_input_move_offset, move) ||
        !write_at(packet, player_auth_input_raw_move_offset, raw_move))) {
        restore();
        return false;
    }
    return true;
}

bool write_gameplay_move_fix_input(std::uintptr_t packet, bool& sprinting) noexcept {
    Vec2 rotation{}, original_move{}, original_vehicle{};
    Vec3 delta{};
    std::array<std::uint64_t, 2> original_flags{};
    std::int32_t original_mode{};
    if (!read_at(packet, player_auth_input_rotation_offset, rotation) ||
        !read_at(packet, player_auth_input_delta_offset, delta) ||
        !read_at(packet, player_auth_input_move_offset, original_move) ||
        !read_at(packet, player_auth_input_vehicle_rotation_offset, original_vehicle) ||
        !read_at(packet, player_auth_input_flags_offset, original_flags) ||
        !read_at(packet, player_auth_input_mode_offset, original_mode) ||
        !std::isfinite(rotation.x) || !std::isfinite(rotation.y) ||
        std::abs(rotation.x) > 180 || std::abs(rotation.y) > 360000 ||
        !valid_position(delta) || !std::isfinite(original_move.x) || !std::isfinite(original_move.y) ||
        std::abs(original_move.x) > 1.01F || std::abs(original_move.y) > 1.01F ||
        !std::isfinite(original_vehicle.x) || !std::isfinite(original_vehicle.y) ||
        (original_flags[1] & ~std::uint64_t{3}) != 0 || original_mode < 0 || original_mode > 4)
        return false;

    const auto bit = [](unsigned index) { return std::uint64_t{1} << index; };
    auto flags = original_flags;
    bool next_sprinting = sprinting;
    if (flags[0] & bit(start_sprinting_input_bit)) next_sprinting = true;
    else if (flags[0] & bit(stop_sprinting_input_bit)) next_sprinting = false;
    else if (flags[0] & bit(sprinting_input_bit)) next_sprinting = true;

    const bool has_input = original_move.x != 0 || original_move.y != 0;
    const float motion = std::hypot(delta.x, delta.z);
    if (!has_input && motion == 0) { sprinting = next_sprinting; return true; }

    Vec2 move{};
    if (has_input) {
        move = original_move;
        // With no horizontal displacement, retain the input rather than invent
        // a direction from atan2(0, 0). Motion-only packets keep zero input.
        if (motion > 0.000001F) {
            const float yaw = std::remainder(rotation.y, 360.0F) * degrees_to_radians;
            const float sine = std::sin(yaw), cosine = std::cos(yaw);
            move = {(delta.x * cosine + delta.z * sine) / motion,
                    (delta.z * cosine - delta.x * sine) / motion};
        }
        if (std::abs(move.x) < 0.001F) move.x = 0;
        if (std::abs(move.y) < 0.001F) move.y = 0;
    }
    // Motion-controller input supplies its continuous direction in mMove.
    // Remove Up/Down/Left/Right/UpLeft/UpRight left over from keyboard input.
    flags[0] &= ~(std::uint64_t{0x3F} << 10);
    if (move.y <= 0) {
        flags[0] &= ~(bit(sprinting_input_bit) | bit(start_sprinting_input_bit) | bit(27));
        if (next_sprinting) flags[0] |= bit(stop_sprinting_input_bit);
        next_sprinting = false;
    }

    const auto restore = [&] {
        (void)write_at(packet, player_auth_input_move_offset, original_move);
        (void)write_at(packet, player_auth_input_vehicle_rotation_offset, original_vehicle);
        (void)write_at(packet, player_auth_input_flags_offset, original_flags);
        (void)write_at(packet, player_auth_input_mode_offset, original_mode);
    };
    if (!write_at(packet, player_auth_input_move_offset, move) ||
        !write_at(packet, player_auth_input_vehicle_rotation_offset, move) ||
        !write_at(packet, player_auth_input_flags_offset, flags) ||
        !write_at(packet, player_auth_input_mode_offset, std::int32_t{4})) {
        restore();
        return false;
    }
    sprinting = next_sprinting;
    return true;
}

RenderPitchScope::RenderPitchScope(std::uintptr_t rotation_component, float pitch) noexcept {
    if (!rotation_component || !std::isfinite(pitch) ||
        !read_at(rotation_component, 0, original_pitch_) ||
        !read_at(rotation_component, 8, original_previous_pitch_) ||
        !std::isfinite(original_pitch_) || !std::isfinite(original_previous_pitch_)) return;
    const float requested = std::clamp(pitch, -90.0F, 90.0F);
    if (!write_at(rotation_component, 0, requested)) return;
    if (!write_at(rotation_component, 8, requested)) {
        (void)write_at(rotation_component, 0, original_pitch_);
        return;
    }
    component_ = rotation_component;
}

RenderPitchScope::~RenderPitchScope() {
    if (component_) {
        (void)write_at(component_, 0, original_pitch_);
        (void)write_at(component_, 8, original_previous_pitch_);
    }
}

} // namespace detail

struct BedrockGameplay::Impl {
    GameplayFrame frame;
    bool located = false, supported = false, active = false;
    DWORD thread = 0;
    std::uintptr_t client = 0, player = 0, registry = 0, block_source = 0;
    std::atomic<std::uintptr_t> diagnostic_local_game_mode{};
    // The LevelRenderer child whose camera fields the last capture validated.
    std::uintptr_t view_player = 0;
    // The local player's RenderPositionComponent, which the game moves every
    // frame to where it draws the model. Resolved again by every capture.
    std::uintptr_t render_position = 0;
    std::atomic_bool sprint_packet_override{};
    std::atomic_bool sprint_packet_last_state{};
    std::atomic_bool move_fix_sprinting{};
    std::uintptr_t packet_send_target{};
    void(*packet_send_original)(void*, void*){};
    VelocityTraceObserver velocity_observer{};
    void* velocity_observer_context{};
    // NaN marks no request. Publishing both axes at once prevents a send on
    // another thread from pairing a new yaw with the previous tick's pitch.
    std::atomic<std::uint64_t> server_rotation{no_server_rotation};
    // Auto Select keeps the block slot selected while placing (like Solstice's
    // default Full switch) so the game reports it to the server on its own
    // tick before a placement uses it. The original slot comes back once
    // nothing has placed from the switched slot for a second. Survives clear().
    std::uintptr_t switch_supplies = 0;
    int switch_from = -1, switch_to = -1;
    ULONGLONG switch_used = 0;
    bool switched_this_tick = false;
    bool destroy_active = false;
    std::uintptr_t destroy_mode = 0;
    BlockPos destroy_position{};
    std::uint8_t destroy_face = 0;
    std::uint8_t destroy_diagnostic_stage = 0;
    std::uint8_t destroy_diagnostic_profile_stage = 0;
    bool destroy_diagnostic_continue_return = false;
    bool destroy_diagnostic_native_destroyed = false;
    float destroy_diagnostic_progress = 0.0F;
    std::uintptr_t destroy_diagnostic_requested_mode = 0;
    std::uintptr_t destroy_diagnostic_requested_owner = 0;
    std::uintptr_t destroy_diagnostic_requested_vtable_rva = 0;
    bool destroy_diagnostic_requested_owner_matches = false;
    BlockPos destroy_diagnostic_requested_position{};
    std::uint8_t destroy_diagnostic_requested_face = 0;
    std::atomic_bool destroy_speed_active{};
    std::atomic<std::uint64_t> destroy_speed_target_xz{};
    std::atomic<std::int32_t> destroy_speed_target_y{};
    std::atomic<std::uint8_t> destroy_speed_target_face{};
    std::atomic<float> destroy_speed_scale{1.0F};
    mutable std::atomic<float> destroy_speed_server_progress{};
    std::uint32_t local_entity = 0;
    std::uint64_t capture_serial = 0, head_body_rotation_serial = 0;
    std::uintptr_t head_body_rotation_player = 0, head_body_rotation_registry = 0;
    std::uint32_t head_body_rotation_entity = 0;
    float last_head_yaw = 0, last_body_yaw = 0;
    bool head_body_rotation_valid = false;
    bool rotation_control_enabled = false;
    bool rotation_render_enabled = false;
    bool rotation_yaw_valid = false;
    bool rotation_override_active = false;
    float rotation_yaw = 0, rotation_previous_yaw = 0, rotation_pitch = 0, rotation_previous_pitch = 0;
    std::uint64_t rotation_pitch_serial = 0;
    std::uintptr_t rotation_player = 0, rotation_registry = 0;
    std::uint32_t rotation_entity = 0;
    std::uintptr_t body_yaw_interpolator = 0, head_yaw_interpolator = 0;
    const void* (*model_name_getter)(void*){};
    std::uintptr_t local_swing_method = 0;
    memory::AddressRange image;
    std::vector<memory::AddressRange> executable_regions;
    std::vector<detail::GameplayPool> pools;
    std::string diagnostic;
    mutable InventoryDiagnostic inventory_diagnostic;
    std::uintptr_t inventory_add_action{}, inventory_transaction_vtable{};
    std::uint32_t health_attribute = 0;

    void trace_velocity(VelocityTracePhase phase, const char* path, KnockbackApplyResult result,
                        std::uint64_t latency_id = 0) const noexcept {
        if (!velocity_observer || !frame.ready) return;
        NativeVelocityTrace event;
        event.phase = phase; event.path = path; event.result = result;
        event.capture_serial = capture_serial; event.latency_id = latency_id;
        event.position = frame.local.position; event.velocity = frame.local.velocity;
        const auto vector = component(local_entity, state_hash, 36);
        std::uintptr_t cached{};
        event.state_matches_cached = vector && read_at(player, 0x218, cached) && vector == cached;
        (void)read_at(vector, 0, event.position);
        (void)read_at(vector, 24, event.velocity);
        velocity_observer(velocity_observer_context, event);
    }

    static std::uint64_t block_xz_key(BlockPos position) noexcept {
        return static_cast<std::uint64_t>(static_cast<std::uint32_t>(position.x)) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(position.z)) << 32);
    }

    void publish_destroy_speed_target(BlockPos position, std::uint8_t face, float scale) noexcept {
        destroy_speed_target_xz.store(block_xz_key(position), std::memory_order_relaxed);
        destroy_speed_target_y.store(position.y, std::memory_order_relaxed);
        destroy_speed_target_face.store(face, std::memory_order_relaxed);
        destroy_speed_scale.store(scale, std::memory_order_relaxed);
        destroy_speed_server_progress.store(0.0F, std::memory_order_relaxed);
        destroy_speed_active.store(true, std::memory_order_release);
    }

    void clear_destroy_speed_target() noexcept {
        destroy_speed_active.store(false, std::memory_order_release);
        destroy_speed_scale.store(1.0F, std::memory_order_relaxed);
        destroy_speed_server_progress.store(0.0F, std::memory_order_relaxed);
    }

    bool executable(std::uintptr_t address) const noexcept {
        return image.contains(address) && std::ranges::any_of(executable_regions,
            [address](const auto& region) { return region.contains(address); });
    }

    std::uintptr_t method(std::uintptr_t object, std::size_t index) const noexcept {
        std::uintptr_t table = 0, target = 0;
        if (!object || object % 8 || !read_at(object, 0, table) ||
            !image.contains(table, (index + 1) * 8) || !read_at(table, index * 8, target) ||
            !executable(target)) return 0;
        return target;
    }

    bool has_method(std::uintptr_t object, std::uintptr_t expected) const noexcept {
        constexpr std::size_t maximum_methods = 32;
        std::uintptr_t table = 0;
        if (!expected || !object || object % 8 || !read_at(object, 0, table) ||
            !image.contains(table, maximum_methods * sizeof(std::uintptr_t))) return false;
        std::array<std::uintptr_t, maximum_methods> methods{};
        return copy_bytes(table, methods.data(), sizeof(methods)) &&
            std::ranges::find(methods, expected) != methods.end();
    }

    template<std::size_t Size>
    bool native_identity(std::uintptr_t target, std::size_t rva,
                         const std::array<unsigned char, Size>& expected) const noexcept {
        std::array<unsigned char, Size> actual{};
        return rva <= UINTPTR_MAX - image.address && target == image.address + rva &&
               executable(target) && copy_bytes(target, actual.data(), actual.size()) && actual == expected;
    }

    bool native_identity(std::uintptr_t target, std::size_t rva,
                         const std::array<unsigned char, 16>& expected) const noexcept {
        return native_identity<16>(target, rva, expected);
    }

    std::uintptr_t storage(std::uint32_t hash) const noexcept {
        const auto it = std::ranges::lower_bound(pools, hash, {}, &detail::GameplayPool::hash);
        return it != pools.end() && it->hash == hash && method(it->storage, 0) ? it->storage : 0;
    }

    std::uintptr_t component(std::uint32_t entity, std::uint32_t hash, std::size_t size) const noexcept {
        return detail::gameplay_component(storage(hash), entity, size);
    }

    bool context(std::uintptr_t actor, std::uint32_t& entity) const noexcept {
        std::uintptr_t actor_registry = 0;
        return method(actor, 0) && read_at(actor, 0x10, actor_registry) && actor_registry == registry &&
               read_at(actor, 0x18, entity) && (entity & entity_mask) != entity_mask;
    }

    bool snapshot(std::uintptr_t actor, ActorSnapshot& result) const noexcept {
        std::uint32_t entity = 0, type = 0;
        struct State { Vec3 position, old_position, velocity; } state{};
        struct Shape { Bounds bounds; float width, height; } shape{};
        std::uintptr_t owner = 0;
        if (!context(actor, entity) || !read_at(component(entity, owner_hash, 8), 0, owner) || owner != actor ||
            !read_at(component(entity, state_hash, sizeof(State)), 0, state) ||
            !read_at(component(entity, bounds_hash, sizeof(Shape)), 0, shape) ||
            !read_at(component(entity, type_hash, 4), 0, type) ||
            !valid_position(state.position) || !finite(state.velocity) ||
            distance_squared(state.velocity, {}) > 100000000 ||
            !valid_position(shape.bounds.min) || !valid_position(shape.bounds.max)) return false;
        const auto& a = shape.bounds.min;
        const auto& b = shape.bounds.max;
        if (b.x <= a.x || b.y <= a.y || b.z <= a.z || b.x - a.x > 128 ||
            b.y - a.y > 128 || b.z - a.z > 128 ||
            state.position.x < a.x - 4 || state.position.x > b.x + 4 ||
            state.position.y < a.y - 4 || state.position.y > b.y + 4 ||
            state.position.z < a.z - 4 || state.position.z > b.z + 4) return false;
        const auto dead_pool = storage(dead_hash);
        result = {actor, state.position, state.velocity, shape.bounds,
                  (type & 0xFF) == 63, (type & 0x100) != 0 &&
                  !(dead_pool && detail::gameplay_contains(dead_pool, entity)), actor == player, false};
        std::uintptr_t actor_data{};
        if (result.player && model_name_getter && read_at(actor, 0x128, actor_data) && actor_data &&
            read_at(component(entity, 0x18F957AF, 8), 0, result.unique_id)) {
            if (const auto name = read_msvc_string(model_name_getter(reinterpret_cast<void*>(actor)), 512)) {
                result.model_name = medicon::custom_model::model_player_name(*name);
                result.multiline_name = name->find('\n') != std::string::npos;
            }
        }
        std::uint64_t flags{};
        result.invisible = read_at(component(entity, actor_data_flag_hash, detail::actor_data_flags_size), 0, flags) &&
            (flags & (std::uint64_t{1} << invisible_flag)) != 0;
        if (result.player && actor != player) result.armor_pieces = worn_armor_pieces(entity);
        if (float health{}; detail::read_gameplay_attribute(
                component(entity, attributes_hash, detail::attributes_component_size), health_attribute, health))
            result.health = health;
        // A teleport is not motion to draw between.
        if (valid_position(state.old_position) && distance_squared(state.old_position, state.position) <= 8 * 8) {
            result.previous_position = state.old_position;
            result.has_previous = true;
        }
        return true;
    }

    std::uintptr_t game_mode() const noexcept {
        std::uintptr_t mode = 0, owner = 0, table = 0;
        if (!read_at(player, 0xAA0, mode) || !method(mode, 0) ||
            !read_at(mode, 8, owner) || owner != player || !read_at(mode, 0, table)) return 0;
        // RVAs identify the two observed 1.26.52.3 implementations. Validate
        // the actual methods at each call site; unrelated attack/build slots
        // can differ across otherwise compatible GameMode instances.
        const bool base = table == image.address + 0xE827560;
        const bool survival = table == image.address + 0xE827600;
        if (!base && !survival) return 0;
        return mode;
    }

    void resolve_health_attribute() noexcept {
        if (health_attribute) return;
        try {
            const auto attribute = image.address + health_attribute_rva;
            const auto name = read_msvc_string(reinterpret_cast<const void*>(attribute + 0x10), 32);
            if (name && *name == "minecraft:health") (void)read_at(attribute, 4, health_attribute);
        } catch (...) {}
    }

    void forget_switch() noexcept { switch_supplies = 0; switch_from = switch_to = -1; }

    // Capture only, after the player was validated.
    void restore_switched_slot() noexcept {
        if (switch_from < 0) return;
        std::uintptr_t supplies = 0;
        int selected = -1;
        // Another player or world, or a slot the user picked: nothing to undo.
        if (!read_at(player, 0x5B8, supplies) || supplies != switch_supplies ||
            !read_at(supplies, 0x10, selected) || selected != switch_to) { forget_switch(); return; }
        if (GetTickCount64() - switch_used < 1000) return;
        write_at(supplies, 0x10, switch_from);
        forget_switch();
    }

    bool on_tick() const noexcept {
        if (!active || !supported || !frame.ready || GetCurrentThreadId() != thread) return false;
        std::uint32_t entity = 0;
        const auto local_target = method(client, 31);
        if (!local_target || reinterpret_cast<void*(__fastcall*)(void*)>(local_target)(
            reinterpret_cast<void*>(client)) != reinterpret_cast<void*>(player)) return false;
        return context(player, entity) && entity == local_entity;
    }

    // The current local player, from a host thread that may not be baseTick:
    // screen ticks arrive outside the capture window.
    bool local_player_current() const noexcept {
        if (!supported || !frame.ready) return false;
        const auto local_target = method(client, 31);
        return local_target && reinterpret_cast<std::uintptr_t(__fastcall*)(void*)>(local_target)(
            reinterpret_cast<void*>(client)) == player;
    }

    // Player's shared_ptr<ContainerManagerModel>: assigned at 0x1EEE80, locked
    // by the client network handlers at +0x5A0/+0x5A8. Type at +0x51, read by
    // the model's getContainerType (0xA3AF50).
    bool container_model(std::uintptr_t& model, std::int8_t& type) const noexcept {
        model = 0;
        return read_at(player, 0x5A0, model) && model && method(model, 0) && read_at(model, 0x51, type);
    }

    std::uintptr_t inventory_container() const noexcept {
        std::uintptr_t supplies{}, container{};
        if (!read_at(player, 0x5B8, supplies) || !read_at(supplies, 0xB8, container)) {
            inventory_diagnostic.stage = "inventory-container-unreadable";
            return 0;
        }
        std::uintptr_t table{}, owner{};
        (void)read_at(container, 0, table);
        const auto get = method(container, 7), set = method(container, 12);
        inventory_diagnostic.container_vtable_rva = image.contains(table) ? table - image.address : 0;
        inventory_diagnostic.getter_rva = image.contains(get) ? get - image.address : 0;
        inventory_diagnostic.setter_rva = image.contains(set) ? set - image.address : 0;
        // Inventory is not FillingContainer nor the transaction manager's
        // cloned working container. Its vector starts at +0x198, not +0x1A0.
        constexpr std::array<unsigned char, 16> get_prefix{
            0x48,0x8D,0x05,0x49,0x0C,0x7C,0x0F,0x85,0xD2,0x78,0x2E,0x4C,0x8B,0x81,0x98,0x01};
        if (table != image.address + 0xE6AFE10 || !native_identity(get, 0x25A5100, get_prefix) ||
            !read_at(container, 0x1B0, owner) || owner != player) {
            inventory_diagnostic.stage = "inventory-type-or-owner-rejected";
            return 0;
        }
        return container;
    }

    std::uintptr_t inventory_stack(std::uintptr_t container, int slot) const noexcept {
        const auto get_item = method(container, 7);
        return get_item ? reinterpret_cast<std::uintptr_t(__fastcall*)(void*, int)>(get_item)(
            reinterpret_cast<void*>(container), slot) : 0;
    }

    // ActorEquipmentComponent {hand, armor}: 16 bytes per entity, and the second
    // container is the one constructed with ContainerType Armor (0x253BCB4).
    std::uintptr_t armor_container(std::uint32_t entity) const noexcept {
        constexpr std::uint32_t equipment_hash = 0xB06141A9;
        std::uintptr_t container{};
        return read_at(component(entity, equipment_hash, 16), 8, container) ? container : 0;
    }
    std::uintptr_t armor_container() const noexcept { return armor_container(local_entity); }

    std::int8_t worn_armor_pieces(std::uint32_t entity) const noexcept {
        const auto container = armor_container(entity);
        std::int8_t pieces = 0;
        for (int slot = 0; slot < 4; ++slot) {
            const auto stack = inventory_stack(container, slot);
            std::uintptr_t item{};
            std::uint8_t count{};
            if (!stack || !detail::read_gameplay_stack_item(stack, item, count)) return -1;
            if (item) ++pieces;
        }
        return pieces;
    }

    // ItemStack::EMPTY_ITEM, returned by getSlot for out-of-range slots (0x3440D19).
    std::uintptr_t empty_stack() const noexcept {
        const auto stack = image.address + 0x11D65D50;
        std::uintptr_t table{};
        return read_at(stack, 0, table) && table == image.address + 0xE6D6CD0 ? stack : 0;
    }

    int enchant_level(std::uint8_t enchant, std::uintptr_t stack) const noexcept {
        // EnchantUtils::getEnchantLevel; a stack with no tag returns 0 before any allocation.
        constexpr std::uintptr_t rva = 0x3260390;
        constexpr std::array<unsigned char, 16> prefix{
            0x56,0x57,0x53,0x48,0x81,0xEC,0x80,0,0,0,0x89,0xCB,0x48,0x8B,0x4A,0x10};
        const auto target = image.address + rva;
        if (!native_identity(target, rva, prefix)) return 0;
        return reinterpret_cast<int(__fastcall*)(std::uint32_t, std::uintptr_t)>(target)(enchant, stack);
    }

    void describe_item(std::uintptr_t stack, std::uintptr_t item, std::uint8_t count, InventoryItemInfo& info) const noexcept {
        try {
            const auto name = read_msvc_string(reinterpret_cast<const void*>(item + item_identifier_offset), 128);
            info = name ? classify_inventory_item(*name, count) : InventoryItemInfo{InventoryItemKind::other, 0.0F, count};
        } catch (...) { info = {InventoryItemKind::other, 0.0F, count}; return; }
        (void)read_at(item, 0xAA, info.id);
        // Solstice's ItemUtils::getItemValue: armor below diamond ignores enchantments.
        constexpr std::uint8_t protection = 0, fire_protection = 1, sharpness = 9, efficiency = 15;
        using Kind = InventoryItemKind;
        if (info.kind == Kind::sword) info.value += static_cast<float>(enchant_level(sharpness, stack));
        else if (info.kind == Kind::pickaxe || info.kind == Kind::axe || info.kind == Kind::shovel)
            info.value += static_cast<float>(enchant_level(efficiency, stack));
        else if (info.kind >= Kind::helmet && info.value >= 4.0F)
            info.value += static_cast<float>(enchant_level(protection, stack) + enchant_level(fire_protection, stack));
    }

    // InventorySource {type, container id, flags}; the default is the player's
    // inventory. Layouts follow the native builders at 0x1F8E88 and 0x23FE39.
    using InventoryChange = detail::NativeInventoryChange;

    bool send_inventory_transaction(const std::array<InventoryChange, 2>& changes) const noexcept {
        using Calls = detail::NativeInventoryCalls;
        auto& trace = inventory_diagnostic;
        ++trace.attempts;
        trace.stage = "native-profile-rejected";
        const auto base = image.address;
        constexpr std::array<unsigned char, 16> action_construct_prefix{
            0x55,0x41,0x56,0x56,0x57,0x53,0x48,0x83,0xEC,0x50,0x48,0x8D,0x6C,0x24,0x50,0x48};
        constexpr std::array<unsigned char, 16> action_destroy_prefix{
            0x56,0x57,0x53,0x48,0x83,0xEC,0x30,0x48,0x89,0xCE,0x48,0x8D,0xB9,0x68,0x01,0};
        constexpr std::array<unsigned char, 16> transaction_construct_prefix{
            0x55,0x56,0x57,0x53,0x48,0x83,0xEC,0x58,0x48,0x8D,0x6C,0x24,0x50,0x0F,0x29,0x75};
        constexpr std::array<unsigned char, 16> add_action_prefix{
            0x55,0x41,0x57,0x41,0x56,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xEC,0x60,0x02,0};
        constexpr std::array<unsigned char, 16> payload_construct_prefix{
            0x55,0x41,0x56,0x56,0x57,0x53,0x48,0x81,0xEC,0x80,0x01,0,0,0x48,0x8D,0xAC};
        constexpr std::array<unsigned char, 16> packet_construct_prefix{
            0x55,0x56,0x48,0x83,0xEC,0x48,0x48,0x8D,0x6C,0x24,0x40,0x48,0xC7,0x45,0,0xFE};
        constexpr std::array<unsigned char, 16> packet_destroy_prefix{
            0x56,0x57,0x48,0x83,0xEC,0x28,0x89,0xD7,0x48,0x89,0xCE,0x48,0x8D,0x05,0x8E,0x33};
        std::uintptr_t transaction_destroy{}, packet_destroy{}, sender{};
        if (!native_identity(base + 0x1D4AD0, 0x1D4AD0, action_construct_prefix) ||
            !native_identity(base + 0x1D4C70, 0x1D4C70, action_destroy_prefix) ||
            !native_identity(base + 0x23FB00, 0x23FB00, transaction_construct_prefix) ||
            !native_identity(inventory_add_action, 0x266EE40, add_action_prefix) ||
            inventory_transaction_vtable != base + 0xE6D6C00 ||
            !read_at(inventory_transaction_vtable, 0, transaction_destroy) || transaction_destroy != base + 0x23FA10 ||
            !native_identity(base + 0x2732240, 0x2732240, payload_construct_prefix) ||
            !native_identity(base + 0x272C430, 0x272C430, packet_construct_prefix) ||
            !read_at(base + 0xE6D5FD0, 0, packet_destroy) ||
            !native_identity(packet_destroy, 0x222C30, packet_destroy_prefix)) return false;
        trace.stage = "packet-sender-unavailable";
        if (!read_at(client, 0x1D0, sender)) return false;
        const auto send_target = method(sender, 4);
        if (!send_target) return false;
        const auto send = send_target == packet_send_target && packet_send_original ? packet_send_original
            : reinterpret_cast<void(*)(void*, void*)>(send_target);
        struct Context { const Impl& state; void* sender; void(*send)(void*, void*); } context{*this, reinterpret_cast<void*>(sender), send};
        const Calls calls{
            reinterpret_cast<decltype(Calls::construct)>(base + 0x1D4AD0),
            reinterpret_cast<decltype(Calls::destroy)>(base + 0x1D4C70),
            reinterpret_cast<decltype(Calls::add_action)>(inventory_add_action),
            [](void* opaque) -> void* {
                const auto& state = static_cast<Context*>(opaque)->state;
                auto* transaction = state.native_allocate(0x68);
                if (!transaction) return nullptr;
                std::memset(transaction, 0, 0x68);
                const auto table = state.inventory_transaction_vtable;
                std::memcpy(transaction, &table, sizeof(table));
                const std::uint32_t seed = 0;
                const std::uint8_t allocator_tag = 0;
                reinterpret_cast<void(*)(void*, const std::uint32_t*, const std::uint8_t*)>(state.image.address + 0x23FB00)(
                    static_cast<std::byte*>(transaction) + 0x10, &seed, &allocator_tag);
                return transaction;
            },
            [](void* opaque, void* transaction) {
                const auto& state = static_cast<Context*>(opaque)->state;
                reinterpret_cast<void(*)(void*, unsigned)>(state.image.address + 0x23FA10)(transaction, 1);
            },
            [](void* opaque, void** transaction) {
                const auto& context = *static_cast<Context*>(opaque);
                const auto base = context.state.image.address;
                using PacketCalls = detail::NativeInventoryPacketCalls;
                return detail::send_gameplay_inventory_packet(transaction, context.sender, {
                    reinterpret_cast<decltype(PacketCalls::construct_payload)>(base + 0x2732240),
                    reinterpret_cast<decltype(PacketCalls::construct_packet)>(base + 0x272C430),
                    reinterpret_cast<decltype(PacketCalls::destroy_packet)>(base + 0x222C30), context.send});
            }};
        trace.stage = "transaction-allocation-or-send-failed";
        if (!detail::send_gameplay_inventory_changes(changes, &context, calls)) return false;
        ++trace.sent;
        trace.stage = "sent-solstice-normal";
        return true;
    }

    // Container::setItem (Inventory vtable slot 12, forwards to slot 13 with
    // force=false). The server does not resend slots for these moves until the
    // screen closes, so without this the client keeps showing what it gave away.
    bool set_inventory_slot(std::uintptr_t container, int slot, std::uintptr_t stack) const noexcept {
        constexpr std::array<unsigned char, 16> prefix{
            0x48,0x8B,0x01,0x48,0x8B,0x40,0x68,0x4C,0x8B,0x15,0x4A,0x69,0xF2,0x0E,0x45,0x31};
        const auto set = method(container, 12);
        if (!stack || !native_identity(set, 0x1D5150, prefix)) return false;
        reinterpret_cast<void(__fastcall*)(std::uintptr_t, int, std::uintptr_t)>(set)(container, slot, stack);
        return true;
    }

    // Actor::setArmor, the local half of Player's own armor setter (0x20A5B4),
    // which skips the InventoryTransactionManager action that the packet replaces.
    bool set_armor_slot(int slot, std::uintptr_t stack) const noexcept {
        constexpr std::uintptr_t rva = 0x1A0B8E0;
        constexpr std::array<unsigned char, 16> prefix{
            0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xEC,0x18};
        const auto target = image.address + rva;
        if (!stack || !native_identity(target, rva, prefix)) return false;
        reinterpret_cast<void(__fastcall*)(std::uintptr_t, int, std::uintptr_t)>(target)(player, slot, stack);
        return true;
    }

    // Writing one slot changes what the other side's pointer reads, so both
    // writes use the owned copies of a native InventoryAction (+0xD0 and +0x168).
    // Only call after send_inventory_transaction validated these natives.
    template<class Write>
    bool exchange_local(std::uintptr_t first, std::uintptr_t second, Write write) const noexcept {
        using Calls = detail::NativeInventoryCalls;
        alignas(16) std::array<std::byte, 0x200> action;
        const detail::NativeInventorySource source{};
        reinterpret_cast<decltype(Calls::construct)>(image.address + 0x1D4AD0)(action.data(), &source, 0, first, second);
        const auto copies = reinterpret_cast<std::uintptr_t>(action.data());
        const bool written = write(copies + 0x168, copies + 0xD0);
        reinterpret_cast<decltype(Calls::destroy)>(image.address + 0x1D4C70)(action.data());
        return written;
    }

    void note_local_update(bool updated) const noexcept {
        inventory_diagnostic.stage = updated ? "sent-local-updated" : "sent-local-update-failed";
    }

    // Native allocation for objects the game later frees itself. The pointer
    // at 0x11805230 is what every inline operator new in the image reads.
    void* native_allocate(std::size_t size) const noexcept {
        std::uintptr_t allocator{};
        if (!read_at(image.address, 0x11805230, allocator)) return nullptr;
        const auto allocate = method(allocator, 1);
        return allocate ? reinterpret_cast<void*(__fastcall*)(void*, std::size_t)>(allocate)(
            reinterpret_cast<void*>(allocator), size) : nullptr;
    }

    struct Block { std::uint16_t id; bool solid; };
    std::optional<Block> block(BlockPos position) const noexcept {
        if (std::abs(static_cast<double>(position.x)) > 32000000 ||
            std::abs(static_cast<double>(position.z)) > 32000000 ||
            position.y < -4096 || position.y > 4096) return std::nullopt;
        const auto get = method(block_source, 2);
        if (!get) return std::nullopt;
        using GetBlock = std::uintptr_t(__fastcall*)(void*, const BlockPos*);
        const auto address = reinterpret_cast<GetBlock>(get)(reinterpret_cast<void*>(block_source), &position);
        std::uintptr_t legacy = 0;
        std::uint16_t id = 0;
        std::uint8_t flags = 0;
        if (!read_at(address, 0x68, legacy) || !method(legacy, 0) ||
            !read_at(legacy, 0x17E, id) || !read_at(legacy, 0x164, flags)) return std::nullopt;
        return Block{id, (flags & 2) != 0};
    }

    bool destroy_methods(std::uintptr_t mode, std::uintptr_t& start, std::uintptr_t& destroy,
                        std::uintptr_t& proceed, std::uintptr_t& stop) const noexcept {
        std::uintptr_t table{};
        if (!mode || !read_at(mode, 0, table)) return false;
        const auto base_table = image.address + 0xE827560;
        const auto survival_table = image.address + 0xE827600;
        constexpr std::array<unsigned char, 16> base_start_prefix{
            0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x55,0x53,0x48,0x83,0xEC,0x78};
        constexpr std::array<unsigned char, 16> base_destroy_prefix{
            0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x83,0xEC,0x78};
        constexpr std::array<unsigned char, 16> continue_prefix{
            0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xEC,0xD8};
        constexpr std::array<unsigned char, 16> survival_start_prefix{
            0x48,0x83,0xEC,0x28,0x80,0xB9,0xC8,0x00,0x00,0x00,0x01,0x75,0x17,0x80,0x3D,0x90};
        constexpr std::array<unsigned char, 16> survival_destroy_prefix{
            0x80,0xB9,0xC8,0x00,0x00,0x00,0x01,0x0F,0x85,0xF3,0xA5,0xFF,0xFF,0x80,0x3D,0xB0};
        if (table != base_table && table != survival_table) return false;
        start = method(mode, 1);
        destroy = method(mode, 2);
        proceed = method(mode, 3);
        stop = method(mode, 4);
        // stopDestroyBlock is detoured by Regen before the module starts. Its
        // original signature was checked by destroy_stop_target() before the
        // hook was installed, so here validate the vtable slot's exact pinned
        // address instead of comparing the now-detoured function prologue.
        constexpr std::uintptr_t stop_rva = 0x259BBF0;
        const bool stop_matches = stop == image.address + stop_rva && executable(stop);
        if (table == base_table) {
            return native_identity(start, 0x259A320, base_start_prefix) &&
                native_identity(destroy, 0x259ABB0, base_destroy_prefix) &&
                native_identity(proceed, 0x259AFD0, continue_prefix) && stop_matches;
        }
        return native_identity(start, 0x25A05D0, survival_start_prefix) &&
            native_identity(destroy, 0x25A05B0, survival_destroy_prefix) &&
            native_identity(proceed, 0x259AFD0, continue_prefix) && stop_matches;
    }

    bool clear_ray(Vec3 destination) const noexcept {
        // Conservative voxel traversal: collision/solid blocks occlude aim;
        // missing chunks and failed reads withhold visibility. Limits protect
        // the game tick from unbounded work for distant ESP actors.
        const auto length = std::sqrt(distance_squared(frame.eye, destination));
        if (!std::isfinite(length) || length > 128) return false;
        const auto steps = std::max(1, static_cast<int>(std::ceil(length * 4)));
        BlockPos previous{INT_MAX, INT_MAX, INT_MAX};
        for (int i = 1; i < steps; ++i) {
            const auto t = static_cast<float>(i) / static_cast<float>(steps);
            const Vec3 point{frame.eye.x + (destination.x - frame.eye.x) * t,
                             frame.eye.y + (destination.y - frame.eye.y) * t,
                             frame.eye.z + (destination.z - frame.eye.z) * t};
            const BlockPos voxel{static_cast<int>(std::floor(point.x)), static_cast<int>(std::floor(point.y)),
                                 static_cast<int>(std::floor(point.z))};
            if (voxel.x == previous.x && voxel.y == previous.y && voxel.z == previous.z) continue;
            previous = voxel;
            const auto value = block(voxel);
            if (!value || value->solid) return false;
        }
        return true;
    }

    bool clear_ray_to_block(BlockPos target) const noexcept {
        const Vec3 destination{static_cast<float>(target.x) + 0.5F,
                               static_cast<float>(target.y) + 0.5F,
                               static_cast<float>(target.z) + 0.5F};
        const auto length = std::sqrt(distance_squared(frame.eye, destination));
        if (!std::isfinite(length) || length > 128) return false;
        const auto steps = std::max(1, static_cast<int>(std::ceil(length * 4)));
        BlockPos previous{INT_MAX, INT_MAX, INT_MAX};
        for (int i = 1; i < steps; ++i) {
            const auto t = static_cast<float>(i) / static_cast<float>(steps);
            const Vec3 point{frame.eye.x + (destination.x - frame.eye.x) * t,
                             frame.eye.y + (destination.y - frame.eye.y) * t,
                             frame.eye.z + (destination.z - frame.eye.z) * t};
            const BlockPos voxel{static_cast<int>(std::floor(point.x)), static_cast<int>(std::floor(point.y)),
                                 static_cast<int>(std::floor(point.z))};
            if (voxel.x == target.x && voxel.y == target.y && voxel.z == target.z) continue;
            if (voxel.x == previous.x && voxel.y == previous.y && voxel.z == previous.z) continue;
            previous = voxel;
            const auto value = block(voxel);
            if (!value || value->solid) return false;
        }
        return true;
    }

    // Copies only: the camera origin and FOV of the renderer child, and the
    // client's view matrix. Also used from Present between ticks.
    bool read_view(std::uintptr_t renderer_player, Vec3& camera, Vec2& fov,
                   std::array<float, 16>& matrix) const noexcept {
        if (!client || !renderer_player ||
            !read_at(renderer_player, 0x9CC, camera) || !valid_position(camera) ||
            !read_at(renderer_player, 0xF58, fov.x) || !read_at(renderer_player, 0xF6C, fov.y) ||
            !std::isfinite(fov.x) || !std::isfinite(fov.y) || fov.x < 0.01F || fov.y < 0.01F ||
            fov.x > 20 || fov.y > 20 || !read_at(client, 0x420, matrix) ||
            !std::ranges::all_of(matrix, [](float v) { return std::isfinite(v) && std::abs(v) < 100; })) return false;
        for (std::size_t i = 0; i < 3; ++i) {
            const auto offset = i * 4;
            const float magnitude = matrix[offset] * matrix[offset] + matrix[offset + 1] * matrix[offset + 1] +
                                    matrix[offset + 2] * matrix[offset + 2];
            if (magnitude < 0.5F || magnitude > 1.5F) return false;
        }
        return true;
    }

    // A copy only. The drawn position stays within a couple of ticks' motion
    // of the tick position; anything else (a moved component, a teleport) is
    // dropped and the overlay falls back to the tick positions.
    void read_render_position(ActorSnapshot& local) const noexcept {
        Vec3 position{};
        local.has_render_position = false;
        if (!read_at(render_position, 0, position) || !valid_position(position)) return;
        float step = distance_squared(local.velocity, {});
        if (local.has_previous) step = std::max(step, distance_squared(local.position, local.previous_position));
        const float reach = 1.0F + 2.0F * std::sqrt(step);
        if (!(distance_squared(position, local.position) <= reach * reach)) return;
        local.render_position = position;
        local.has_render_position = true;
    }

    void projection() noexcept {
        const auto get_renderer = method(client, 187);
        if (!get_renderer) return;
        const auto renderer = reinterpret_cast<std::uintptr_t(__fastcall*)(void*)>(get_renderer)(
            reinterpret_cast<void*>(client));
        std::uintptr_t renderer_player = 0;
        Vec3 camera{};
        Vec2 fov{};
        std::array<float, 16> matrix{};
        if (!renderer || !read_at(renderer, 0x468, renderer_player) ||
            !read_view(renderer_player, camera, fov, matrix)) return;
        view_player = renderer_player;
        frame.camera = camera;
        frame.fov = fov;
        frame.view_matrix = matrix;
        frame.projection_ready = true;
    }
};

BedrockGameplay::BedrockGameplay() : impl_(std::make_unique<Impl>()) {}
BedrockGameplay::~BedrockGameplay() = default;

bool BedrockGameplay::locate(std::string& diagnostic) {
    auto& state = *impl_;
    if (!state.located) {
        state.located = true;
        state.diagnostic = "Cannot verify Minecraft gameplay ABI; gameplay bridge withheld.";
        if (supported_version(state.diagnostic)) {
            if (const auto module = memory::current_process_module(&state.diagnostic)) {
                state.image = module->image;
                for (const auto& region : module->executable_regions)
                    state.executable_regions.push_back({region.address, region.bytes.size()});
                state.supported = true;
                std::vector<memory::SignatureDefinition> inventory_signatures;
                for (const auto& definition : memory::solstice_signatures())
                    if (definition.name == "InventoryTransaction_addAction" || definition.name == "ComplexInventoryTransaction_vtable")
                        inventory_signatures.push_back(definition);
                memory::SignatureRegistry inventory_registry;
                inventory_registry.scan(inventory_signatures, module->executable_regions, module->image);
                state.inventory_add_action = inventory_registry.address("InventoryTransaction_addAction").value_or(0);
                state.inventory_transaction_vtable = inventory_registry.address("ComplexInventoryTransaction_vtable").value_or(0);
                state.inventory_diagnostic.add_action_rva = state.inventory_add_action ? state.inventory_add_action - state.image.address : 0;
                state.inventory_diagnostic.transaction_vtable_rva = state.inventory_transaction_vtable ? state.inventory_transaction_vtable - state.image.address : 0;
                state.inventory_diagnostic.stage = state.inventory_add_action && state.inventory_transaction_vtable
                    ? "solstice-signatures-ready" : "solstice-signatures-unavailable";
                // Actor_getNameTag 1.26.52.3: borrowed MSVC string result.
                // The bounded body reads SynchedActorData +0x128 and returns
                // its type-4 string; no DLL STL object crosses this ABI.
                constexpr std::array<unsigned char, 32> name_prefix{
                    0x56,0x48,0x83,0xEC,0x20,0x48,0x8B,0x81,0x28,0x01,0,0,0x48,0x85,0xC0,0x74,
                    0x4A,0x8B,0x50,0x08,0x48,0x8B,0x08,0x29,0xCA,0x81,0xE2,0xF8,0xFF,0x07,0,0x48};
                const auto name_target = state.image.address + 0x1A14AC0;
                if (state.native_identity(name_target, 0x1A14AC0, name_prefix))
                    state.model_name_getter = reinterpret_cast<const void* (*)(void*)>(name_target);
                state.diagnostic = "Gameplay SDK ready for Minecraft 1.26.52.3; waiting for validated world components.";
            }
        }
    }
    diagnostic = state.diagnostic;
    return state.supported;
}

const std::string& BedrockGameplay::diagnostic() const noexcept { return impl_->diagnostic; }
InventoryDiagnostic BedrockGameplay::inventory_diagnostic() const noexcept { return impl_->inventory_diagnostic; }

void BedrockGameplay::clear() noexcept {
    impl_->server_rotation.store(no_server_rotation, std::memory_order_release);
    impl_->move_fix_sprinting.store(false, std::memory_order_release);
    impl_->diagnostic_local_game_mode.store(0, std::memory_order_release);
    impl_->active = false;
    impl_->switched_this_tick = false;
    impl_->frame = {};
    impl_->client = impl_->player = impl_->registry = impl_->block_source = impl_->view_player = 0;
    impl_->render_position = 0;
    impl_->local_entity = 0;
    impl_->destroy_active = false;
    impl_->destroy_mode = 0;
    impl_->clear_destroy_speed_target();
    impl_->pools.clear();
    impl_->rotation_control_enabled = false;
    impl_->rotation_render_enabled = false;
    impl_->rotation_yaw_valid = false;
    impl_->rotation_override_active = false;
    impl_->rotation_player = impl_->rotation_registry = 0;
    impl_->rotation_entity = 0;
    impl_->head_body_rotation_valid = false;
}

void BedrockGameplay::end_tick() noexcept { impl_->active = false; }

bool BedrockGameplay::slot_restore_pending() const noexcept { return impl_->switch_from >= 0; }

std::uintptr_t BedrockGameplay::motion_dispatcher_target() const noexcept {
    // 1.26.52.3 SetActorMotion dispatcher: borrows shared_ptr at R9, then
    // tail-calls the callback's +0x668 virtual method with Packet* in R8.
    constexpr std::uintptr_t rva = 0x1733000;
    constexpr std::array<unsigned char, 16> prefix{
        0x4C,0x89,0xC1,0x4D,0x8B,0x01,0x48,0x8B,0x01,0x48,0x8B,0x80,0x68,0x06,0,0};
    auto& state = *impl_;
    if (!state.supported || rva > UINTPTR_MAX - state.image.address) return 0;
    const auto target = state.image.address + rva;
    std::uintptr_t table_entry{};
    return state.native_identity(target, rva, prefix) &&
        read_at(state.image.address, 0xE7BF8C8, table_entry) && table_entry == target ? target : 0;
}

std::uintptr_t BedrockGameplay::prediction_correction_dispatcher_target() const noexcept {
    constexpr std::uintptr_t rva = 0x1734000;
    constexpr std::array<unsigned char, 16> prefix{
        0x4C,0x89,0xC1,0x4D,0x8B,0x01,0x48,0x8B,0x01,0x48,0x8B,0x80,0x30,0x02,0,0};
    const auto& state = *impl_;
    std::uintptr_t entry{};
    const auto target = state.image.address + rva;
    return state.supported && state.native_identity(target, rva, prefix) &&
        read_at(state.image.address, 0xE7C0038, entry) && entry == target ? target : 0;
}

std::uintptr_t BedrockGameplay::disconnect_dispatcher_target() const noexcept {
    constexpr std::uintptr_t rva = 0x1723320;
    constexpr std::array<unsigned char, 16> prefix{
        0x4C,0x89,0xC1,0x4D,0x8B,0x01,0x48,0x8B,0x01,0x48,0x8B,0x80,0xE8,0x07,0,0};
    const auto& state = *impl_;
    std::uintptr_t entry{};
    const auto target = state.image.address + rva;
    return state.supported && state.native_identity(target, rva, prefix) &&
        read_at(state.image.address, 0xE7BE680, entry) && entry == target ? target : 0;
}

bool BedrockGameplay::is_client_network_handler(void* handler) const noexcept {
    std::uintptr_t table{};
    return impl_->supported && read_at(reinterpret_cast<std::uintptr_t>(handler), 0, table) &&
        table == impl_->image.address + 0xE7AF110;
}

std::uintptr_t BedrockGameplay::move_player_dispatcher_target() const noexcept {
    constexpr std::uintptr_t rva = 0x1732C20;
    constexpr std::array<unsigned char, 16> prefix{
        0x4C,0x89,0xC1,0x4D,0x8B,0x01,0x48,0x8B,0x01,0x48,0x8B,0x80,0x08,0x07,0,0};
    const auto& state = *impl_;
    std::uintptr_t entry{};
    const auto target = state.image.address + rva;
    return state.supported && state.native_identity(target, rva, prefix) &&
        read_at(state.image.address, 0xE7BF748, entry) && entry == target ? target : 0;
}

bool BedrockGameplay::read_movement_report(void* packet, MovementReport& report) const noexcept {
    const auto& state = *impl_;
    const auto address = reinterpret_cast<std::uintptr_t>(packet);
    std::uintptr_t table{}, get_id{};
    constexpr std::array<unsigned char, 6> prefix{0xB8,0x90,0,0,0,0xC3};
    return state.supported && read_at(address, 0, table) && table == state.image.address + 0xE83A360 &&
        read_at(table, 8, get_id) && state.native_identity(get_id, 0xB26A00, prefix) &&
        detail::read_gameplay_movement_report(address, report);
}

bool BedrockGameplay::read_player_move(void* packet_reference, LocalPlayerMove& move) const noexcept {
    const auto& state = *impl_;
    std::uintptr_t packet{}, table{}, get_id{};
    std::uint64_t runtime_id{};
    constexpr std::array<unsigned char, 6> prefix{0xB8,0x13,0,0,0,0xC3};
    return state.supported && read_at(reinterpret_cast<std::uintptr_t>(packet_reference), 0, packet) &&
        read_at(packet, 0, table) && table == state.image.address + 0xE85BFC0 &&
        read_at(table, 8, get_id) && state.native_identity(get_id, 0x2ECD950, prefix) &&
        read_at(packet, 0x30, runtime_id) &&
        detail::read_gameplay_local_move(packet, runtime_id, move);
}

bool BedrockGameplay::is_local_runtime_id(std::uint64_t runtime_id) const noexcept {
    const auto& state = *impl_;
    std::uint64_t current{};
    return state.supported && state.frame.ready && runtime_id &&
        read_at(state.component(state.local_entity, runtime_id_hash, 8), 0, current) && current == runtime_id;
}

bool BedrockGameplay::local_player_move(void* packet_reference, LocalPlayerMove& move) const noexcept {
    LocalPlayerMove copied;
    if (!read_player_move(packet_reference, copied) || !is_local_runtime_id(copied.runtime_id)) return false;
    move = copied;
    return true;
}

void BedrockGameplay::bind_velocity_trace(VelocityTraceObserver observer, void* context) noexcept {
    impl_->velocity_observer = observer;
    impl_->velocity_observer_context = context;
}

bool BedrockGameplay::local_motion(void* packet_reference, void* local_actor, Vec3& motion,
                                   std::uint64_t* input_tick, std::int64_t* received_timestamp) const noexcept {
    auto& state = *impl_;
    const auto actor = reinterpret_cast<std::uintptr_t>(local_actor);
    std::uint32_t entity{};
    std::uintptr_t packet{}, table{}, get_id{}, owner{};
    std::uint64_t runtime_id{};
    constexpr std::array<unsigned char, 6> get_id_prefix{0xB8,0x28,0,0,0,0xC3};
    if (!state.supported || !state.frame.ready || !state.frame.local.alive || actor != state.player || !state.context(actor, entity) ||
        !read_at(state.component(entity, owner_hash, 8), 0, owner) || owner != actor ||
        !read_at(state.component(entity, runtime_id_hash, 8), 0, runtime_id) ||
        !read_at(reinterpret_cast<std::uintptr_t>(packet_reference), 0, packet) ||
        !read_at(packet, 0, table) || table != state.image.address + 0xE8A73D0 ||
        !read_at(table, 8, get_id) || !state.native_identity(get_id, 0xB270F0, get_id_prefix)) return false;
    Vec3 copied;
    std::uint64_t tick{};
    std::int64_t timestamp{};
    if (!detail::read_gameplay_motion(packet, runtime_id, copied) || (input_tick && !read_at(packet, 0x48, tick)) ||
        (received_timestamp && !read_at(packet, 0x18, timestamp))) return false;
    motion = copied;
    if (input_tick) *input_tick = tick;
    if (received_timestamp) *received_timestamp = timestamp;
    return true;
}

bool BedrockGameplay::write_local_motion(void* packet_reference, void* local_actor, Vec3 motion) const noexcept {
    std::uintptr_t packet{};
    Vec3 current{};
    return local_motion(packet_reference, local_actor, current) &&
        read_at(reinterpret_cast<std::uintptr_t>(packet_reference), 0, packet) &&
        detail::write_gameplay_motion(packet, motion);
}

void BedrockGameplay::bind_latency_sender(std::uintptr_t target, void(*original)(void*, void*)) noexcept {
    if (!impl_->supported || !impl_->executable(target) || !original) return;
    impl_->packet_send_target = target;
    impl_->packet_send_original = original;
}

bool BedrockGameplay::is_latency_reply(void* packet) const noexcept {
    std::uintptr_t table{};
    return impl_->supported && read_at(reinterpret_cast<std::uintptr_t>(packet), 0, table) &&
        table == impl_->image.address + 0xE7B0FD0;
}

bool BedrockGameplay::read_latency_reply(void* sender, void* packet, LatencyReply& reply) const noexcept {
    const auto& state = *impl_;
    std::uintptr_t current_sender{}, table{}, get_id{}, dispatcher{};
    const auto address = reinterpret_cast<std::uintptr_t>(packet);
    constexpr std::array<unsigned char, 6> prefix{0xB8,0x73,0,0,0,0xC3};
    if (!state.supported || !state.frame.ready || !state.packet_send_original ||
        !read_at(state.client, 0x1D0, current_sender) || current_sender != reinterpret_cast<std::uintptr_t>(sender) ||
        state.method(current_sender, 4) != state.packet_send_target ||
        !read_at(address, 0, table) || table != state.image.address + 0xE7B0FD0 ||
        !read_at(table, 8, get_id) || !state.native_identity(get_id, 0x3C40700, prefix) ||
        !read_at(address, 0x20, dispatcher) || dispatcher != state.image.address + 0x1185B5B0) return false;
    return detail::copy_gameplay_latency(address, reply);
}

bool BedrockGameplay::send_latency_reply(const LatencyReply& reply) {
    auto& state = *impl_;
    if (!state.on_tick() || !state.packet_send_original) return false;
    std::uintptr_t sender{};
    LatencyReply checked;
    // Revalidate the native identity and current sender at replay. The
    // original sender borrows Packet* synchronously, bypassing our send hook.
    if (!read_at(state.client, 0x1D0, sender) || !sender ||
        !read_latency_reply(reinterpret_cast<void*>(sender), const_cast<std::byte*>(reply.bytes.data()), checked)) return false;
    std::uint64_t latency_id{};
    std::memcpy(&latency_id, checked.bytes.data() + 0x30, sizeof latency_id);
    state.trace_velocity(VelocityTracePhase::latency_submit, "sendToServer", KnockbackApplyResult::direct, latency_id);
    state.packet_send_original(reinterpret_cast<void*>(sender), checked.bytes.data());
    return true;
}

bool BedrockGameplay::refresh_view(GameplayFrame& frame) const noexcept {
    const auto& state = *impl_;
    if (!state.supported || !frame.ready || !frame.projection_ready) return false;
    state.read_render_position(frame.local);
    Vec3 camera{};
    Vec2 fov{};
    std::array<float, 16> matrix{};
    if (!state.read_view(state.view_player, camera, fov, matrix)) return false;
    frame.camera = camera;
    frame.fov = fov;
    frame.view_matrix = matrix;
    return true;
}
const GameplayFrame& BedrockGameplay::frame() const noexcept { return impl_->frame; }

void BedrockGameplay::begin_rotation_control(bool enabled) noexcept {
    begin_rotation_control(enabled, enabled);
}

void BedrockGameplay::begin_rotation_control(bool enabled, bool render_enabled) noexcept {
    auto& state = *impl_;
    state.server_rotation.store(no_server_rotation, std::memory_order_release);
    // Restore the current camera yaw before dropping an active head/body
    // override (for example when Killaura or Rotations is turned off).
    if (!enabled && state.rotation_control_enabled && state.rotation_override_active &&
        state.rotation_yaw_valid && state.rotation_player == state.player &&
        state.rotation_registry == state.registry && state.rotation_entity == state.local_entity &&
        state.frame.ready && std::isfinite(state.frame.yaw)) {
        (void)rotate_head_and_body(state.frame.yaw, state.frame.pitch);
    }
    state.rotation_control_enabled = enabled;
    state.rotation_render_enabled = enabled && render_enabled;
    if (!enabled) {
        state.rotation_yaw_valid = false;
        state.rotation_override_active = false;
        state.rotation_yaw = state.rotation_previous_yaw = state.rotation_pitch = state.rotation_previous_pitch = 0;
        state.rotation_pitch_serial = 0;
        return;
    }

    const bool same_player = state.rotation_yaw_valid &&
        state.rotation_player == state.player && state.rotation_registry == state.registry &&
        state.rotation_entity == state.local_entity;
    if (!state.on_tick()) return;
    if (same_player) {
        state.frame.head_yaw = state.rotation_yaw;
        state.frame.head_yaw_ready = true;
        return;
    }

    state.rotation_override_active = false;
    std::array<float, 2> current_head{};
    const auto head = state.component(state.local_entity, head_hash, sizeof(current_head));
    if (!read_at(head, 0, current_head) ||
        !std::ranges::all_of(current_head, [](float value) {
            return std::isfinite(value) && std::abs(value) <= 360000;
        })) {
        state.rotation_yaw_valid = false;
        return;
    }
    state.rotation_yaw = std::remainder(current_head[0], 360.0F);
    state.rotation_previous_yaw = state.rotation_yaw;
    state.frame.head_yaw = state.rotation_yaw;
    state.frame.head_yaw_ready = true;
    state.rotation_player = state.player;
    state.rotation_registry = state.registry;
    state.rotation_entity = state.local_entity;
    state.rotation_yaw_valid = true;
    // A newly enabled rotation controller must start pitch interpolation at
    // the captured view angle, not the zeroed state left by the previous off
    // transition. The first aim update will then move smoothly from here.
    state.rotation_pitch = std::clamp(state.frame.pitch, -90.0F, 90.0F);
    state.rotation_previous_pitch = state.rotation_pitch;
    state.rotation_pitch_serial = 0;
}

void BedrockGameplay::finish_rotation_control() noexcept {
    auto& state = *impl_;
    if (!state.rotation_control_enabled || !state.rotation_yaw_valid || !state.rotation_override_active ||
        state.rotation_player != state.player || state.rotation_registry != state.registry ||
        state.rotation_entity != state.local_entity || !state.on_tick()) return;
    (void)rotate_head_and_body(state.rotation_yaw, state.rotation_pitch);
}

void BedrockGameplay::finish_rotation_control(bool rotation_requested) noexcept {
    finish_rotation_control();
    auto& state = *impl_;
    if (!rotation_requested || !state.rotation_control_enabled || !state.rotation_yaw_valid ||
        !state.rotation_override_active || state.rotation_player != state.player ||
        state.rotation_registry != state.registry || state.rotation_entity != state.local_entity || !state.on_tick()) {
        state.server_rotation.store(no_server_rotation, std::memory_order_release);
        return;
    }
    const auto angles = static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(state.rotation_yaw)) |
        (static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(state.rotation_pitch)) << 32);
    state.server_rotation.store(angles, std::memory_order_release);
}

std::uintptr_t BedrockGameplay::body_yaw_interpolator_target() const noexcept {
    constexpr std::uintptr_t body_yaw_interpolator_rva = 0x20874B0;
    constexpr std::array<unsigned char, 32> body_yaw_prefix{
        0x48,0x83,0xEC,0x58,0x44,0x0F,0x29,0x44,0x24,0x40,0x0F,0x29,0x7C,0x24,0x30,0x0F,
        0x29,0x74,0x24,0x20,0x0F,0x28,0xF1,0x48,0x8B,0x51,0x10,0x8B,0x41,0x18,0x48,0x8B
    };
    auto& state = *impl_;
    if (!state.supported || body_yaw_interpolator_rva > UINTPTR_MAX - state.image.address)
        return 0;
    if (state.body_yaw_interpolator) return state.body_yaw_interpolator;
    const auto target = state.image.address + body_yaw_interpolator_rva;
    if (!state.native_identity(target, body_yaw_interpolator_rva, body_yaw_prefix)) return 0;
    state.body_yaw_interpolator = target;
    return target;
}

std::uintptr_t BedrockGameplay::head_yaw_interpolator_target() const noexcept {
    constexpr std::uintptr_t head_yaw_interpolator_rva = 0x2087610;
    constexpr std::array<unsigned char, 61> head_yaw_prefix{
        0x48,0x83,0xEC,0x68,0x44,0x0F,0x29,0x4C,0x24,0x50,0x44,0x0F,0x29,0x44,0x24,0x40,
        0x0F,0x29,0x7C,0x24,0x30,0x0F,0x29,0x74,0x24,0x20,0x0F,0x28,0xF1,0x48,0x8B,0x51,
        0x10,0x8B,0x41,0x18,0x48,0x8B,0x4A,0x48,0x4C,0x8B,0x42,0x50,0x49,0x29,0xC8,0x49,
        0xC1,0xE8,0x03,0x41,0xFF,0xC8,0x41,0x81,0xE0,0x11,0x72,0xBE,0xBA
    };
    auto& state = *impl_;
    if (!state.supported || head_yaw_interpolator_rva > UINTPTR_MAX - state.image.address)
        return 0;
    if (state.head_yaw_interpolator) return state.head_yaw_interpolator;
    const auto target = state.image.address + head_yaw_interpolator_rva;
    if (!state.native_identity(target, head_yaw_interpolator_rva, head_yaw_prefix)) return 0;
    state.head_yaw_interpolator = target;
    return target;
}

std::uintptr_t BedrockGameplay::destroy_progress_target() const noexcept {
    constexpr std::uintptr_t destroy_progress_rva = 0x259BA80;
    constexpr std::array<unsigned char, 16> progress_prefix{
        0x56,0x57,0x48,0x83,0xEC,0x38,0x0F,0x29,0x74,0x24,0x20,0x48,0x89,0xCE,0x48,0x8B
    };
    const auto& state = *impl_;
    if (!state.supported || destroy_progress_rva > UINTPTR_MAX - state.image.address) return 0;
    const auto target = state.image.address + destroy_progress_rva;
    return state.native_identity(target, destroy_progress_rva, progress_prefix) ? target : 0;
}

std::uintptr_t BedrockGameplay::destroy_stop_target() const noexcept {
    constexpr std::uintptr_t destroy_stop_rva = 0x259BBF0;
    constexpr std::array<unsigned char, 16> stop_prefix{
        0x56,0x48,0x83,0xEC,0x20,0x48,0x89,0xCE,0x48,0x8B,0x89,0xC0,0x00,0x00,0x00,0xF3
    };
    const auto& state = *impl_;
    if (!state.supported || destroy_stop_rva > UINTPTR_MAX - state.image.address) return 0;
    const auto target = state.image.address + destroy_stop_rva;
    return state.native_identity(target, destroy_stop_rva, stop_prefix) ? target : 0;
}

float BedrockGameplay::scale_destroy_progress(std::uintptr_t game_mode, float progress) const noexcept {
    const auto& state = *impl_;
    if (!state.destroy_speed_active.load(std::memory_order_acquire) ||
        !std::isfinite(progress) || progress <= 0.0F) return progress;
    BlockPos target{};
    std::uint8_t face{};
    if (!read_at(game_mode, 0x10, target) || !read_at(game_mode, 0x1C, face) ||
        Impl::block_xz_key(target) != state.destroy_speed_target_xz.load(std::memory_order_relaxed) ||
        target.y != state.destroy_speed_target_y.load(std::memory_order_relaxed) ||
        face != state.destroy_speed_target_face.load(std::memory_order_relaxed)) return progress;
    // In an integrated world the authoritative GameMode belongs to the local
    // server player, while BedrockGameplay::advance_destroy_block drives the
    // client GameMode. Capture the authoritative accumulator so Regen's UI and
    // rotation threshold follow the same progress that causes the block update.
    const auto local_mode = state.diagnostic_local_game_mode.load(std::memory_order_acquire);
    if (game_mode != local_mode) {
        float accumulated{};
        if (read_at(game_mode, 0x24, accumulated) && std::isfinite(accumulated)) {
            accumulated = std::clamp(accumulated, 0.0F, 1.0F);
            auto previous = state.destroy_speed_server_progress.load(std::memory_order_relaxed);
            while (accumulated > previous && !state.destroy_speed_server_progress.compare_exchange_weak(
                previous, accumulated, std::memory_order_release, std::memory_order_relaxed)) {}
        }
    }
    const float scale = std::clamp(state.destroy_speed_scale.load(std::memory_order_relaxed), 0.01F, 1.0F);
    return progress / scale;
}

bool BedrockGameplay::should_defer_destroy_stop(std::uintptr_t game_mode, const BlockPos* position) const noexcept {
    const auto& state = *impl_;
    if (!position || !state.destroy_speed_active.load(std::memory_order_acquire)) return false;
    BlockPos current{};
    if (!read_at(game_mode, 0x10, current)) return false;
    const auto target_xz = state.destroy_speed_target_xz.load(std::memory_order_relaxed);
    return Impl::block_xz_key(*position) == target_xz && position->y == state.destroy_speed_target_y.load(std::memory_order_relaxed) &&
        Impl::block_xz_key(current) == target_xz && current.y == position->y;
}

bool BedrockGameplay::read_outgoing_block_actions(void* packet, OutgoingBlockActionSample& sample) const noexcept {
    sample = {};
    const auto& state = *impl_;
    if (!state.supported || !packet) return false;
    constexpr std::uintptr_t packet_vtable_rva = 0xE83A360;
    constexpr std::size_t packet_tick_offset = 0xA8;
    constexpr std::size_t input_data_offset = 0x88;
    constexpr std::size_t block_actions_offset = 0xC0;
    constexpr std::uint32_t perform_block_actions_bit = 35;
    const auto address = reinterpret_cast<std::uintptr_t>(packet);
    const auto get_id = state.method(address, 1);
    std::uintptr_t vtable{};
    if (!get_id || !read_at(address, 0, vtable)) return false;
    using GetId = int(__fastcall*)(void*);
    if (reinterpret_cast<GetId>(get_id)(packet) != 144) return false;
    sample.recognized = true;
    sample.vtable_rva = state.image.contains(vtable) ? vtable - state.image.address : 0;
    sample.validation_stage = 1; // packet ID recognized
    if (sample.vtable_rva != packet_vtable_rva) {
        sample.validation_stage = 2; // packet vtable profile mismatch
        return false;
    }
    sample.validation_stage = 3; // packet profile validated
    if (!read_at(address, packet_tick_offset, sample.client_tick)) return false;

    std::array<std::uint64_t, 2> input_flags{};
    if (!read_at(address, input_data_offset, input_flags) ||
        (input_flags[1] & ~std::uint64_t{0x3}) != 0) {
        sample.validation_stage = 4; // InputData layout rejected
        return false;
    }
    sample.perform_block_actions =
        (input_flags[perform_block_actions_bit / 64] & (std::uint64_t{1} << (perform_block_actions_bit % 64))) != 0;

    ForeignVector actions{};
    constexpr std::size_t action_stride = 20; // type(4), BlockPos(12), face(4)
    if (!vector_at(address, block_actions_offset, actions, action_stride, 32)) {
        sample.validation_stage = 5; // block-action vector layout rejected
        return false;
    }
    const auto total = (actions.end - actions.begin) / action_stride;
    sample.count = static_cast<std::uint32_t>(total);
    const auto captured = std::min(total, sample.actions.size());
    for (std::size_t i = 0; i < captured; ++i) {
        auto& action = sample.actions[i];
        const auto item = actions.begin + i * action_stride;
        if (!copy_bytes(item, &action.type, sizeof(action.type)) ||
            !copy_bytes(item + 4, &action.position, sizeof(action.position)) ||
            !copy_bytes(item + 16, &action.face, sizeof(action.face)) ||
            action.type < 0 || action.type > 39 || action.face < -1 || action.face > 5 ||
            std::abs(static_cast<double>(action.position.x)) > 32000000 ||
            std::abs(static_cast<double>(action.position.z)) > 32000000 ||
            action.position.y < -4096 || action.position.y > 4096) {
            sample.validation_stage = 6; // action entry validation failed
            return false;
        }
    }
    sample.valid = true;
    sample.validation_stage = 7;
    return true;
}

DestroyBlockDiagnostic BedrockGameplay::destroy_block_diagnostic() const noexcept {
    const auto& state = *impl_;
    DestroyBlockDiagnostic diagnostic{};
    diagnostic.stage = state.destroy_diagnostic_stage;
    diagnostic.profile_stage = state.destroy_diagnostic_profile_stage;
    diagnostic.continue_return = state.destroy_diagnostic_continue_return;
    diagnostic.native_destroyed = state.destroy_diagnostic_native_destroyed;
    diagnostic.active = state.destroy_active;
    diagnostic.progress = state.destroy_diagnostic_progress;
    diagnostic.position = state.destroy_position;
    diagnostic.face = state.destroy_face;
    diagnostic.requested_mode = state.destroy_diagnostic_requested_mode;
    diagnostic.requested_owner = state.destroy_diagnostic_requested_owner;
    diagnostic.requested_vtable_rva = state.destroy_diagnostic_requested_vtable_rva;
    diagnostic.requested_owner_matches = state.destroy_diagnostic_requested_owner_matches;
    diagnostic.requested_position = state.destroy_diagnostic_requested_position;
    diagnostic.requested_face = state.destroy_diagnostic_requested_face;
    if (state.active) {
        if (const auto block = state.block(state.destroy_position)) {
            diagnostic.block_readable = true;
            diagnostic.block_id = block->id;
        }
    }
    return diagnostic;
}

DestroyModeDiagnostic BedrockGameplay::destroy_mode_diagnostic(std::uintptr_t game_mode) const noexcept {
    DestroyModeDiagnostic diagnostic{};
    const auto& state = *impl_;
    std::uintptr_t table{};
    if (!game_mode || !read_at(game_mode, 0, table) || !read_at(game_mode, 8, diagnostic.owner) ||
        !read_at(game_mode, 0x10, diagnostic.position) || !read_at(game_mode, 0x1C, diagnostic.face) ||
        !read_at(game_mode, 0x20, diagnostic.previous_progress) ||
        !read_at(game_mode, 0x24, diagnostic.accumulated_progress) || diagnostic.face > 5 ||
        !std::isfinite(diagnostic.previous_progress) || !std::isfinite(diagnostic.accumulated_progress))
        return diagnostic;
    diagnostic.vtable_rva = state.image.contains(table) ? table - state.image.address : 0;
    diagnostic.local_mode = state.diagnostic_local_game_mode.load(std::memory_order_acquire) == game_mode;
    diagnostic.readable = true;
    return diagnostic;
}

bool BedrockGameplay::validates_body_yaw_interpolator(void* actor) const noexcept {
    if (!actor) return false;
    const auto target = body_yaw_interpolator_target();
    return target && impl_->has_method(reinterpret_cast<std::uintptr_t>(actor), target);
}

bool BedrockGameplay::validates_head_yaw_interpolator(void* actor) const noexcept {
    if (!actor) return false;
    const auto target = head_yaw_interpolator_target();
    return target && impl_->has_method(reinterpret_cast<std::uintptr_t>(actor), target);
}

std::string BedrockGameplay::rotation_getter_diagnostic(void* actor) const {
    constexpr std::size_t method_count = 32;
    std::uintptr_t table = 0;
    std::array<std::uintptr_t, method_count> methods{};
    const auto actor_address = reinterpret_cast<std::uintptr_t>(actor);
    if (!actor || actor_address % 8 || !read_at(actor_address, 0, table) ||
        !impl_->image.contains(table, sizeof(methods)) || !copy_bytes(table, methods.data(), sizeof(methods)))
        return "Rotation vtable diagnostic unavailable.";

    const auto find_index = [&](std::uintptr_t target) {
        const auto found = std::ranges::find(methods, target);
        return found == methods.end() ? -1 : static_cast<int>(found - methods.begin());
    };
    const auto body_index = find_index(body_yaw_interpolator_target());
    const auto head_index = find_index(head_yaw_interpolator_target());
    char text[256]{};
    std::snprintf(text, sizeof(text),
        "Rotation vtable: table=0x%llX slot13=0x%llX body_index=%d slot14=0x%llX head_index=%d",
        static_cast<unsigned long long>(table),
        static_cast<unsigned long long>(methods[13]), body_index,
        static_cast<unsigned long long>(methods[14]), head_index);
    return text;
}

bool BedrockGameplay::rotation_override(float& previous_yaw, float& yaw) const noexcept {
    const auto& state = *impl_;
    if (!state.rotation_control_enabled || !state.rotation_render_enabled || !state.rotation_override_active ||
        !state.rotation_yaw_valid || !std::isfinite(state.rotation_yaw) ||
        !std::isfinite(state.rotation_previous_yaw))
        return false;
    previous_yaw = state.rotation_previous_yaw;
    yaw = state.rotation_yaw;
    return true;
}

bool BedrockGameplay::rotation_diagnostic(RotationDiagnostic& diagnostic) const noexcept {
    const auto& state = *impl_;
    if (!state.rotation_render_enabled || !state.rotation_yaw_valid || !state.rotation_override_active ||
        state.rotation_player != state.player || state.rotation_registry != state.registry ||
        state.rotation_entity != state.local_entity) return false;
    const auto rotation = state.component(state.local_entity, rotation_hash, 16);
    const auto render_rotation = state.component(state.local_entity, render_rotation_hash, 8);
    std::array<float, 4> view{};
    std::array<float, 2> render{};
    if (!read_at(rotation, 0, view) || !read_at(render_rotation, 0, render)) return false;
    diagnostic = {state.rotation_pitch, state.rotation_previous_pitch, view[0], view[2], render[0], render[1],
        render_rotation, rotation};
    return std::ranges::all_of(std::array<float, 6>{diagnostic.requested_pitch, diagnostic.previous_pitch,
        diagnostic.view_pitch, diagnostic.view_previous_pitch, diagnostic.render_pitch, diagnostic.render_yaw},
        [](float value) { return std::isfinite(value); });
}

bool BedrockGameplay::capture(void* client, void* local_actor, bool check_visibility) {
    auto& state = *impl_;
    state.active = false;
    state.switched_this_tick = false;
    state.frame = {};
    state.client = state.player = state.registry = state.block_source = state.view_player = 0;
    state.render_position = 0;
    state.local_entity = 0;
    state.pools.clear();
    if (++state.capture_serial == 0) ++state.capture_serial;
    if (!state.supported || !client || !local_actor) return false;
    state.thread = GetCurrentThreadId();
    state.client = reinterpret_cast<std::uintptr_t>(client);
    state.player = reinterpret_cast<std::uintptr_t>(local_actor);
    std::uintptr_t sampled_game_mode{};
    (void)read_at(state.player, 0xAA0, sampled_game_mode);
    state.diagnostic_local_game_mode.store(sampled_game_mode, std::memory_order_release);
    const auto local_target = state.method(state.client, 31);
    if (!local_target || reinterpret_cast<void*(__fastcall*)(void*)>(local_target)(client) != local_actor ||
        !read_at(state.player, 0x10, state.registry) || !read_at(state.player, 0x18, state.local_entity)) {
        state.diagnostic = "Gameplay SDK: current client/player context failed validation.";
        clear();
        return false;
    }
    // Slot cleanup needs only the current client/player and inventory. Do it
    // before ECS, rotation or camera reads can withhold the world snapshot.
    state.restore_switched_slot();
    if (!detail::read_gameplay_pools(state.registry, state.pools)) {
        state.diagnostic = "Gameplay SDK: unsupported EnTT registry layout or required component hashes missing.";
        clear();
        return false;
    }
    state.resolve_health_attribute();
    if (!state.snapshot(state.player, state.frame.local) || !state.frame.local.player || !state.frame.local.alive) {
        state.diagnostic = "Gameplay SDK: waiting for a living player with valid owner, state and bounds components.";
        clear();
        return false;
    }
    if (state.head_body_rotation_valid &&
        (state.head_body_rotation_player != state.player ||
         state.head_body_rotation_registry != state.registry ||
         state.head_body_rotation_entity != state.local_entity))
        state.head_body_rotation_valid = false;
    state.frame.world_identity = state.registry;
    // Verified on 1.26.52.3: 12-byte elements (lea [i+i*2], shl 2 at 0x28A56C3).
    state.render_position = state.component(state.local_entity, render_position_hash, sizeof(Vec3));
    state.read_render_position(state.frame.local);
    struct Rotation { float pitch, yaw, old_pitch, old_yaw; } rotation{};
    if (!read_at(state.component(state.local_entity, rotation_hash, sizeof(rotation)), 0, rotation) ||
        !std::isfinite(rotation.pitch) || !std::isfinite(rotation.yaw) || std::abs(rotation.pitch) > 180 ||
        std::abs(rotation.yaw) > 360000) {
        state.diagnostic = "Gameplay SDK: local rotation component failed validation.";
        clear();
        return false;
    }
    state.frame.ready = true;
    state.frame.eye = state.frame.local.position;
    const auto& local_bounds = state.frame.local.bounds;
    const float local_height = local_bounds.max.y - local_bounds.min.y;
    if (std::isfinite(local_height) && local_height > 0.01F) {
        // StateVectorComponent.position is the actor's feet origin. Aim angles
        // and visibility rays need the player's eye origin; using the feet
        // makes Killaura pitch point well above the target at close range.
        state.frame.eye.y = local_bounds.min.y + std::min(1.62F, local_height * 0.9F);
    }
    state.frame.camera = state.frame.eye;
    state.frame.yaw = rotation.yaw;
    state.frame.pitch = rotation.pitch;
    std::uint64_t actor_flags{};
    const auto actor_data_flags = state.component(state.local_entity, actor_data_flag_hash, detail::actor_data_flags_size);
    if (read_at(actor_data_flags, 0, actor_flags))
        state.frame.sprinting = (actor_flags & (std::uint64_t{1} << 3)) != 0;
    detail::GameplayMoveInput movement{};
    state.frame.move_input_ready = detail::read_gameplay_move_input(
        state.component(state.local_entity, move_input_hash, detail::move_input_size), movement);
    state.frame.move_input = {movement.strafe, movement.forward};
    state.frame.sneaking = movement.sneaking;
    std::array<float, 2> head_rotation{};
    const auto head = state.component(state.local_entity, head_hash, sizeof(head_rotation));
    if (read_at(head, 0, head_rotation) && std::isfinite(head_rotation[0]) &&
        std::abs(head_rotation[0]) <= 360000) {
        state.frame.head_yaw = std::remainder(head_rotation[0], 360.0F);
        state.frame.head_yaw_ready = true;
    } else {
        state.frame.head_yaw = state.frame.yaw;
        state.frame.head_yaw_ready = false;
    }
    state.frame.attack_down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const auto get_source = state.method(state.client, 30);
    if (get_source) state.block_source = reinterpret_cast<std::uintptr_t(__fastcall*)(void*)>(get_source)(client);
    state.projection();
    ForeignVector owners{};
    const auto owner_pool = state.storage(owner_hash);
    if (vector_at(owner_pool, 0x20, owners, 4, maximum_entities)) {
        const auto count = std::min<std::size_t>((owners.end - owners.begin) / 4, 4096);
        state.frame.actors.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            std::uint32_t entity = 0;
            std::uintptr_t actor = 0;
            ActorSnapshot snapshot{};
            if (!read_at(owners.begin, i * 4, entity) ||
                !read_at(state.component(entity, owner_hash, 8), 0, actor) || actor == state.player ||
                !state.snapshot(actor, snapshot) || distance_squared(snapshot.position, state.frame.local.position) > 512 * 512)
                continue;
            const Vec3 target{(snapshot.bounds.min.x + snapshot.bounds.max.x) * 0.5F,
                              snapshot.bounds.min.y + (snapshot.bounds.max.y - snapshot.bounds.min.y) * 0.75F,
                             (snapshot.bounds.min.z + snapshot.bounds.max.z) * 0.5F};
            // Gameplay targets are close; distant ESP actors need no costly
            // block walk and remain available in the copied actor list.
            if (check_visibility && snapshot.alive && distance_squared(target, state.frame.eye) <= 12 * 12)
                snapshot.visible = state.clear_ray(target);
            state.frame.actors.push_back(snapshot);
        }
    }
    state.frame.local.visible = true;
    state.active = true;
    state.diagnostic = state.frame.projection_ready ? "Gameplay SDK: world snapshot and camera ready." :
        "Gameplay SDK: world snapshot ready; camera profile failed validation, visual projection withheld.";
    return true;
}

bool BedrockGameplay::rotate(float yaw, float pitch) {
    if (!std::isfinite(pitch)) return false;
    return rotate_head_and_body(yaw, pitch);
}

bool BedrockGameplay::rotate_head_and_body(float yaw) {
    return rotate_head_and_body(yaw, impl_->frame.pitch);
}

bool BedrockGameplay::rotate_head_and_body(float yaw, float pitch) {
    auto& state = *impl_;
    if (!state.rotation_control_enabled || !state.on_tick() || !std::isfinite(yaw) || !std::isfinite(pitch)) return false;
    yaw = std::remainder(yaw, 360.0F);
    pitch = std::clamp(pitch, -90.0F, 90.0F);
    const auto rotation = state.component(state.local_entity, rotation_hash, 16);
    std::array<float, 4> old_rotation{};
    if (!rotation || !read_at(rotation, 0, old_rotation) ||
        !std::ranges::all_of(old_rotation, [](float value) { return std::isfinite(value) && std::abs(value) <= 360000; }))
        return false;
    const auto render_rotation = state.component(state.local_entity, render_rotation_hash, 8);
    std::array<float, 2> old_render_rotation{};
    if (!render_rotation || !read_at(render_rotation, 0, old_render_rotation) ||
        !std::ranges::all_of(old_render_rotation, [](float value) { return std::isfinite(value) && std::abs(value) <= 360000; }))
        return false;
    const auto head = state.component(state.local_entity, head_hash, 8);
    const auto body = state.component(state.local_entity, body_hash, 8);
    if (!head || !body) return false;
    std::array<float, 2> old_head{}, old_body{};
    if (!read_at(head, 0, old_head) || !read_at(body, 0, old_body)) return false;
    if (!std::ranges::all_of(old_head, [](float value) { return std::isfinite(value) && std::abs(value) <= 360000; }) ||
        !std::ranges::all_of(old_body, [](float value) { return std::isfinite(value) && std::abs(value) <= 360000; }))
        return false;
    const bool consecutive = state.head_body_rotation_valid &&
        state.head_body_rotation_player == state.player &&
        state.head_body_rotation_registry == state.registry &&
        state.head_body_rotation_entity == state.local_entity &&
        state.capture_serial != 0 && state.head_body_rotation_serial == state.capture_serial - 1;
    // The native tick may restore these components from view rotation; keep our
    // own last rendered angles as the interpolation origin while active.
    const float previous_head = consecutive ? state.last_head_yaw : old_head[0];
    const float previous_body = consecutive ? state.last_body_yaw : old_body[0];
    const std::array<float, 2> updated_head{yaw, previous_head};
    const std::array<float, 2> updated_body{yaw, previous_body};
    if (!write_at(head, 0, updated_head)) return false;
    const bool same_rotation_actor = state.rotation_yaw_valid &&
        state.rotation_player == state.player && state.rotation_registry == state.registry &&
        state.rotation_entity == state.local_entity;
    const float previous_pitch = same_rotation_actor
        ? (state.rotation_pitch_serial == state.capture_serial ? state.rotation_previous_pitch : state.rotation_pitch)
        : old_rotation[0];
    // Reset the model pitch to the actual camera angle on the game tick. The
    // render callback applies the requested pitch only in third person; this
    // keeps first-person hands and the camera on the user's real view angle.
    const std::array<float, 2> updated_render_rotation{old_rotation[0], old_render_rotation[1]};
    if (write_at(body, 0, updated_body) && write_at(render_rotation, 0, updated_render_rotation)) {
        state.head_body_rotation_player = state.player;
        state.head_body_rotation_registry = state.registry;
        state.head_body_rotation_entity = state.local_entity;
        state.last_head_yaw = yaw;
        state.last_body_yaw = yaw;
        state.head_body_rotation_serial = state.capture_serial;
        state.head_body_rotation_valid = true;
        state.rotation_previous_yaw = std::remainder(previous_head, 360.0F);
        state.rotation_yaw = yaw;
        state.rotation_previous_pitch = previous_pitch;
        state.rotation_pitch = pitch;
        state.rotation_pitch_serial = state.capture_serial;
        state.rotation_player = state.player;
        state.rotation_registry = state.registry;
        state.rotation_entity = state.local_entity;
        state.rotation_yaw_valid = true;
        state.rotation_override_active = true;
        state.frame.head_yaw = yaw;
        state.frame.head_yaw_ready = true;
        return true;
    }
    (void)write_at(head, 0, old_head);
    (void)write_at(body, 0, old_body);
    (void)write_at(render_rotation, 0, old_render_rotation);
    return false;
}

bool BedrockGameplay::attack(std::uintptr_t actor_id) {
    auto& state = *impl_;
    if (!state.on_tick() || !actor_id || actor_id == state.player) return false;
    const auto it = std::ranges::find(state.frame.actors, actor_id, &ActorSnapshot::id);
    ActorSnapshot target{};
    // Revalidate a current frame member against its registry owner and current
    // bounds immediately before the native call; stale world handles fail.
    if (it == state.frame.actors.end() || !state.snapshot(actor_id, target) || !target.alive ||
        distance_squared(target.position, state.frame.local.position) > 12 * 12) return false;
    const auto mode = state.game_mode();
    const auto function = state.method(mode, 15);
    if (!function) return false;
    const Vec3 position{(target.bounds.min.x + target.bounds.max.x) * 0.5F,
                        (target.bounds.min.y + target.bounds.max.y) * 0.5F,
                        (target.bounds.min.z + target.bounds.max.z) * 0.5F};
    using Attack = bool(__fastcall*)(void*, void*, const Vec3*);
    return reinterpret_cast<Attack>(function)(reinterpret_cast<void*>(mode),
        reinterpret_cast<void*>(actor_id), &position);
}

bool BedrockGameplay::set_sprinting(bool sprinting) {
    auto& state = *impl_;
    if (!state.on_tick()) return false;
    const bool input_written = detail::write_gameplay_sprint_input(
        state.component(state.local_entity, move_input_hash, detail::move_input_size), sprinting);
    if (!sprinting) {
        const bool flag_cleared = detail::write_gameplay_sprinting_flag(
            state.component(state.local_entity, actor_data_flag_hash, detail::actor_data_flags_size), false);
        return input_written || flag_cleared;
    }
    return input_written;
}

bool BedrockGameplay::prepare_sprinting(bool sprinting) noexcept {
    auto& state = *impl_;
    if (!state.supported || !state.player || GetCurrentThreadId() != state.thread) return false;
    const auto move_input = state.component(state.local_entity, move_input_hash, detail::move_input_size);
    const bool input_written = detail::write_gameplay_sprint_input(move_input, sprinting);
    if (!sprinting) {
        const bool flag_cleared = detail::write_gameplay_sprinting_flag(
            state.component(state.local_entity, actor_data_flag_hash, detail::actor_data_flags_size), false);
        return input_written || flag_cleared;
    }
    return input_written;
}

bool BedrockGameplay::set_velocity(Vec3 velocity) {
    auto& state = *impl_;
    std::uintptr_t owner{};
    if (!state.on_tick() || !state.frame.local.alive ||
        !read_at(state.component(state.local_entity, owner_hash, 8), 0, owner) || owner != state.player ||
        !detail::write_gameplay_velocity(state.component(state.local_entity, state_hash, 36), velocity)) return false;
    state.frame.local.velocity = velocity;
    return true;
}

KnockbackApplyResult BedrockGameplay::apply_knockback(Vec3 velocity, std::int64_t received_timestamp) {
    auto& state = *impl_;
    std::uintptr_t owner{};
    if (!state.on_tick() || !state.frame.local.alive || !finite(velocity) ||
        !read_at(state.component(state.local_entity, owner_hash, 8), 0, owner) || owner != state.player)
        return KnockbackApplyResult::failed;
    auto result = KnockbackApplyResult::failed;
    const char* path = "validate";
    state.trace_velocity(VelocityTracePhase::apply_begin, path, result);
    struct TraceEnd {
        Impl& state;
        KnockbackApplyResult& result;
        const char*& path;
        ~TraceEnd() { state.trace_velocity(VelocityTracePhase::apply_end, path, result); }
    } trace{state, result, path};
    const auto component = state.component(state.local_entity, predicted_movement_hash, 0x90);
    path = component ? "direct:bypassed" : "direct:no-component";
    std::uint8_t bypass{};
    if (component) {
        if (!read_at(component, 0, bypass)) return KnockbackApplyResult::failed;
        if (!bypass) {
            constexpr std::array<unsigned char, 16> enabled_prefix{
                0x55,0x56,0x57,0x53,0x48,0x83,0xEC,0x58,0x48,0x8D,0x6C,0x24,0x50,0x48,0xC7,0x45};
            const auto enabled = state.image.address + 0x1040080;
            if (!state.native_identity(enabled, 0x1040080, enabled_prefix)) return KnockbackApplyResult::failed;
            // Same no-argument feature gate used by the native motion handler.
            path = "direct:feature-off";
            if (reinterpret_cast<bool(*)()>(enabled)()) {
                path = "predicted";
                constexpr std::array<unsigned char, 16> enqueue_prefix{
                    0x55,0x41,0x57,0x41,0x56,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xEC,0x90,0,0};
                constexpr std::array<unsigned char, 16> update_prefix{
                    0x56,0x57,0x48,0x83,0xEC,0x28,0x48,0x8B,0x41,0x30,0x4C,0x8B,0x40,0x20,0x49,0x83};
                const auto enqueue = state.image.address + 0x2888190;
                const auto update = state.image.address + 0x7CAEA60;
                std::uintptr_t controller{};
                std::array<std::byte, 0x40> probe{};
                if (!state.native_identity(enqueue, 0x2888190, enqueue_prefix) ||
                    !state.native_identity(update, 0x7CAEA60, update_prefix) ||
                    !read_at(component, 0x30, controller) || !controller || controller % 8 ||
                    !copy_bytes(controller, probe.data(), probe.size()))
                    return KnockbackApplyResult::failed;
                if (!detail::enqueue_gameplay_knockback(reinterpret_cast<void*>(component),
                    reinterpret_cast<void*>(controller), velocity, received_timestamp,
                    reinterpret_cast<detail::EnqueuePredictedMotion>(enqueue),
                    reinterpret_cast<detail::UpdatePredictedMotion>(update))) return KnockbackApplyResult::failed;
                // Queue admission can leave the current StateVector unchanged
                // until the next native simulation step. Report actual memory.
                Vec3 current{};
                const auto vector = state.component(state.local_entity, state_hash, 36);
                if (read_at(vector, 24, current) && finite(current)) state.frame.local.velocity = current;
                return result = KnockbackApplyResult::predicted;
            }
        }
    }
    return result = set_velocity(velocity) ? KnockbackApplyResult::direct : KnockbackApplyResult::failed;
}

SprintInputRewrite BedrockGameplay::rewrite_sprint_input(
    void* packet, bool override_enabled, bool sprinting) noexcept {
    auto& state = *impl_;
    if (!state.supported || !packet) return SprintInputRewrite::ignored;
    const bool was_overriding = state.sprint_packet_override.load(std::memory_order_acquire);
    if (!override_enabled && !was_overriding) return SprintInputRewrite::ignored;
    const auto packet_address = reinterpret_cast<std::uintptr_t>(packet);
    const auto get_id = state.method(packet_address, 1);
    if (!get_id) return SprintInputRewrite::ignored;
    using GetId = int(__fastcall*)(void*);
    if (reinterpret_cast<GetId>(get_id)(packet) != player_auth_input_packet_id)
        return SprintInputRewrite::ignored;

    std::array<std::uint64_t, 2> input_flags{};
    if (!read_at(packet_address, player_auth_input_flags_offset, input_flags) ||
        // This profile has 66 input bits. Bits 66..127 must stay clear; that
        // rejects unrelated packets and unknown layouts before any write.
        (input_flags[1] & ~std::uint64_t{0x3}) != 0)
        return SprintInputRewrite::layout_rejected;

    const auto bit = [](std::uint32_t index) { return std::uint64_t{1} << index; };
    const bool packet_was_sprinting = (input_flags[0] & bit(sprinting_input_bit)) != 0;
    const bool previous = was_overriding
        ? state.sprint_packet_last_state.load(std::memory_order_acquire)
        : packet_was_sprinting;
    // While Sprint is enabled, its desired state is authoritative. When it is
    // disabled after controlling sprint, send one explicit stop transition,
    // then let subsequent packets pass through unchanged.
    const bool desired = override_enabled ? sprinting : (was_overriding ? false : packet_was_sprinting);
    if (desired) input_flags[0] |= bit(sprinting_input_bit);
    else input_flags[0] &= ~bit(sprinting_input_bit);
    if (override_enabled || was_overriding) {
        input_flags[0] &= ~(bit(start_sprinting_input_bit) | bit(stop_sprinting_input_bit));
        if (previous != desired)
            input_flags[0] |= bit(desired ? start_sprinting_input_bit : stop_sprinting_input_bit);
    }
    if (!write_at(packet_address, player_auth_input_flags_offset, input_flags))
        return SprintInputRewrite::layout_rejected;

    state.sprint_packet_last_state.store(desired, std::memory_order_release);
    state.sprint_packet_override.store(override_enabled, std::memory_order_release);
    return SprintInputRewrite::rewritten;
}

RotationInputRewrite BedrockGameplay::rewrite_rotation_input(void* packet, bool vector_fix) noexcept {
    const auto& state = *impl_;
    const auto angles = state.server_rotation.load(std::memory_order_acquire);
    if (!state.supported || !packet || angles == no_server_rotation) return RotationInputRewrite::ignored;
    const auto address = reinterpret_cast<std::uintptr_t>(packet);
    const auto get_id = state.method(address, 1);
    if (!get_id) return RotationInputRewrite::ignored;
    using GetId = int(__fastcall*)(void*);
    const int id = reinterpret_cast<GetId>(get_id)(packet);
    if (id != player_auth_input_packet_id && id != move_player_packet_id) return RotationInputRewrite::ignored;
    // Both the vtable and getId implementation were checked in the supported
    // PE. A matching numeric ID alone does not establish a packet's layout.
    const std::uintptr_t vtable_rva = id == player_auth_input_packet_id ? 0xE83A360 : 0xE85BFC0;
    const std::uintptr_t get_id_rva = id == player_auth_input_packet_id ? 0xB26A00 : 0x2ECD950;
    const std::array<unsigned char, 6> get_id_code{0xB8, static_cast<unsigned char>(id), 0, 0, 0, 0xC3};
    std::uintptr_t vtable{};
    if (!read_at(address, 0, vtable) || !state.image.contains(vtable) ||
        vtable - state.image.address != vtable_rva || !state.native_identity(get_id, get_id_rva, get_id_code))
        return RotationInputRewrite::layout_rejected;
    const auto yaw = std::bit_cast<float>(static_cast<std::uint32_t>(angles));
    const auto pitch = std::bit_cast<float>(static_cast<std::uint32_t>(angles >> 32));
    return detail::write_gameplay_rotation_input(address, id, yaw, pitch, vector_fix)
        ? RotationInputRewrite::rewritten : RotationInputRewrite::layout_rejected;
}

MoveFixInputRewrite BedrockGameplay::rewrite_move_fix_input(void* packet, bool enabled) noexcept {
    auto& state = *impl_;
    if (!enabled) {
        state.move_fix_sprinting.store(false, std::memory_order_release);
        return MoveFixInputRewrite::ignored;
    }
    if (!state.supported || !packet) return MoveFixInputRewrite::ignored;
    const auto address = reinterpret_cast<std::uintptr_t>(packet);
    const auto get_id = state.method(address, 1);
    if (!get_id) return MoveFixInputRewrite::ignored;
    using GetId = int(__fastcall*)(void*);
    if (reinterpret_cast<GetId>(get_id)(packet) != player_auth_input_packet_id)
        return MoveFixInputRewrite::ignored;
    constexpr std::array<unsigned char, 6> get_id_code{0xB8, 144, 0, 0, 0, 0xC3};
    std::uintptr_t vtable{};
    if (!read_at(address, 0, vtable) || !state.image.contains(vtable) ||
        vtable - state.image.address != 0xE83A360 || !state.native_identity(get_id, 0xB26A00, get_id_code))
        return MoveFixInputRewrite::layout_rejected;
    bool sprinting = state.move_fix_sprinting.load(std::memory_order_acquire);
    if (!detail::write_gameplay_move_fix_input(address, sprinting))
        return MoveFixInputRewrite::layout_rejected;
    state.move_fix_sprinting.store(sprinting, std::memory_order_release);
    return MoveFixInputRewrite::rewritten;
}

bool BedrockGameplay::read_rotation_movement(void* packet, RotationMovementSample& sample) const noexcept {
    sample = {};
    const auto& state = *impl_;
    if (!state.supported || !packet) return false;
    constexpr std::uintptr_t auth_vtable_rva = 0xE83A360;
    constexpr std::uintptr_t auth_get_id_rva = 0xB26A00;
    constexpr std::uintptr_t move_vtable_rva = 0xE85BFC0;
    constexpr std::uintptr_t move_get_id_rva = 0x2ECD950;
    constexpr std::size_t client_tick_offset = 0xA8;
    constexpr std::array<unsigned char, 6> get_id_code{0xB8, 144, 0, 0, 0, 0xC3};
    constexpr std::array<unsigned char, 6> move_get_id_code{0xB8, 19, 0, 0, 0, 0xC3};
    const auto address = reinterpret_cast<std::uintptr_t>(packet);
    const auto get_id = state.method(address, 1);
    std::uintptr_t vtable{};
    if (!get_id || !read_at(address, 0, vtable) || !state.image.contains(vtable)) return false;
    const auto vtable_rva = vtable - state.image.address;
    if (vtable_rva == auth_vtable_rva) {
        if (!state.native_identity(get_id, auth_get_id_rva, get_id_code)) return false;
    } else if (vtable_rva == move_vtable_rva) {
        if (!state.native_identity(get_id, move_get_id_rva, move_get_id_code)) return false;
    } else return false;
    using GetId = int(__fastcall*)(void*);
    sample.packet_id = reinterpret_cast<GetId>(get_id)(packet);
    sample.recognized = true;
    sample.validation_stage = 1;
    if (sample.packet_id != player_auth_input_packet_id) {
        sample.validation_stage = 2; // valid MovePlayer has no input vectors
        return false;
    }
    if (vtable_rva != auth_vtable_rva) {
        sample.validation_stage = 3;
        return false;
    }
    if (!read_at(address, player_auth_input_rotation_offset, sample.rotation) ||
        !read_at(address, 0x38, sample.position) ||
        !read_at(address, player_auth_input_analog_move_offset, sample.analog_move) ||
        !read_at(address, player_auth_input_move_offset, sample.move) ||
        !read_at(address, player_auth_input_raw_move_offset, sample.raw_move) ||
        !read_at(address, client_tick_offset, sample.client_tick) ||
        !valid_position(sample.position) || !std::isfinite(sample.rotation.x) ||
        !std::isfinite(sample.rotation.y)) {
        sample.validation_stage = 4;
        return false;
    }
    const auto valid_move = [](Vec2 value) {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
            std::abs(value.x) <= 1.01F && std::abs(value.y) <= 1.01F;
    };
    if (!valid_move(sample.analog_move) || !valid_move(sample.move) || !valid_move(sample.raw_move)) {
        sample.validation_stage = 5;
        return false;
    }
    sample.valid = true;
    sample.validation_stage = 6;
    return true;
}

bool BedrockGameplay::swing() {
    return swing_with_source(4); // ActorSwingSource::Attack
}

bool BedrockGameplay::mining_swing() {
    return swing_with_source(2); // ActorSwingSource::Mine
}

bool BedrockGameplay::swing_with_source(std::uint8_t swing_source) {
    constexpr std::uintptr_t local_swing_rva = 0x2080660;
    // Actor::swing(source, hand) is vtable slot 109 in the validated 1.26.52.3
    // actor layout. LocalPlayer overrides it to carry the swing source through
    // the normal gameplay/network path while updating the local arm animation.
    constexpr std::size_t local_player_swing_slot = 109;
    constexpr std::uint8_t main_hand = 0;
    constexpr std::array<unsigned char, 24> local_swing_prefix{
        0x56,0x57,0x48,0x83,0xEC,0x28,0x48,0x89,0xCE,0x80,0xB9,0x34,0x04,0x00,0x00,0x01,
        0x75,0x28,0x8B,0xBE,0x10,0x04,0x00,0x00
    };
    auto& state = *impl_;
    if (!state.on_tick() || !state.player) return false;

    // The interpolation methods at slots 13/14 are checked against exact
    // executable identities first, so an unexpected player vtable cannot
    // turn the swing call into an arbitrary native call.
    const auto body_yaw = body_yaw_interpolator_target();
    const auto head_yaw = head_yaw_interpolator_target();
    if (body_yaw && head_yaw && state.has_method(state.player, body_yaw) &&
        state.has_method(state.player, head_yaw)) {
        if (const auto method = state.method(state.player, local_player_swing_slot)) {
            using PlayerSwing = bool(__fastcall*)(void*, std::uint8_t, std::uint8_t);
            return reinterpret_cast<PlayerSwing>(method)(reinterpret_cast<void*>(state.player),
                swing_source, main_hand);
        }
    }

    // Preserve local-only animation as a safe fallback if the LocalPlayer
    // virtual entry is unavailable for this executable profile.
    if (local_swing_rva > UINTPTR_MAX - state.image.address) return false;
    const auto target = state.image.address + local_swing_rva;
    if (state.local_swing_method && state.local_swing_method != target) state.local_swing_method = 0;
    if (!state.local_swing_method) {
        if (!state.native_identity(target, local_swing_rva, local_swing_prefix)) return false;
        state.local_swing_method = target;
    }
    using Swing = bool(__fastcall*)(void*);
    return reinterpret_cast<Swing>(state.local_swing_method)(reinterpret_cast<void*>(state.player));
}

bool BedrockGameplay::replaceable(BlockPos position) const {
    if (!impl_->on_tick()) return false;
    const auto block = impl_->block(position);
    return block && (block->id == 0 || (block->id >= 8 && block->id <= 11));
}

bool BedrockGameplay::block_id(BlockPos position, std::uint16_t& id) const {
    if (!impl_->on_tick()) return false;
    const auto block = impl_->block(position);
    if (!block) return false;
    id = block->id;
    return true;
}

bool BedrockGameplay::can_see_block(BlockPos position) const {
    if (!impl_->on_tick()) return false;
    return impl_->clear_ray_to_block(position);
}

void BedrockGameplay::stop_destroy_block() noexcept {
    auto& state = *impl_;
    state.clear_destroy_speed_target();
    if (!state.destroy_active) return;
    if (state.on_tick() && state.destroy_mode && state.game_mode() == state.destroy_mode) {
        std::uintptr_t start{}, proceed{}, stop{};
        std::uintptr_t destroy{};
        if (state.destroy_methods(state.destroy_mode, start, destroy, proceed, stop)) {
            using Stop = void(__fastcall*)(void*, const BlockPos*);
            reinterpret_cast<Stop>(stop)(reinterpret_cast<void*>(state.destroy_mode), &state.destroy_position);
        }
    }
    state.destroy_active = false;
    state.destroy_mode = 0;
}

bool BedrockGameplay::advance_destroy_block(BlockPos position, std::uint8_t face, float destroy_time_scale,
                                            float& progress, bool& destroyed) {
    auto& state = *impl_;
    progress = 0.0F;
    destroyed = false;
    state.destroy_diagnostic_stage = 1; // precondition/profile validation failed unless advanced below
    state.destroy_diagnostic_profile_stage = 0;
    state.destroy_diagnostic_continue_return = false;
    state.destroy_diagnostic_native_destroyed = false;
    state.destroy_diagnostic_progress = 0.0F;
    state.destroy_diagnostic_requested_mode = 0;
    state.destroy_diagnostic_requested_owner = 0;
    state.destroy_diagnostic_requested_vtable_rva = 0;
    state.destroy_diagnostic_requested_owner_matches = false;
    state.destroy_diagnostic_requested_position = position;
    state.destroy_diagnostic_requested_face = face;
    if (!state.on_tick() || face > 5 || !std::isfinite(destroy_time_scale)) return false;
    std::uintptr_t candidate_mode{}, candidate_owner{}, candidate_table{};
    if (read_at(state.player, 0xAA0, candidate_mode) && candidate_mode &&
        read_at(candidate_mode, 0, candidate_table) && read_at(candidate_mode, 8, candidate_owner)) {
        state.destroy_diagnostic_requested_mode = candidate_mode;
        state.destroy_diagnostic_requested_owner = candidate_owner;
        state.destroy_diagnostic_requested_owner_matches = candidate_owner == state.player;
        state.destroy_diagnostic_requested_vtable_rva = state.image.contains(candidate_table)
            ? candidate_table - state.image.address : 0;
        state.destroy_diagnostic_profile_stage = 1; // candidate pointer, owner and vtable readable
    }
    destroy_time_scale = std::clamp(destroy_time_scale, 0.01F, 1.0F);
    state.destroy_speed_scale.store(destroy_time_scale, std::memory_order_release);
    const auto mode = state.game_mode();
    std::uintptr_t start{}, destroy{}, proceed{}, stop{};
    if (!mode) {
        state.destroy_diagnostic_stage = 2; // GameMode/vtable profile rejected
        return false;
    }
    state.destroy_diagnostic_profile_stage = 2; // owner and mode vtable profile accepted
    if (!state.destroy_methods(mode, start, destroy, proceed, stop)) {
        state.destroy_diagnostic_stage = 2;
        state.destroy_diagnostic_profile_stage = 3; // destroy method identities rejected
        return false;
    }
    state.destroy_diagnostic_profile_stage = 4; // destroy methods validated
    (void)start;
    (void)stop;
    constexpr std::size_t destroy_position_offset = 0x10;
    constexpr std::size_t destroy_face_offset = 0x1C;
    constexpr std::size_t destroy_progress_offset = 0x24;
    if (state.destroy_active && (state.destroy_mode != mode || state.destroy_position.x != position.x ||
        state.destroy_position.y != position.y || state.destroy_position.z != position.z || state.destroy_face != face))
        stop_destroy_block();
    if (!state.destroy_active) {
        bool destroyed_immediately = false;
        using Start = bool(__fastcall*)(void*, const BlockPos*, std::uint8_t, bool*);
        const bool accepted = reinterpret_cast<Start>(start)(reinterpret_cast<void*>(mode), &position, face,
                                                              &destroyed_immediately);
        if (destroyed_immediately) {
            destroyed = true;
            state.destroy_diagnostic_native_destroyed = true;
            progress = 1.0F;
            state.destroy_diagnostic_stage = 7; // native immediate break
            state.destroy_diagnostic_progress = progress;
            return true;
        }
        if (!accepted) {
            state.destroy_diagnostic_stage = 3; // startDestroyBlock returned false
            return false;
        }
        BlockPos stored_position{};
        std::uint8_t stored_face{};
        float current_progress{};
        if (!read_at(mode, destroy_position_offset, stored_position) ||
            !read_at(mode, destroy_face_offset, stored_face) ||
            !read_at(mode, destroy_progress_offset, current_progress) ||
            stored_position.x != position.x || stored_position.y != position.y || stored_position.z != position.z ||
            stored_face != face || !std::isfinite(current_progress) || current_progress < -0.01F ||
            current_progress > 1.01F) {
            using Stop = void(__fastcall*)(void*, const BlockPos*);
            reinterpret_cast<Stop>(stop)(reinterpret_cast<void*>(mode), &position);
            state.destroy_diagnostic_stage = 4; // native start state did not match the request
            return false;
        }
        state.destroy_active = true;
        state.destroy_mode = mode;
        state.destroy_position = position;
        state.destroy_face = face;
        state.publish_destroy_speed_target(position, face, destroy_time_scale);
        progress = std::clamp(current_progress, 0.0F, 1.0F);
        state.destroy_diagnostic_stage = 8; // native start accepted
        state.destroy_diagnostic_progress = progress;
        return true;
    }

    BlockPos stored_position{};
    std::uint8_t stored_face{};
    float previous_progress{};
    if (!read_at(mode, destroy_position_offset, stored_position) ||
        !read_at(mode, destroy_face_offset, stored_face) ||
        !read_at(mode, destroy_progress_offset, previous_progress) ||
        stored_position.x != position.x || stored_position.y != position.y || stored_position.z != position.z ||
        stored_face != face || !std::isfinite(previous_progress) || previous_progress < -0.01F ||
        previous_progress > 1.01F) {
        stop_destroy_block();
        state.destroy_diagnostic_stage = 5; // active GameMode state changed before continue
        return false;
    }
    bool destroyed_now = false;
    state.destroy_speed_scale.store(destroy_time_scale, std::memory_order_release);
    using Continue = bool(__fastcall*)(void*, const BlockPos*, std::uint8_t, const Vec3*, bool*);
    const bool continuing = reinterpret_cast<Continue>(proceed)(reinterpret_cast<void*>(mode), &position, face,
                                                                 &state.frame.local.position, &destroyed_now);
    // The native return reports whether its own input loop should continue;
    // Regen drives that loop explicitly, so validated GameMode progress is the
    // completion signal for this caller.
    state.destroy_diagnostic_continue_return = continuing;
    float native_progress{};
    if (!read_at(mode, destroy_progress_offset, native_progress) || !std::isfinite(native_progress) ||
        native_progress < -0.01F || native_progress > 1.01F) {
        stop_destroy_block();
        state.destroy_diagnostic_stage = 6; // progress field became invalid after continue
        return false;
    }
    progress = std::clamp(std::max(native_progress,
        state.destroy_speed_server_progress.load(std::memory_order_acquire)), 0.0F, 1.0F);
    destroyed = destroyed_now;
    state.destroy_diagnostic_native_destroyed = destroyed_now;
    state.destroy_diagnostic_stage = continuing ? 9 : 10; // 9=continue true, 10=continue false
    state.destroy_diagnostic_progress = progress;
    if (destroyed) {
        state.destroy_active = false;
        state.destroy_mode = 0;
        state.clear_destroy_speed_target();
    }
    return true;
}

bool BedrockGameplay::should_switch_before_destroy() const {
    const auto& state = *impl_;
    if (!state.on_tick() || !state.destroy_active || state.game_mode() != state.destroy_mode) return false;
    const auto mode = state.destroy_mode;
    constexpr std::size_t old_progress_offset = 0x20;
    constexpr std::size_t destroy_progress_offset = 0x24;
    float previous{}, current{};
    BlockPos position{};
    if (!read_at(mode, 0x10, position) || position.x != state.destroy_position.x ||
        position.y != state.destroy_position.y || position.z != state.destroy_position.z ||
        !read_at(mode, old_progress_offset, previous) || !read_at(mode, destroy_progress_offset, current) ||
        !std::isfinite(previous) || !std::isfinite(current) || previous < -0.01F || current < 0.0F ||
        current > 1.01F || current <= previous) return false;
    // The progress hook already scaled what the GameMode accumulated.
    return current + (current - previous) >= 1.0F;
}

bool BedrockGameplay::prepare_durability_safe_finish() {
    auto& state = *impl_;
    if (!state.on_tick() || !state.destroy_active || state.game_mode() != state.destroy_mode) return false;
    constexpr std::size_t destroy_progress_offset = 0x24;
    float progress{};
    BlockPos position{};
    std::uint8_t face{};
    if (!read_at(state.destroy_mode, 0x10, position) || position.x != state.destroy_position.x ||
        position.y != state.destroy_position.y || position.z != state.destroy_position.z ||
        !read_at(state.destroy_mode, 0x1C, face) || face != state.destroy_face ||
        !read_at(state.destroy_mode, destroy_progress_offset, progress) || !std::isfinite(progress) ||
        progress < 0.0F || progress >= 1.0F) return false;
    return write_at(state.destroy_mode, destroy_progress_offset, 0.999F);
}

int BedrockGameplay::selected_hotbar_slot() const noexcept {
    const auto& state = *impl_;
    if (!state.on_tick()) return -1;
    std::uintptr_t supplies{};
    int selected = -1;
    if (!read_at(state.player, 0x5B8, supplies) || !read_at(supplies, 0x10, selected) ||
        selected < 0 || selected > 8) return -1;
    return selected;
}

bool BedrockGameplay::hotbar_item(int slot, BlockPos target, HotbarItemInfo& info) const {
    auto& state = *impl_;
    info = {};
    if (!state.on_tick() || slot < 0 || slot > 8) return false;
    const auto stack = state.inventory_stack(state.inventory_container(), slot);
    std::uintptr_t item{};
    std::uint8_t count{};
    if (!stack || !detail::read_gameplay_stack_item(stack, item, count)) return false;
    if (!item) return true;
    constexpr std::size_t item_id_offset = 0xAA;
    constexpr std::size_t max_damage_offset = 0x150;
    const auto name = read_msvc_string(reinterpret_cast<const void*>(item + item_identifier_offset), 128);
    std::int16_t item_id{}, max_damage{};
    const bool item_id_read = read_at(item, item_id_offset, item_id);
    const bool max_damage_read = read_at(item, max_damage_offset, max_damage) && max_damage >= 0;
    const auto classified = name ? classify_inventory_item(*name, count) : InventoryItemInfo{};
    const auto pickaxe_rank = [](std::int16_t id) noexcept -> float {
        switch (id) {
        case 621: case 607: return 5.0F; // Netherite
        case 320: case 276: return 4.0F; // Diamond
        case 299: case 267: return 3.0F; // Iron
        case 327: case 283: return 2.0F; // Golden
        case 316: case 272: return 1.0F; // Stone
        case 312: case 268: return 0.5F; // Wooden
        default: return 0.0F;
        }
    };
    const float id_rank = item_id_read ? pickaxe_rank(item_id) : 0.0F;
    if (!name && id_rank == 0.0F) return false;
    info.occupied = true;
    info.item_id = item_id_read ? item_id : 0;
    info.pickaxe = classified.kind == InventoryItemKind::pickaxe || id_rank > 0.0F;
    info.damageable = max_damage_read ? max_damage > 0 : info.pickaxe;
    // Tier ranking is only for choosing between known vanilla pickaxes;
    // actual break progress still comes from the game's native GameMode.
    if (info.pickaxe) info.destroy_speed = std::max(classified.value, id_rank);
    return true;
}

bool BedrockGameplay::select_hotbar_slot(int slot) {
    auto& state = *impl_;
    if (!state.on_tick() || slot < 0 || slot > 8) return false;
    std::uintptr_t supplies{};
    int selected = -1;
    if (!read_at(state.player, 0x5B8, supplies) || !read_at(supplies, 0x10, selected) ||
        selected < 0 || selected > 8) return false;
    if (selected == slot) return true;
    if (!write_at(supplies, 0x10, slot)) return false;
    state.switched_this_tick = true;
    return true;
}

bool BedrockGameplay::inventory_item(int slot, InventoryItemInfo& info) const {
    const auto& state = *impl_;
    info = {};
    if (!state.on_tick() || slot < 0 || slot > 35) return false;
    const auto container = state.inventory_container();
    if (!container) return false;
    const auto stack = state.inventory_stack(container, slot);
    std::uintptr_t item{};
    std::uint8_t count{};
    if (!stack || !detail::read_gameplay_stack_item(stack, item, count)) {
        state.inventory_diagnostic.stage = "inventory-stack-unreadable";
        return false;
    }
    if (!item) return true;
    // Same rule as Scaffold's block slots: only solid block items count.
    std::uintptr_t block{}, legacy{};
    std::uint8_t flags{};
    if (read_at(stack, 0x18, block) && block && read_at(block, 0x68, legacy) && state.method(legacy, 0) &&
        read_at(legacy, 0x164, flags) && (flags & 2) != 0) {
        info = {InventoryItemKind::block, 0.0F, count};
        (void)read_at(item, 0xAA, info.id);
        return true;
    }
    state.describe_item(stack, item, count, info);
    return true;
}

bool BedrockGameplay::armor_item(int slot, InventoryItemInfo& info) const {
    const auto& state = *impl_;
    info = {};
    if (!state.on_tick() || slot < 0 || slot > 3) return false;
    const auto stack = state.inventory_stack(state.armor_container(), slot);
    std::uintptr_t item{};
    std::uint8_t count{};
    if (!stack || !detail::read_gameplay_stack_item(stack, item, count)) return false;
    if (item) state.describe_item(stack, item, count, info);
    return true;
}

bool BedrockGameplay::inventory_screen_open() const {
    const auto& state = *impl_;
    std::uintptr_t model{};
    std::int8_t type{};
    return state.on_tick() && state.container_model(model, type) && type == -1;
}

bool BedrockGameplay::swap_inventory_slots(int first, int second) {
    auto& state = *impl_;
    if (!state.on_tick() || first == second || first < 0 || first > 35 || second < 0 || second > 35) return false;
    const auto container = state.inventory_container();
    const auto first_stack = state.inventory_stack(container, first);
    const auto second_stack = state.inventory_stack(container, second);
    if (!first_stack || !second_stack || !state.send_inventory_transaction({{
        {{}, static_cast<std::uint32_t>(first), first_stack, second_stack},
        {{}, static_cast<std::uint32_t>(second), second_stack, first_stack}}})) return false;
    state.note_local_update(state.exchange_local(first_stack, second_stack, [&](std::uintptr_t for_first, std::uintptr_t for_second) {
        return state.set_inventory_slot(container, first, for_first) && state.set_inventory_slot(container, second, for_second);
    }));
    return true;
}

bool BedrockGameplay::equip_armor(int inventory_slot, int armor_slot) {
    auto& state = *impl_;
    if (!state.on_tick() || inventory_slot < 0 || inventory_slot > 35 || armor_slot < 0 || armor_slot > 3) return false;
    const auto container = state.inventory_container();
    const auto stack = state.inventory_stack(container, inventory_slot);
    const auto worn = state.inventory_stack(state.armor_container(), armor_slot);
    constexpr std::int8_t armor_container_id = 120;
    if (!stack || !worn || !state.send_inventory_transaction({{
        {{}, static_cast<std::uint32_t>(inventory_slot), stack, worn},
        {{0, armor_container_id}, static_cast<std::uint32_t>(armor_slot), worn, stack}}})) return false;
    state.note_local_update(state.exchange_local(stack, worn, [&](std::uintptr_t for_inventory, std::uintptr_t for_armor) {
        return state.set_inventory_slot(container, inventory_slot, for_inventory) && state.set_armor_slot(armor_slot, for_armor);
    }));
    return true;
}

bool BedrockGameplay::drop_inventory_slot(int slot) {
    auto& state = *impl_;
    if (!state.on_tick() || slot < 0 || slot > 35) return false;
    const auto container = state.inventory_container();
    const auto stack = state.inventory_stack(container, slot);
    const auto empty = state.empty_stack();
    constexpr std::int32_t world_interaction = 2;
    if (!stack || !empty || !state.send_inventory_transaction({{
        {{}, static_cast<std::uint32_t>(slot), stack, empty},
        {{world_interaction, 0}, 0, empty, stack}}})) return false;
    state.note_local_update(state.set_inventory_slot(container, slot, empty));
    return true;
}

std::uintptr_t BedrockGameplay::container_screen_tick_target() const noexcept {
    constexpr std::uintptr_t rva = 0x4DF8350;
    constexpr std::array<unsigned char, 16> prefix{
        0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xEC,0xC8};
    const auto& state = *impl_;
    const auto target = state.image.address + rva;
    return state.supported && state.native_identity(target, rva, prefix) ? target : 0;
}

bool BedrockGameplay::container_is_chest() const noexcept {
    const auto& state = *impl_;
    std::uintptr_t model{};
    std::int8_t type{};
    return state.local_player_current() && state.container_model(model, type) && type == 0;
}

bool BedrockGameplay::container_slot_occupied(int slot) const noexcept {
    const auto& state = *impl_;
    std::uintptr_t model{}, item{};
    std::int8_t type{};
    std::uint8_t count{};
    if (slot < 0 || slot >= ContainerScreen::slot_limit || !state.container_model(model, type)) return false;
    // ContainerManagerModel slot 9 is getSlot; out-of-range slots return a shared empty stack.
    const auto get_slot = state.method(model, 9);
    const auto stack = get_slot ? reinterpret_cast<std::uintptr_t(__fastcall*)(void*, int)>(get_slot)(
        reinterpret_cast<void*>(model), slot) : 0;
    return stack && detail::read_gameplay_stack_item(stack, item, count) && item;
}

bool BedrockGameplay::take_container_slot(void* controller, int slot) noexcept {
    constexpr std::uintptr_t rva = 0x4DFB4D0;
    constexpr std::array<unsigned char, 16> prefix{
        0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xEC,0x48};
    constexpr std::string_view collection = "container_items";
    const auto& state = *impl_;
    const auto target = state.image.address + rva;
    if (!controller || slot < 0 || slot >= ContainerScreen::slot_limit ||
        !state.native_identity(target, rva, prefix)) return false;
    static_assert(sizeof(detail::NativeSmallString) == detail::native_small_string_size);
    static_assert(collection.size() <= 15);
    detail::NativeSmallString name;
    std::memcpy(name.text, collection.data(), collection.size());
    name.size = collection.size();
    // handleAutoPlace(controller, 0x7FFFFFFF, const std::string&, slot); the
    // name is copied before the call returns.
    using AutoPlace = void(__fastcall*)(void*, int, const void*, int);
    reinterpret_cast<AutoPlace>(target)(controller, 0x7FFFFFFF, &name, slot);
    return true;
}

bool BedrockGameplay::close_container_screen(void* controller) noexcept {
    constexpr std::uintptr_t rva = 0x2240F40;
    constexpr std::array<unsigned char, 16> prefix{
        0x55,0x56,0x57,0x48,0x81,0xEC,0xE0,0,0,0,0x48,0x8D,0xAC,0x24,0x80,0};
    const auto& state = *impl_;
    const auto target = state.method(reinterpret_cast<std::uintptr_t>(controller), 12);
    if (!target || !state.native_identity(target, rva, prefix)) return false;
    reinterpret_cast<void*(__fastcall*)(void*)>(target)(controller);
    return true;
}

bool BedrockGameplay::place_block(BlockPos position, bool auto_select) {
    auto& state = *impl_;
    if (!state.on_tick() || !replaceable(position)) return false;
    const Vec3 center{static_cast<float>(position.x) + 0.5F, static_cast<float>(position.y) + 0.5F,
                      static_cast<float>(position.z) + 0.5F};
    if (distance_squared(center, state.frame.local.position) > 8 * 8) return false;
    // A placement face points outward from the supporting neighbor into the
    // target cell. This matches Solstice BlockUtils' face/neighbor mapping.
    constexpr std::array<BlockPos, 6> offsets{{{0, 1, 0}, {0, -1, 0}, {0, 0, 1},
                                              {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}}};
    int face = -1;
    BlockPos neighbor{};
    for (const int index : {1, 2, 3, 4, 5, 0}) {
        const auto offset = offsets[static_cast<std::size_t>(index)];
        const BlockPos candidate{position.x + offset.x, position.y + offset.y, position.z + offset.z};
        const auto block = state.block(candidate);
        // Any block that is not air or liquid has a face to click, as in
        // Solstice's getBlockPlaceFace: glass, slabs, stairs and leaves too.
        if (block && block->id != 0 && !(block->id >= 8 && block->id <= 11)) {
            face = index;
            neighbor = candidate;
            break;
        }
    }
    if (face < 0) return false;
    std::uintptr_t supplies = 0, container = 0;
    int selected = -1;
    if (!read_at(state.player, 0x5B8, supplies) || !read_at(supplies, 0x10, selected) || selected < 0 || selected > 8 ||
        !read_at(supplies, 0xB8, container)) return false;
    const auto get_item = state.method(container, 7);
    const auto mode = state.game_mode();
    const auto build = state.method(mode, 6);
    if (!get_item || !build) return false;
    const auto block_slot = [&](int slot) {
        const auto item = reinterpret_cast<std::uintptr_t(__fastcall*)(void*, int)>(get_item)(
            reinterpret_cast<void*>(container), slot);
        std::uintptr_t block = 0, legacy = 0;
        std::uint8_t count = 0, flags = 0;
        return item && read_at(item, 0x18, block) && block && read_at(item, 0x22, count) && count &&
               read_at(block, 0x68, legacy) && state.method(legacy, 0) &&
               read_at(legacy, 0x164, flags) && (flags & 2) != 0;
    };
    // Nothing more this tick once Auto Select changed the slot: the game has
    // not told the server yet, so the placement would be refused.
    if (state.switched_this_tick) return false;
    if (!block_slot(selected)) {
        if (!auto_select) return false;
        int slot = -1;
        for (int i = 0; i < 9; ++i) if (block_slot(i)) { slot = i; break; }
        if (slot < 0 || !write_at(supplies, 0x10, slot)) return false;
        if (state.switch_from < 0 || state.switch_supplies != supplies) state.switch_from = selected;
        state.switch_supplies = supplies;
        state.switch_to = slot;
        state.switch_used = GetTickCount64();
        state.switched_this_tick = true;
        return false; // placed from the new slot on a later attempt
    }
    if (selected == state.switch_to && supplies == state.switch_supplies) state.switch_used = GetTickCount64();
    std::uintptr_t level = 0;
    if (!read_at(state.player, 0x1D8, level)) return false;
    const auto get_hit = state.method(level, 335);
    if (!get_hit) return false;
    const auto hit = reinterpret_cast<std::uintptr_t(__fastcall*)(void*)>(get_hit)(reinterpret_cast<void*>(level));
    std::array<std::byte, 0x38> original_hit{};
    if (!read_at(hit, 0, original_hit)) return false;
    auto placement_hit = original_hit;
    const auto offset = offsets[static_cast<std::size_t>(face)];
    const Vec3 hit_position{static_cast<float>(neighbor.x) + 0.5F - static_cast<float>(offset.x) * 0.5F,
                           static_cast<float>(neighbor.y) + 0.5F - static_cast<float>(offset.y) * 0.5F,
                           static_cast<float>(neighbor.z) + 0.5F - static_cast<float>(offset.z) * 0.5F};
    Vec3 direction{hit_position.x - state.frame.eye.x, hit_position.y - state.frame.eye.y,
                   hit_position.z - state.frame.eye.z};
    const auto length = std::sqrt(distance_squared(direction, {}));
    if (length < 0.01F || !std::isfinite(length)) return false;
    direction = {direction.x / length, direction.y / length, direction.z / length};
    const std::uint32_t hit_type = 0;
    const auto hit_face = static_cast<std::uint8_t>(face);
    std::memcpy(placement_hit.data(), &state.frame.eye, sizeof(Vec3));
    std::memcpy(placement_hit.data() + 0xC, &direction, sizeof(Vec3));
    std::memcpy(placement_hit.data() + 0x18, &hit_type, 4);
    std::memcpy(placement_hit.data() + 0x1C, &hit_face, 1);
    std::memcpy(placement_hit.data() + 0x20, &neighbor, sizeof(BlockPos));
    std::memcpy(placement_hit.data() + 0x2C, &hit_position, sizeof(Vec3));
    // buildBlock consumes the current level HitResult synchronously to form
    // transaction click coordinates. Preserve only its scalar prefix; the
    // WeakEntityRef at +0x38 and subsequent game-owned state are untouched.
    struct RestoreHit {
        std::uintptr_t address;
        std::array<std::byte, 0x38> original;
        ~RestoreHit() { write_at(address, 0, original); }
    } restore_hit{hit, original_hit};
    if (!write_at(hit, 0, placement_hit)) return false;
    using Build = bool(__fastcall*)(void*, const BlockPos*, unsigned char, unsigned char, bool);
    return reinterpret_cast<Build>(build)(reinterpret_cast<void*>(mode), &neighbor,
        static_cast<unsigned char>(face), 0, false);
}

} // namespace medicon::sdk
