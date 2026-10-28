#pragma once

#include <medicon/memory/module_view.hpp>
#include <medicon/sdk/CustomModelStatic.hpp>
#include <medicon/sdk/CustomModelView.hpp>
#include <medicon/custom_model/ModelAsset.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace medicon::sdk::custom_model {

// Physical Windows x64 slots, not reconstructed MSVC classes/STL types.
// Bounded IDA callsites/layout evidence: docs/custom_model_p1_report.md.
using WorldRenderFn = void (*)(void*, void*, const void*, void*);
using MeshImmediateFn = void (*)(void*, void*, const void*, const void*, const void*);
using MeshImmediateViewFn = void (*)(void*, void*, const void*, const void*, std::uint16_t, const void*);
// MeshSubmit reads pointer arguments 1..4, 7, 9. Slots 5/6/8 are unused
// in the bounded body and are forwarded bit-for-bit without invented types.
using MeshSubmitFn = void (*)(void*, void*, const void*, void*, std::uintptr_t, std::uintptr_t,
                             const void*, std::uintptr_t, void*);
using TessBeginFn = void (*)(void*, void*, std::uint8_t, int, std::uint8_t);
using TessVertexFn = void (*)(void*, float, float, float);
struct BoxVec3 { float x{}, y{}, z{}; };
using InterpolatedPositionFn = BoxVec3* (*)(void*, BoxVec3*, float);
using RidingOffsetFn = BoxVec3* (*)(BoxVec3*, void*, float);
using BodyYawFn = float (*)(void*, float);
using HeightOffsetFn = float (*)(void*);
using MaterialLookupFn = void* (*)(void*, void*, const void*);
using MaterialReleaseFn = void (*)(void*);
using BindingCopyFn = void (*)(std::intptr_t, void**, const void*);
using BindingReleaseFn = void (*)(void*);
using BoxMatrix = std::array<float, 16>;
struct BoxVertex { BoxVec3 position, normal; std::array<float, 2> uv; std::uint32_t color; };
[[nodiscard]] const std::array<BoxVertex, 36>& test_box_vertices() noexcept;
[[nodiscard]] BoxMatrix box_world_matrix(const BoxMatrix& base, BoxVec3 position, float yaw) noexcept;
[[nodiscard]] BoxMatrix model_world_matrix(const BoxMatrix& base, BoxVec3 position, float body_yaw, float yaw_offset) noexcept;
[[nodiscard]] BoxMatrix first_person_world_matrix(const BoxMatrix& base,float head_yaw,float pitch) noexcept;

struct BoxTargets {
    WorldRenderFn world{};
    MeshImmediateFn mesh{};
    MeshImmediateViewFn mesh_view{};
    TessBeginFn begin{};
    TessVertexFn vertex{};
    InterpolatedPositionFn position{};
    RidingOffsetFn riding{};
    BodyYawFn yaw{};
    HeightOffsetFn height{};
    MaterialLookupFn material{};
    MaterialReleaseFn release{};
    void* material_group{};
    const void* alpha_key{};
    const void* entity_key{};
    MeshSubmitFn submit{};
    BindingCopyFn copy_binding{};
    BindingReleaseFn release_binding{};
    explicit operator bool() const noexcept;
};
[[nodiscard]] BoxTargets resolve_box_targets(std::uint32_t timestamp, std::uint32_t image_size,
    std::span<const memory::MemoryRegion> regions, memory::AddressRange image, std::string& diagnostic);
[[nodiscard]] BoxTargets locate_box_targets(std::string& diagnostic);

enum class BoxFailure : unsigned { none, context, matrix, pose, texture, tessellator, material, count };
[[nodiscard]] const char* box_failure_name(BoxFailure) noexcept;
struct BoxFrame {
    std::uint64_t worlds{}, submits{}, in_world{}, drawn{}, ticks{};
    std::uint64_t native_submits{}, scoped_submits{}, other_screen{};
    std::array<std::uint64_t, static_cast<unsigned>(BoxFailure::count)> failures{};
};

