#include <medicon/memory/signature_registry.hpp>

#include <algorithm>
#include <utility>

namespace medicon::memory {

void SignatureRegistry::scan(std::span<const SignatureDefinition> definitions,
                             std::span<const MemoryRegion> regions,
                             AddressRange permitted_targets) {
    results_.clear();
    results_.reserve(definitions.size());
    for (const auto& definition : definitions) {
        ResolvedSignature result;
        result.name = definition.name;
        // Mark every occurrence unusable, including a previously resolved one.
        if (auto previous = std::find_if(results_.begin(), results_.end(), [&](const auto& entry) {
                return entry.name == definition.name;
            }); previous != results_.end()) {
            previous->status = SignatureStatus::duplicate_name;
            previous->address.reset();
            previous->detail = "Duplicate signature name";
            result.status = SignatureStatus::duplicate_name;
            result.detail = "Duplicate signature name";
            results_.push_back(std::move(result));
            continue;
        }
        auto pattern = Pattern::parse(definition.pattern, &result.detail);
        if (!pattern) {
            result.status = SignatureStatus::invalid_pattern;
            results_.push_back(std::move(result));
            continue;
        }
        const auto match = find_unique(regions, *pattern);
        if (match.status == ScanStatus::not_found) {
            result.status = SignatureStatus::not_found;
            result.detail = "No match in executable regions";
        } else if (match.status == ScanStatus::ambiguous) {
            result.status = SignatureStatus::ambiguous;
            result.detail = "At least two matches; address withheld";
        } else {
            if (definition.resolution == SignatureResolution::relative32) {
                result.address = resolve_relative32(regions[match.region_index], match.offset,
                                                    definition.displacement_offset, permitted_targets);
            } else if (permitted_targets.contains(match.address)) {
                result.address = match.address;
            }
            result.status = result.address ? SignatureStatus::resolved : SignatureStatus::out_of_bounds;
            if (!result.address) result.detail = "Displacement read or resolved target is outside allowed bounds";
        }
        results_.push_back(std::move(result));
    }
}

const ResolvedSignature* SignatureRegistry::find(std::string_view name) const noexcept {
    const auto match = std::find_if(results_.begin(), results_.end(), [name](const auto& result) {
        return result.name == name;
    });
    return match == results_.end() ? nullptr : &*match;
}

std::optional<std::uintptr_t> SignatureRegistry::address(std::string_view name) const noexcept {
    const auto* result = find(name);
    return result && result->status == SignatureStatus::resolved ? result->address : std::nullopt;
}

std::string_view to_string(SignatureStatus status) noexcept {
    switch (status) {
    case SignatureStatus::resolved: return "resolved";
    case SignatureStatus::invalid_pattern: return "invalid_pattern";
    case SignatureStatus::not_found: return "not_found";
    case SignatureStatus::ambiguous: return "ambiguous";
    case SignatureStatus::out_of_bounds: return "out_of_bounds";
    case SignatureStatus::duplicate_name: return "duplicate_name";
    }
    return "unknown";
}

} // namespace medicon::memory
