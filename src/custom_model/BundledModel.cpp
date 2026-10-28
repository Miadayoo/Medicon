#include <medicon/custom_model/BundledModel.hpp>
#include <medicon/custom_model/ModelAsset.hpp>
#include <medicon_embedded_model_manifest.hpp>

#include <Windows.h>

#include <atomic>
#include <fstream>
#include <span>
#include <stdexcept>
#include <vector>

namespace medicon::custom_model {
namespace {
std::span<const std::byte> resource(HMODULE module, const embedded_models::File& file) {
    const auto found = FindResourceW(module, MAKEINTRESOURCEW(file.id), RT_RCDATA);
    if (!found || SizeofResource(module, found) != file.size)
        throw std::runtime_error("Missing or truncated bundled model resource: " + std::string(file.path));
    const auto loaded = LoadResource(module, found);
    const auto* bytes = loaded ? static_cast<const std::byte*>(LockResource(loaded)) : nullptr;
    if (!bytes) throw std::runtime_error("Cannot read bundled model resource: " + std::string(file.path));
    const std::span<const std::byte> contents(bytes, file.size);
    if (sha256(contents) != file.sha256)
        throw std::runtime_error("Bundled model resource hash mismatch: " + std::string(file.path));
    return contents;
}
bool inside(const std::filesystem::path& root, const std::filesystem::path& path) {
    auto r = root.begin(), p = path.begin();
    for (; r != root.end(); ++r, ++p)
        if (p == path.end() || _wcsicmp(r->c_str(), p->c_str())) return false;
    return p != path.end();
}
std::filesystem::path checked(const std::filesystem::path& root, const std::filesystem::path& path) {
    const auto resolved = std::filesystem::weakly_canonical(path);
    if (!inside(root, resolved)) throw std::runtime_error("Bundled model cache path escapes its directory.");
    return resolved;
}
bool matches(const std::filesystem::path& path, const embedded_models::File& file) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error ||
        std::filesystem::file_size(path, error) != file.size || error) return false;
    std::ifstream in(path, std::ios::binary);
    std::vector<std::byte> bytes(file.size);
    return in && in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())) &&
        in.peek() == EOF && sha256(bytes) == file.sha256;
}
void publish(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    static std::atomic<unsigned> sequence{};
    auto temporary = path;
    temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed)) + L".tmp";
    try {
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            if (!out || !out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
                throw std::runtime_error("Cannot extract bundled model.");
            out.close();
            if (!out) throw std::runtime_error("Cannot finish extracting bundled model.");
        }
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
            throw std::runtime_error("Cannot publish bundled model cache.");
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}
}

std::filesystem::path materialize_bundled_neon(const std::filesystem::path& cache_root, std::string& diagnostic) {
    return materialize_bundled_model(BuiltinModel::neon, cache_root, diagnostic);
}
std::string_view builtin_model_name(BuiltinModel model) noexcept {
    const auto index = static_cast<unsigned>(model);
    return index < builtin_model_count ? embedded_models::packs[index].name : "Unknown";
}
std::string_view builtin_model_directory(BuiltinModel model) noexcept {
    const auto index = static_cast<unsigned>(model);
    return index < builtin_model_count ? embedded_models::packs[index].directory : "";
}
std::filesystem::path materialize_bundled_model(BuiltinModel model, const std::filesystem::path& cache_root, std::string& diagnostic) {
    try {
        const auto selected = static_cast<unsigned>(model);
        if (selected >= builtin_model_count) throw std::runtime_error("Unknown bundled model.");
        const auto& pack = embedded_models::packs[selected];
        if (cache_root.empty()) throw std::runtime_error("Bundled model cache directory is empty.");
        HMODULE module{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&materialize_bundled_model), &module))
            throw std::runtime_error("Cannot locate bundled model module.");
        // Verify every embedded file before publishing any part of this pack.
        std::vector<std::span<const std::byte>> contents;
        for (const auto& file : pack.files) contents.push_back(resource(module, file));
        std::filesystem::create_directories(cache_root);
        const auto root = std::filesystem::canonical(cache_root);
        const auto directory = checked(root, root / (std::string(pack.cache_prefix) + "-" + pack.sha256));
        std::filesystem::create_directories(directory);
        std::size_t index{};
        for (const auto& file : pack.files) {
            const auto relative = std::filesystem::u8path(file.path);
            if (relative.is_absolute()) throw std::runtime_error("Invalid bundled model resource path.");
            const auto destination = checked(directory, directory / relative);
            std::filesystem::create_directories(destination.parent_path());
            if (!matches(destination, file)) publish(destination, contents[index]);
            ++index;
        }
        // UTF-8: path::string() throws for profile folders outside the ANSI code page.
        const auto shown = directory.u8string();
        diagnostic = "Bundled " + std::string(pack.name) + " assets ready: " + std::string(shown.begin(), shown.end());
        return directory;
    } catch (const std::exception& error) {
        diagnostic = error.what();
        return {};
    }
}
}
