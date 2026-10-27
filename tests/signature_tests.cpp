#include <medicon/memory/module_view.hpp>
#include <medicon/memory/signature_registry.hpp>

#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace {

using namespace medicon::memory;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<std::size_t N>
MemoryRegion region(const std::array<std::uint8_t, N>& bytes, std::uintptr_t address = 0x1000) {
    return {std::as_bytes(std::span(bytes)), address};
}

void parsing_and_scanning() {
    std::string error = "old error";
    auto pattern = Pattern::parse(" \t48 8b ? ?? 90\r\n", &error);
    require(pattern.has_value() && error.empty(), "valid pattern rejected");
    require(pattern->bytes().size() == 5, "wildcards parsed as wrong number of bytes");
    for (const auto invalid : {"", " \t", "? ??", "4", "GG", "4?", "?48", "***", "488B"}) {
        require(!Pattern::parse(invalid, &error) && !error.empty(), "invalid pattern accepted");
    }
    const std::array<std::uint8_t, 7> bytes{0, 0, 0x48, 0x8B, 1, 2, 0x90};
    const std::array regions{region(bytes)};
    const auto found = find_unique(regions, *pattern);
    require(found.status == ScanStatus::unique && found.address == 0x1002 && found.offset == 2,
            "match ending at final byte was missed");
    const auto absent = Pattern::parse("90 91");
    require(find_unique(regions, *absent).status == ScanStatus::not_found, "missing pattern matched");
    const std::array<std::uint8_t, 1> tiny{0x48};
    const std::array short_regions{region(tiny)};
    require(find_unique(short_regions, *pattern).status == ScanStatus::not_found, "short buffer matched");
    const std::array<std::uint8_t, 3> repeated{0xAA, 0xAA, 0xAA};
    const auto overlapping = Pattern::parse("AA AA");
    const std::array repeated_regions{region(repeated)};
    require(find_unique(repeated_regions, *overlapping).status == ScanStatus::ambiguous,
            "overlapping duplicate matches were accepted");
    const std::array separate_regions{region(bytes), region(bytes, 0x2000)};
    require(find_unique(separate_regions, *pattern).matches == 2, "duplicates across regions were accepted");
    const std::array overflow_regions{region(bytes, std::numeric_limits<std::uintptr_t>::max() - 2)};
    require(find_unique(overflow_regions, *pattern).status == ScanStatus::not_found, "logical address overflow accepted");
}

void relative_addresses() {
    const std::array<std::uint8_t, 7> forward{0x48, 0x8D, 0x05, 0x09, 0, 0, 0};
    const std::array<std::uint8_t, 7> backward{0x48, 0x8D, 0x05, 0xF9, 0xFF, 0xFF, 0xFF};
    require(resolve_relative32(region(forward), 0, 3, {0x1000, 0x100}) == 0x1010,
            "positive relative displacement resolved incorrectly");
    require(resolve_relative32(region(backward), 0, 3, {0x1000, 0x100}) == 0x1000,
            "negative relative displacement resolved incorrectly");
    require(!resolve_relative32(region(forward), 0, 3, {0x1000, 0x10}), "out-of-range target accepted");
    require(!resolve_relative32(region(forward), 0, 4, {0x1000, 0x100}), "truncated displacement read accepted");
    require(!resolve_relative32(region(forward), std::numeric_limits<std::size_t>::max(), 3, {0x1000, 0x100}),
            "overflowing match offset accepted");
    require(!resolve_relative32(region(forward), 0, std::numeric_limits<std::size_t>::max(), {0x1000, 0x100}),
            "overflowing displacement offset accepted");
    require(!resolve_relative32(region(backward, 0), 0, 3, {1, 100}), "target outside permitted range accepted");
    const std::array<std::uint8_t, 4> negative{0x00, 0x00, 0x00, 0x80};
    require(!resolve_relative32(region(negative, 1), 0, 0, {0, 100}), "relative subtraction underflow accepted");
    require(!resolve_relative32(region(forward, std::numeric_limits<std::uintptr_t>::max() - 8), 0, 3,
                                {0, std::numeric_limits<std::size_t>::max()}),
            "relative addition overflow accepted");
    require(!AddressRange{std::numeric_limits<std::uintptr_t>::max() - 1, 3}.contains(
                std::numeric_limits<std::uintptr_t>::max()), "overflowing address range accepted");
}

