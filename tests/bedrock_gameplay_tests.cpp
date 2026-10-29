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
    alignas(8) std::array<std::byte, 160> nodes{};
    constexpr std::array<std::uint32_t, 5> hashes{0x1B5D5238, 0xBAC1B3CF, 0x75DF36B7, 0x4F6BA419, 0x85B93800};
    alignas(8) std::array<std::byte, 0x80> storage{};
    for (std::size_t i = 0; i < hashes.size(); ++i) {
        store(nodes, i * 32 + 8, hashes[i]);
        store(nodes, i * 32 + 16, reinterpret_cast<std::uintptr_t>(storage.data()));
    }
    store(registry, 0x68, reinterpret_cast<std::uintptr_t>(nodes.data()));
    store(registry, 0x70, reinterpret_cast<std::uintptr_t>(nodes.data() + nodes.size()));
    store(registry, 0x78, reinterpret_cast<std::uintptr_t>(nodes.data() + nodes.size()));
    std::vector<GameplayPool> copied;
    require(read_gameplay_pools(reinterpret_cast<std::uintptr_t>(registry.data()), copied) && copied.size() == 5,
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
    require(!access.rotate(0, 0) && !access.attack(1) && !access.place_block({}, true) && !access.replaceable({}),
            "native action escaped closed tick scope");
    access.clear();
    require(access.frame().actors.empty() && !access.frame().ready, "world clear retained actor handles");
}

void slot_restore_rules() {
    using medicon::sdk::SlotRestore;
    using medicon::sdk::slot_restore;
    static_assert(slot_restore(true, true, 0) == SlotRestore::wait);
    require(slot_restore(true, true, 999) == SlotRestore::wait, "Scaffold on: block slot kept within the idle second");
    require(slot_restore(true, true, 1000) == SlotRestore::restore, "Scaffold on: slot restored after a second unused");
    require(slot_restore(true, false, 0) == SlotRestore::restore, "Scaffold off: slot restored at once, no idle wait");
    require(slot_restore(false, true, 0) == SlotRestore::forget && slot_restore(false, false, 5000) == SlotRestore::forget,
            "A slot the user picked meanwhile is never overwritten");
}

void slot_restore_inventory() {
    using medicon::sdk::SlotRestore;
    using medicon::sdk::detail::restore_gameplay_slot;
    std::array<std::byte, 0x5C0> player{};
    std::array<std::byte, 0x18> supplies{}, other_supplies{};
    const auto actor = reinterpret_cast<std::uintptr_t>(player.data());
    const auto inventory = reinterpret_cast<std::uintptr_t>(supplies.data());
    const auto selected = [&] {
        int slot = -1;
        std::memcpy(&slot, supplies.data() + 0x10, sizeof(slot));
        return slot;
    };
    store(player, 0x5B8, inventory);
    // UI slots 3 -> 1 -> OFF -> 3 (the SDK uses zero-based indices).
    store(supplies, 0x10, 0);
    require(restore_gameplay_slot(actor, inventory, 2, 0, true, 999) == SlotRestore::wait && selected() == 0,
            "active Scaffold did not keep its block slot");
    require(restore_gameplay_slot(actor, inventory, 2, 0, false, 0) == SlotRestore::restore && selected() == 2,
            "disabling Scaffold did not restore slot 3 from slot 1");

    store(supplies, 0x10, 0);
    store(player, 0x5B8, std::uintptr_t{1});
    require(restore_gameplay_slot(actor, inventory, 2, 0, false, 0) == SlotRestore::wait,
            "unreadable inventory discarded Scaffold's original slot");
    store(player, 0x5B8, std::uintptr_t{0});
    require(restore_gameplay_slot(actor, inventory, 2, 0, false, 0) == SlotRestore::wait,
            "temporarily absent inventory discarded Scaffold's original slot");
    store(player, 0x5B8, inventory);
    require(restore_gameplay_slot(actor, inventory, 2, 0, false, 0) == SlotRestore::restore && selected() == 2,
            "Scaffold did not restore the original slot after inventory recovery");

    store(supplies, 0x10, 4);
    require(restore_gameplay_slot(actor, inventory, 2, 0, false, 0) == SlotRestore::forget && selected() == 4,
            "slot restoration overwrote a manual selection");
    store(player, 0x5B8, reinterpret_cast<std::uintptr_t>(other_supplies.data()));
    require(restore_gameplay_slot(actor, inventory, 2, 0, false, 0) == SlotRestore::forget,
            "slot restoration followed an inventory into another world/player");

    struct Page {
        void* address = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        ~Page() { if (address) VirtualFree(address, 0, MEM_RELEASE); }
    } page;
    require(page.address != nullptr, "slot fixture allocation failed");
    const auto read_only_inventory = reinterpret_cast<std::uintptr_t>(page.address);
    store(player, 0x5B8, read_only_inventory);
    DWORD previous = 0;
    require(VirtualProtect(page.address, 4096, PAGE_READONLY, &previous), "slot fixture protection failed");
    require(restore_gameplay_slot(actor, read_only_inventory, 2, 0, false, 0) == SlotRestore::wait,
            "failed slot write discarded Scaffold's original slot");
    require(VirtualProtect(page.address, 4096, PAGE_READWRITE, &previous), "slot fixture unprotect failed");
    require(restore_gameplay_slot(actor, read_only_inventory, 2, 0, false, 0) == SlotRestore::restore &&
            *reinterpret_cast<int*>(static_cast<std::byte*>(page.address) + 0x10) == 2,
            "Scaffold did not retry a failed slot write");
}
} // namespace

int main() {
    try { components(); pools(); native_boundary(); slot_restore_rules(); slot_restore_inventory(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "Bedrock gameplay ABI tests passed\n";
}
