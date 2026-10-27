#include <medicon/memory/signature_catalog.hpp>

namespace medicon::memory {
namespace {
#include "solstice_catalog.inc"
} // namespace

std::span<const SignatureDefinition> solstice_signatures() noexcept { return definitions; }
const SignatureSource& solstice_signature_source() noexcept { return source; }

} // namespace medicon::memory
