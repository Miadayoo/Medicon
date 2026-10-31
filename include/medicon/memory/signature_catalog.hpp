#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace medicon::memory {

enum class SignatureResolution { direct, relative32 };

struct SignatureDefinition {
    std::string_view name{};
    std::string_view pattern{};
    SignatureResolution resolution{SignatureResolution::direct};
    std::size_t displacement_offset{};
    std::size_t source_line{};
    std::string_view upstream_version_hint{};
    bool upstream_uncertain{};
};

struct SignatureSource {
    std::string_view repository;
    std::string_view commit;
    std::string_view path;
    std::string_view license_status;
};

[[nodiscard]] std::span<const SignatureDefinition> solstice_signatures() noexcept;
[[nodiscard]] const SignatureSource& solstice_signature_source() noexcept;

// Medicon's own auth-flow signatures for 1.26.52.03 (docs/alt-manager-*). These
// are not verified against a running game; every entry is upstream_uncertain.
[[nodiscard]] std::span<const SignatureDefinition> medicon_auth_signatures() noexcept;
[[nodiscard]] const SignatureSource& medicon_auth_signature_source() noexcept;

} // namespace medicon::memory
