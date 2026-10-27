#include <medicon/memory/pattern.hpp>

#include <bit>
#include <cctype>
#include <limits>
#include <utility>

namespace medicon::memory {
namespace {

int hex_value(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool is_space(char value) noexcept {
    return std::isspace(static_cast<unsigned char>(value)) != 0;
}

} // namespace

std::optional<Pattern> Pattern::parse(std::string_view text, std::string* error) {
    if (error) error->clear();
    auto fail = [error](std::string message) -> std::optional<Pattern> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };
    std::vector<PatternByte> bytes;
    bool has_literal = false;
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        while (cursor < text.size() && is_space(text[cursor])) ++cursor;
        if (cursor == text.size()) break;
        const auto start = cursor;
        while (cursor < text.size() && !is_space(text[cursor])) ++cursor;
        const auto token = text.substr(start, cursor - start);
        if (token == "?" || token == "??") {
            bytes.push_back({0, true});
            continue;
        }
        if (token.size() != 2 || hex_value(token[0]) < 0 || hex_value(token[1]) < 0) {
            return fail("Invalid signature token at byte " + std::to_string(start) + ": " + std::string(token));
        }
        bytes.push_back({static_cast<std::uint8_t>((hex_value(token[0]) << 4) | hex_value(token[1])), false});
        has_literal = true;
    }
    if (bytes.empty()) return fail("Signature is empty");
    if (!has_literal) return fail("Signature must contain at least one literal byte");
    return Pattern(std::move(bytes));
}

bool AddressRange::contains(std::uintptr_t candidate, std::size_t length) const noexcept {
    if (size == 0 || size - 1 > std::numeric_limits<std::uintptr_t>::max() - address || candidate < address) return false;
    const auto distance = candidate - address;
    return distance < size && length <= size - distance;
}

ScanResult find_unique(std::span<const MemoryRegion> regions, const Pattern& pattern) {
    ScanResult result;
    const auto bytes = pattern.bytes();
    for (std::size_t region_index = 0; region_index < regions.size(); ++region_index) {
        const auto& region = regions[region_index];
        if (bytes.empty() || bytes.size() > region.bytes.size()) continue;
        // Reject overflowing logical ranges before forming an address.
        if (region.bytes.size() - 1 > std::numeric_limits<std::uintptr_t>::max() - region.address) continue;
        const auto last = region.bytes.size() - bytes.size();
        for (std::size_t offset = 0; offset <= last; ++offset) {
            bool matches = true;
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                if (!bytes[i].wildcard && std::to_integer<std::uint8_t>(region.bytes[offset + i]) != bytes[i].value) {
                    matches = false;
                    break;
                }
            }
            if (!matches) continue;
            if (++result.matches == 2) {
                result.status = ScanStatus::ambiguous;
                result.address = 0;
                result.region_index = 0;
                result.offset = 0;
                return result;
            }
            result.status = ScanStatus::unique;
            result.address = region.address + offset;
            result.region_index = region_index;
            result.offset = offset;
        }
    }
    return result;
}

std::optional<std::uintptr_t> resolve_relative32(
    const MemoryRegion& region,
    std::size_t match_offset,
    std::size_t displacement_offset,
    AddressRange permitted_targets) noexcept {
    constexpr std::size_t width = sizeof(std::int32_t);
    if (match_offset > region.bytes.size() || displacement_offset > region.bytes.size() - match_offset) {
        return std::nullopt;
    }
    const auto position = match_offset + displacement_offset;
    if (width > region.bytes.size() - position) return std::nullopt;
    const auto next_offset = position + width;
    if (next_offset > std::numeric_limits<std::uintptr_t>::max() - region.address) return std::nullopt;
    std::uint32_t raw = 0;
    for (std::size_t i = 0; i < width; ++i) {
        raw |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(region.bytes[position + i])) << (i * 8);
    }
    const auto displacement = static_cast<std::int64_t>(std::bit_cast<std::int32_t>(raw));
    const auto next_address = region.address + next_offset;
    std::uintptr_t target = next_address;
    if (displacement < 0) {
        const auto magnitude = static_cast<std::uint64_t>(-displacement);
        if (magnitude > next_address) return std::nullopt;
        target -= static_cast<std::uintptr_t>(magnitude);
    } else {
        const auto magnitude = static_cast<std::uint64_t>(displacement);
        if (magnitude > std::numeric_limits<std::uintptr_t>::max() - next_address) return std::nullopt;
        target += static_cast<std::uintptr_t>(magnitude);
    }
    if (!permitted_targets.contains(target)) return std::nullopt;
    return target;
}

} // namespace medicon::memory
