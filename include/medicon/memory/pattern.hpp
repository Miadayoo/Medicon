#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace medicon::memory {

struct PatternByte {
    std::uint8_t value{};
    bool wildcard{};
};

class Pattern {
public:
    // Space-separated hex bytes and ? / ?? wildcards. Empty and all-wildcard
    // patterns are rejected so accidental placeholder signatures fail closed.
    static std::optional<Pattern> parse(std::string_view text, std::string* error = nullptr);
    [[nodiscard]] std::span<const PatternByte> bytes() const noexcept { return bytes_; }

private:
    explicit Pattern(std::vector<PatternByte> bytes) : bytes_(std::move(bytes)) {}
    std::vector<PatternByte> bytes_;
};

struct AddressRange {
    std::uintptr_t address{};
    std::size_t size{};

    [[nodiscard]] bool contains(std::uintptr_t candidate, std::size_t length = 1) const noexcept;
};

// The caller owns the bytes and guarantees that they remain readable during a
// scan. The logical address permits testing without dereferencing game memory.
struct MemoryRegion {
    std::span<const std::byte> bytes;
    std::uintptr_t address{};
};

enum class ScanStatus { not_found, unique, ambiguous };

struct ScanResult {
    ScanStatus status{ScanStatus::not_found};
    std::uintptr_t address{};
    std::size_t region_index{};
    std::size_t offset{};
    std::size_t matches{}; // Capped at two; two means non-unique.
};

[[nodiscard]] ScanResult find_unique(std::span<const MemoryRegion> regions, const Pattern& pattern);

// Resolve a signed little-endian disp32 at match + displacement_offset.
// Both the displacement read and final address are checked; no target is read.
[[nodiscard]] std::optional<std::uintptr_t> resolve_relative32(
    const MemoryRegion& region,
    std::size_t match_offset,
    std::size_t displacement_offset,
    AddressRange permitted_targets) noexcept;

} // namespace medicon::memory
