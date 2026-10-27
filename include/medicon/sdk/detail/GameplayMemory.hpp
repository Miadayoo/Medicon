#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace medicon::sdk::detail {

// Read-only release-x64 EnTT ABI readers. These never call, mutate, or destroy
// foreign C++ containers. Kept separate so malformed layouts can be tested.
struct GameplayPool { std::uint32_t hash; std::uintptr_t storage; };
bool read_gameplay_pools(std::uintptr_t registry, std::vector<GameplayPool>& output) noexcept;
std::uintptr_t gameplay_component(std::uintptr_t storage, std::uint32_t entity,
                                  std::size_t element_size) noexcept;
bool gameplay_contains(std::uintptr_t storage, std::uint32_t entity) noexcept;

} // namespace medicon::sdk::detail
