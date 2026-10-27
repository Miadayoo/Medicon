#pragma once

#include <medicon/memory/pattern.hpp>
#include <medicon/memory/signature_catalog.hpp>

namespace medicon::memory {

enum class SignatureStatus { resolved, invalid_pattern, not_found, ambiguous, out_of_bounds, duplicate_name };

struct ResolvedSignature {
    std::string name;
    SignatureStatus status{SignatureStatus::not_found};
    std::optional<std::uintptr_t> address;
    std::string detail;
};

// Populate during initialization; hold the registry immutable while consumers
// use it. Missing or non-unique signatures never produce an address.
class SignatureRegistry {
public:
    void scan(std::span<const SignatureDefinition> definitions,
              std::span<const MemoryRegion> regions,
              AddressRange permitted_targets);

    [[nodiscard]] const ResolvedSignature* find(std::string_view name) const noexcept;
    [[nodiscard]] std::optional<std::uintptr_t> address(std::string_view name) const noexcept;
    [[nodiscard]] std::span<const ResolvedSignature> results() const noexcept { return results_; }
    void clear() noexcept { results_.clear(); }

private:
    std::vector<ResolvedSignature> results_;
};

[[nodiscard]] std::string_view to_string(SignatureStatus status) noexcept;

} // namespace medicon::memory
