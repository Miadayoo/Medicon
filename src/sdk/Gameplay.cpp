#include <medicon/sdk/BedrockGameplay.hpp>
#include <medicon/sdk/detail/GameplayMemory.hpp>
#include <medicon/memory/module_view.hpp>

#include <Windows.h>
#include <winver.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <climits>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace medicon::sdk {
namespace {

// Release x64 layout reference, inspected at the pinned revision. No upstream
// SDK code or game-owned STL object is linked into this bridge.
// https://github.com/surukotonai/solstice/tree/1d7808f52ef4ba6e34a7fbcc65294907a7761611
// SDK/EnttHashs.hpp, OffsetProvider.hpp, Actor/EntityId.hpp,
// include/entt/entity/{registry,sparse_set,storage}.hpp.
constexpr std::uint32_t state_hash = 0x1B5D5238, bounds_hash = 0xBAC1B3CF;
constexpr std::uint32_t rotation_hash = 0x75DF36B7, type_hash = 0x4F6BA419;
constexpr std::uint32_t owner_hash = 0x85B93800, dead_hash = 0x710A62C4;
constexpr std::uint32_t head_hash = 0xBABE7211, body_hash = 0xD7F64BBA;
constexpr std::uint32_t render_position_hash = 0xE53C7221;
constexpr std::uint32_t entity_mask = 0x3FFFF;
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
            output.push_back(entry);
        }
        std::ranges::sort(output, {}, &GameplayPool::hash);
        for (std::size_t i = 1; i < output.size(); ++i)
            if (output[i - 1].hash == output[i].hash) { output.clear(); return false; }
        if (required != 31) { output.clear(); return false; }
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

} // namespace detail

struct BedrockGameplay::Impl {
    GameplayFrame frame;
    bool located = false, supported = false, active = false;
    DWORD thread = 0;
    std::uintptr_t client = 0, player = 0, registry = 0, block_source = 0;
    // The LevelRenderer child whose camera fields the last capture validated.
    std::uintptr_t view_player = 0;
    // The local player's RenderPositionComponent, which the game moves every
    // frame to where it draws the model. Resolved again by every capture.
    std::uintptr_t render_position = 0;
    // Auto Select keeps the block slot selected while placing (like Solstice's
    // default Full switch) so the game reports it to the server on its own
    // tick before a placement uses it. The original slot comes back once
    // nothing has placed from the switched slot for a second, or on the first
    // capture after Scaffold was turned off. Survives clear().
    std::uintptr_t switch_supplies = 0;
    int switch_from = -1, switch_to = -1;
    ULONGLONG switch_used = 0;
    bool switch_active = true;
    bool switched_this_tick = false;
    std::uint32_t local_entity = 0;
    memory::AddressRange image;
    std::vector<memory::AddressRange> executable_regions;
    std::vector<detail::GameplayPool> pools;
    std::string diagnostic;

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

    bool native_identity(std::uintptr_t target, std::size_t rva,
                         const std::array<unsigned char, 16>& expected) const noexcept {
        std::array<unsigned char, 16> actual{};
        return rva <= UINTPTR_MAX - image.address && target == image.address + rva &&
               executable(target) && copy_bytes(target, actual.data(), actual.size()) && actual == expected;
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
        // RVAs identify the two observed 1.26.52.3 implementations only. They
        // are never cast to functions or used to bypass validated vtable calls.
        const bool base = table == image.address + 0xE827560;
        const bool survival = table == image.address + 0xE827600;
        if (!base && !survival) return 0;
        const auto attack = method(mode, 15), build = method(mode, 6);
        if (base) {
            if (!native_identity(attack, 0x2599CA0,
                    {0x4D,0x89,0xC1,0x41,0xB0,0x01,0xE9,0x05,0,0,0,0xCC,0xCC,0xCC,0xCC,0xCC}) ||
                !native_identity(build, 0x259BC60,
                    {0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xEC,0x58})) return 0;
        } else {
            if (!native_identity(attack, 0x25A0570,
                    {0x4D,0x89,0xC1,0x80,0xB9,0xC8,0,0,0,0x01,0x75,0x0C,0x80,0x3D,0xF1,0x85}) ||
                !native_identity(build, 0x25A0810,
                    {0x80,0xB9,0xC8,0,0,0,0x01,0x0F,0x85,0x43,0xB4,0xFF,0xFF,0x80,0x3D,0x50})) return 0;
        }
        return mode;
    }

