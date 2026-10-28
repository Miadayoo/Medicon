#pragma once
#include <medicon/custom_model/Animation.hpp>
#include <medicon/custom_model/BundledModel.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace medicon::custom_model {
struct Vertex {
    std::array<float,3> position{}, normal{};
    std::array<float,2> uv{};
    std::uint32_t color{0xFFFFFFFF};
};
struct TextureAsset { std::filesystem::path path; std::string sha256; std::uint32_t width{},height{}; std::string native_path; };
struct MeshPart { std::string name; std::uint32_t texture{}; std::vector<Vertex> vertices; };
struct ModelAsset {
    std::string fbx_sha256, config_sha256;
    float scale{1}, yaw_offset{180};
    std::array<float,3> minimum{}, maximum{};
    std::vector<TextureAsset> textures;
    std::vector<MeshPart> parts;
    std::size_t triangle_count{};
    bool from_cache{};
    std::shared_ptr<const RigAsset> rig;
    std::shared_ptr<AnimationInstance> animation; // worker-allocated; single render writer
};
// File I/O, parsing, validation, triangulation and cache access: worker only.
[[nodiscard]] std::shared_ptr<const ModelAsset> load_model(const std::filesystem::path& directory,
                                                          std::string& diagnostic);
[[nodiscard]] std::string sha256(std::span<const std::byte> bytes);

class ModelRegistry {
public:
    enum class Status { idle, loading, ready, failed };
    ~ModelRegistry();
    // Prefer an external override, then use the embedded default when a cache
    // root is supplied. All extraction/import work stays on the worker.
    void start(std::filesystem::path directory, std::filesystem::path bundled_cache = {});
    // One persistent worker loads the requested character on demand and keeps
    // both CPU generations alive until shutdown. select() never parses/joins.
    void start_catalog(std::array<std::filesystem::path,builtin_model_count> directories,
                       std::filesystem::path bundled_cache, BuiltinModel initial = BuiltinModel::neon);
    bool select(BuiltinModel model) noexcept;
    // Shutdown after render callbacks drain. Keeps published generations alive
    // so releasing a render-thread snapshot never frees a large model there.
    void stop();
    [[nodiscard]] std::shared_ptr<const ModelAsset> snapshot() const noexcept { return std::atomic_load_explicit(&current_,std::memory_order_acquire); }
    [[nodiscard]] Status status() const noexcept { return status_.load(std::memory_order_acquire); }
    [[nodiscard]] std::string diagnostic() const;
private:
    std::shared_ptr<const ModelAsset> current_;
    std::shared_ptr<const ModelAsset> retained_;
    std::array<std::shared_ptr<const ModelAsset>,builtin_model_count> retained_models_;
    std::atomic_bool catalog_active_{};
    unsigned requested_model_{}; // mutex_; paired with wake_ to avoid lost wakeups
    std::uint64_t requested_revision_{};
    std::condition_variable_any wake_;
    std::atomic<Status> status_{Status::idle};
    mutable std::mutex mutex_;
    std::string diagnostic_;
    std::jthread worker_;
};
}
