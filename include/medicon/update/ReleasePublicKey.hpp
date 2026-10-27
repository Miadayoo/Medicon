#pragma once

#include <array>
#include <cstdint>

// Replaced by social-server/tools/release-keygen.ts (npm run release:keygen).
// Until then no release key is embedded and MediconLoader refuses every Core:
// nothing is trusted without a signature (docs/auto-update.md).
namespace medicon::update {

inline constexpr std::array<std::uint8_t, 0> release_key_modulus{};
inline constexpr std::array<std::uint8_t, 0> release_key_exponent{};

} // namespace medicon::update