class BoxRenderer {
public:
    BoxRenderer();
    ~BoxRenderer();
    void configure_static(StaticTargets targets, medicon::custom_model::ModelRegistry& registry);
    void set_static_mode(bool value, bool visible) noexcept;
    void set_animation(bool value) noexcept { animation_enabled_.store(value,std::memory_order_release); }
    struct AnimationFrame { std::uint64_t frames{},cpu_ticks{},draw_ticks{},read_failures{},bulk_batches{},fallback_batches{};unsigned motion{};bool input_ready{}; };
    [[nodiscard]] AnimationFrame take_animation_frame() noexcept;
    [[nodiscard]] StaticProgress static_progress() const noexcept;
    [[nodiscard]] ViewMode view_mode() const noexcept { return view_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool first_person_ready() const noexcept;
    [[nodiscard]] bool custom_model_mode() const noexcept { return static_mode_.load(std::memory_order_acquire); }
    void observe_texture_group(void* group, TextureGetFn original) noexcept;
    // Published before enabling hooks and retained until callbacks drain.
    void configure(BoxTargets targets) noexcept { native_ = targets; }
    void set_ready(bool value) noexcept { ready_.store(value, std::memory_order_release); }
    [[nodiscard]] bool ready() const noexcept { return ready_.load(std::memory_order_acquire); }
    void set_options(bool enabled, unsigned material, float brightness) noexcept;
    [[nodiscard]] bool enabled() const noexcept { return enabled_.load(std::memory_order_acquire) && ready(); }
    [[nodiscard]] BoxFrame take_frame() noexcept;
    [[nodiscard]] bool texture_ready() const noexcept { return texture_ready_.load(std::memory_order_acquire); }
    // Only after disabling native hooks and draining callbacks. Texture handles
    // have native-owned references; actor/context pointers are never cached.
    void release_resources() noexcept;

    // Stack-local scope: no actor/context/texture is retained across callbacks.
    class WorldScope {
    public:
        WorldScope(BoxRenderer& owner, void* context, void* actor, ViewMode view = ViewMode::unknown) noexcept;
        ~WorldScope();
        WorldScope(const WorldScope&) = delete;
        WorldScope& operator=(const WorldScope&) = delete;
    private:
        friend class BoxRenderer;
        BoxRenderer& owner_;
        WorldScope* previous_{};
        void* screen_{};
        std::uintptr_t matrices_{};
        BoxMatrix world_{};
        medicon::custom_model::AnimationInput animation_input_{};
        float distance_squared_{};
        ViewMode view_{ViewMode::unknown};
        bool valid_{}, drawn_{}, drawing_{};
    };
    // Invoke only AFTER forwarding the stock submission. Original trampoline
    // owns mesh finalization/destruction; texture/metadata remain borrowed.
    void after_submit(void* screen, void* tessellator, const void* texture,
                      const void* metadata, MeshImmediateFn original) noexcept;
    void before_mesh_submit(void* render_context, const void* bindings,
                            const void* metadata, MeshImmediateFn original) noexcept;
    void finish_world(MeshImmediateFn original) noexcept;
private:
    struct StaticState;
    std::unique_ptr<StaticState> static_;
    std::atomic_bool static_mode_{false}, static_visible_{false};
    std::atomic_bool animation_enabled_{false};
    std::atomic<ViewMode> view_{ViewMode::unknown};
    void finish_static(WorldScope& scope,MeshImmediateFn original) noexcept;
    void observe_binding(const void* texture, const void* metadata) noexcept;
    void fail(BoxFailure why) noexcept;
    bool prepare(WorldScope& scope, void* context, void* actor) noexcept;
    BoxTargets native_{};
    std::atomic_bool enabled_{false}, ready_{false};
    std::atomic_uint material_{};
    std::atomic<float> brightness_{1.0F};
    alignas(16) std::array<std::byte,40> texture_{};
    alignas(16) std::array<std::byte,40> metadata_{};
    std::atomic_bool texture_ready_{false};
    std::atomic_flag texture_lock_ = ATOMIC_FLAG_INIT;
    std::atomic<std::uint64_t> worlds_{}, submits_{}, in_world_{}, drawn_{}, ticks_{};
    std::atomic<std::uint64_t> native_submits_{}, scoped_submits_{}, other_screen_{};
    std::array<std::atomic<std::uint64_t>, static_cast<unsigned>(BoxFailure::count)> failures_{};
    static thread_local WorldScope* current_;
};
}
