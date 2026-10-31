#include <medicon/memory/signature_catalog.hpp>

namespace medicon::memory {
namespace solstice_catalog {
#include "solstice_catalog.inc"
} // namespace solstice_catalog

namespace medicon_auth_catalog {
#include "medicon_auth_catalog.inc"
} // namespace medicon_auth_catalog

std::span<const SignatureDefinition> solstice_signatures() noexcept { return solstice_catalog::definitions; }
const SignatureSource& solstice_signature_source() noexcept { return solstice_catalog::source; }

std::span<const SignatureDefinition> medicon_auth_signatures() noexcept { return medicon_auth_catalog::definitions; }
const SignatureSource& medicon_auth_signature_source() noexcept { return medicon_auth_catalog::source; }

} // namespace medicon::memory