    void forget_switch() noexcept { switch_supplies = 0; switch_from = switch_to = -1; }

    // Capture only, after the player was validated.
    void restore_switched_slot() noexcept {
        if (switch_from < 0) return;
        std::uintptr_t supplies = 0;
        int selected = -1;
        // Another player or world, or a slot the user picked: nothing to undo.
        const bool held = read_at(player, 0x5B8, supplies) && supplies == switch_supplies &&
            read_at(supplies, 0x10, selected) && selected == switch_to;
        const auto action = slot_restore(held, switch_active, GetTickCount64() - switch_used);
        if (action == SlotRestore::wait) return;
        if (action == SlotRestore::restore) write_at(supplies, 0x10, switch_from);
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
                state.diagnostic = "Gameplay SDK ready for Minecraft 1.26.52.3; waiting for validated world components.";
            }
        }
    }
    diagnostic = state.diagnostic;
    return state.supported;
}

const std::string& BedrockGameplay::diagnostic() const noexcept { return impl_->diagnostic; }

void BedrockGameplay::clear() noexcept {
    impl_->active = false;
    impl_->switched_this_tick = false;
    impl_->frame = {};
    impl_->client = impl_->player = impl_->registry = impl_->block_source = impl_->view_player = 0;
    impl_->render_position = 0;
    impl_->local_entity = 0;
    impl_->pools.clear();
}

void BedrockGameplay::end_tick() noexcept { impl_->active = false; }

bool BedrockGameplay::slot_restore_pending() const noexcept { return impl_->switch_from >= 0; }

void BedrockGameplay::set_auto_select_active(bool active) noexcept { impl_->switch_active = active; }

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

bool BedrockGameplay::capture(void* client, void* local_actor, bool check_visibility) {
    clear();
    auto& state = *impl_;
    if (!state.supported || !client || !local_actor) return false;
    state.thread = GetCurrentThreadId();
    state.client = reinterpret_cast<std::uintptr_t>(client);
    state.player = reinterpret_cast<std::uintptr_t>(local_actor);
    const auto local_target = state.method(state.client, 31);
    if (!local_target || reinterpret_cast<void*(__fastcall*)(void*)>(local_target)(client) != local_actor ||
        !read_at(state.player, 0x10, state.registry) || !read_at(state.player, 0x18, state.local_entity)) {
        state.diagnostic = "Gameplay SDK: current client/player context failed validation.";
        clear();
        return false;
    }
    if (!detail::read_gameplay_pools(state.registry, state.pools)) {
        state.diagnostic = "Gameplay SDK: unsupported EnTT registry layout or required component hashes missing.";
        clear();
        return false;
    }
    if (!state.snapshot(state.player, state.frame.local) || !state.frame.local.player || !state.frame.local.alive) {
        state.diagnostic = "Gameplay SDK: waiting for a living player with valid owner, state and bounds components.";
        clear();
        return false;
    }
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
    state.frame.camera = state.frame.eye;
    state.frame.yaw = rotation.yaw;
    state.frame.pitch = rotation.pitch;
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
    state.restore_switched_slot();
    state.active = true;
    state.diagnostic = state.frame.projection_ready ? "Gameplay SDK: world snapshot and camera ready." :
        "Gameplay SDK: world snapshot ready; camera profile failed validation, visual projection withheld.";
    return true;
}

bool BedrockGameplay::rotate(float yaw, float pitch) {
    auto& state = *impl_;
    if (!state.on_tick() || !std::isfinite(yaw) || !std::isfinite(pitch)) return false;
    yaw = std::remainder(yaw, 360.0F);
    pitch = std::clamp(pitch, -90.0F, 90.0F);
    const auto rotation = state.component(state.local_entity, rotation_hash, 16);
    const std::array<float, 4> values{pitch, yaw, pitch, yaw};
    if (!write_at(rotation, 0, values)) return false;
    if (const auto head = state.component(state.local_entity, head_hash, 8))
        write_at(head, 0, std::array<float, 2>{yaw, yaw});
    if (const auto body = state.component(state.local_entity, body_hash, 8))
        write_at(body, 0, std::array<float, 2>{yaw, yaw});
    state.frame.yaw = yaw;
    state.frame.pitch = pitch;
    return true;
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

bool BedrockGameplay::replaceable(BlockPos position) const {
    if (!impl_->on_tick()) return false;
    const auto block = impl_->block(position);
    return block && (block->id == 0 || (block->id >= 8 && block->id <= 11));
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
