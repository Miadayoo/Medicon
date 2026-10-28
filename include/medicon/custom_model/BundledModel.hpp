#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace medicon::custom_model {
enum class BuiltinModel : unsigned { neon, chocolat, count };
inline constexpr unsigned builtin_model_count = static_cast<unsigned>(BuiltinModel::count);
[[nodiscard]] std::string_view builtin_model_name(BuiltinModel model) noexcept;
[[nodiscard]] std::string_view builtin_model_directory(BuiltinModel model) noexcept;
// Worker only. Extracts and verifies the model resources in the containing
// DLL/EXE, regardless of its name (Medicon.dll or loader's MediconCore.dll).
// Files live in a content-addressed cache; external model overrides are untouched.
// Returns an empty path with a diagnostic on failure.
[[nodiscard]] std::filesystem::path materialize_bundled_neon(
    const std::filesystem::path& cache_root, std::string& diagnostic);
[[nodiscard]] std::filesystem::path materialize_bundled_model(BuiltinModel model,
    const std::filesystem::path& cache_root, std::string& diagnostic);
}
