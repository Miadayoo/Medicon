#pragma once

#include <filesystem>
#include <string>

namespace medicon::custom_model {
// Worker only. Extracts and verifies the model resources in the containing
// DLL/EXE, regardless of its name (Medicon.dll or loader's MediconCore.dll).
// Files live in a content-addressed cache; external model overrides are untouched.
// Returns an empty path with a diagnostic on failure.
[[nodiscard]] std::filesystem::path materialize_bundled_neon(
    const std::filesystem::path& cache_root, std::string& diagnostic);
}
