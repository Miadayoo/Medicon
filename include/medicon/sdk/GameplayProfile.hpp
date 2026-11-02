#pragma once
#include <array>
#include <cstdint>
namespace medicon::sdk::bedrock_1_26_60_5 {
// Audited function identities; unported 1.26.52 addresses are rejected.
// Instruction evidence: docs/bedrock-1.26.60.05-gameplay-evidence.json.
inline constexpr auto gameplay_native_entries = std::to_array<std::uintptr_t>({
    0x3769980, 0x376A210, 0x376A630, 0x376B220, 0x376B290, 0x3769340,
    0x3770060, 0x3770040, 0x37702A0, 0x3770000, 0x376B0B0,
    0x2C384E0, 0x2C38640, 0x2C31210, 0xBBE500, 0x2B654F0,
    0x2AD9E0, 0x2ADB80, 0x314140, 0x314310, 0x2A10970, 0x2A0C600,
    0x2FD350, 0x29B1E20, 0x2AE060, 0x36AC810, 0x5007E60, 0x500AFB0,
    0x4BB2960, 0x23A7600, 0x23A7200, 0x16D5470, 0x34012E0, 0xD810A0,
    0x3FE0B60, 0x23A8620, 0x23978B0, 0xBB5820,
    0x10CACD0, 0x2CD9D60, 0x7E6D0
});
[[nodiscard]] constexpr bool known_gameplay_entry(std::uintptr_t rva) noexcept {
    for (const auto entry : gameplay_native_entries) if (entry == rva) return true;
    return false;
}
} // namespace medicon::sdk::bedrock_1_26_60_5