void registry_failures_and_catalog() {
    const std::array<std::uint8_t, 10> bytes{0x48, 0x8D, 0x05, 0x09, 0, 0, 0, 0xAA, 0xAA, 0xBB};
    const std::array regions{region(bytes)};
    const std::array definitions{
        SignatureDefinition{"direct", "BB"},
        SignatureDefinition{"relative", "48 8D 05 ? ? ? ?", SignatureResolution::relative32, 3},
        SignatureDefinition{"missing", "CC"},
        SignatureDefinition{"invalid", "?"},
        SignatureDefinition{"ambiguous", "AA"},
        SignatureDefinition{"bad_relative", "BB", SignatureResolution::relative32, 1},
        SignatureDefinition{"duplicate", "BB"},
        SignatureDefinition{"duplicate", "BB"}
    };
    SignatureRegistry registry;
    registry.scan(definitions, regions, {0x1000, 0x100});
    require(registry.address("direct") == 0x1009 && registry.address("relative") == 0x1010,
            "registry resolved incorrect address");
    require(registry.find("missing")->status == SignatureStatus::not_found && !registry.address("missing"),
            "missing registry entry has an address");
    require(registry.find("invalid")->status == SignatureStatus::invalid_pattern, "invalid registry pattern accepted");
    require(registry.find("ambiguous")->status == SignatureStatus::ambiguous && !registry.address("ambiguous"),
            "ambiguous registry entry has an address");
    require(registry.find("bad_relative")->status == SignatureStatus::out_of_bounds,
            "out-of-bounds relative read accepted by registry");
    require(registry.find("duplicate")->status == SignatureStatus::duplicate_name && !registry.address("duplicate"),
            "duplicate name left a usable registry entry");
    require(!registry.find("unknown") && !registry.address("unknown"), "unknown name returned an entry");
    registry.scan(std::span<const SignatureDefinition>{}, regions, {0x1000, 0x100});
    require(registry.results().empty(), "rescan retained stale results");

    std::unordered_set<std::string_view> names;
    require(solstice_signatures().size() == 63, "pinned catalog count changed without review");
    require(solstice_signature_source().commit == "1d7808f52ef4ba6e34a7fbcc65294907a7761611",
            "catalog source commit changed without review");
    for (const auto& signature : solstice_signatures()) {
        require(names.insert(signature.name).second, "duplicate name in imported catalog");
        require(Pattern::parse(signature.pattern).has_value(), "invalid pattern in imported catalog");
        require(signature.source_line > 0, "signature has no upstream source line");
    }
    require(names.contains("TapSwingAnim") && !names.contains("glm_rotate"), "upstream comments imported incorrectly");
}

void loaded_module_inspection() {
    std::string error;
    const auto module = current_process_module(&error);
#if defined(_WIN32) && defined(_WIN64)
    require(module.has_value() && error.empty(), "current x64 executable could not be inspected");
    for (const auto& executable : module->executable_regions) {
        require(module->image.contains(executable.address, executable.bytes.size()),
                "executable region escaped image bounds");
    }
#else
    require(!module && !error.empty(), "unsupported platform did not report module inspection unavailable");
#endif
}

} // namespace

int main() {
    try {
        parsing_and_scanning();
        relative_addresses();
        registry_failures_and_catalog();
        loaded_module_inspection();
        std::cout << "Signature tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Signature test failed: " << error.what() << '\n';
        return 1;
    }
}
