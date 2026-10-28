#pragma once

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <future>
#include <list>
#include <unordered_map>
#include <vector>

namespace medicon::gui {

struct CornerRadii {
    float top_left = 0.0F;
    float top_right = 0.0F;
    float bottom_right = 0.0F;
    float bottom_left = 0.0F;
    static CornerRadii all(float radius) { return {radius, radius, radius, radius}; }
};

// Gaussian shadow of a rounded rectangle, in physical px. sigma = CSS blur / 2.
struct ShadowParams {
    float opacity = 0.0F;
    float sigma = 7.0F;
    float offset_x = 0.0F;
    float offset_y = 4.0F;
    // Colour and peak alpha; 0 = theme::shadow. A pale tint with no offset is a glow.
    ImU32 tint = 0;
};

// --- Analytic model (pure functions, unit tested) -----------------------------

// Standard normal CDF; argument clamped to +/-10 to avoid overflow for tiny sigma.
float normal_cdf(float z);
// Convolution of the rounded rectangle [0,w]x[0,h] with a 2D Gaussian, at (x, y).
float blurred_rounded_rect(float w, float h, const CornerRadii& radii, float sigma, float x, float y);
// Signed distance to the rounded rectangle [0,w]x[0,h] (negative inside).
float rounded_rect_sdf(float w, float h, const CornerRadii& radii, float x, float y);

// Optional GPU timing hooks (Preview). begin/end bracket every shadow draw.
class ShadowProfiler {
public:
    virtual ~ShadowProfiler() = default;
    virtual void begin(ImDrawList* list) = 0;
    virtual void end(ImDrawList* list) = 0;
};

class IShadowRenderer {
public:
    virtual ~IShadowRenderer() = default;
    // Draws the shadow outside the shape [min,max] only (the shape interior is excluded).
    virtual void draw(ImDrawList* list, ImVec2 min, ImVec2 max, const CornerRadii& radii, const ShadowParams& params,
                      float alpha) = 0;
    // Readies whatever drawing a shape of `size` will need, off the render
    // thread where possible, so its first draw() costs no more than later ones.
    virtual void prepare(ImVec2 size, const CornerRadii& radii, const ShadowParams& params) {
        (void)size, (void)radii, (void)params;
    }
    // Nine-slice: the shadow of a `source` sized shape, scaled by the height
    // ratio and stretched horizontally across its middle to fill [min,max].
    // For shapes that animate: one cached mesh serves every frame. Prepare
    // `source` first; the radii of [min,max] itself are not followed.
    virtual void draw_stretched(ImDrawList* list, ImVec2 min, ImVec2 max, ImVec2 source, const CornerRadii& radii,
                                const ShadowParams& params, float alpha) {
        (void)source;
        draw(list, min, max, radii, params, alpha);
    }
    void set_profiler(ShadowProfiler* profiler) { profiler_ = profiler; }

protected:
    ShadowProfiler* profiler_ = nullptr;
};

// Placeholder that draws nothing.
class NullShadowRenderer final : public IShadowRenderer {
public:
    void draw(ImDrawList*, ImVec2, ImVec2, const CornerRadii&, const ShadowParams&, float) override {}
};

// Method E: the analytic Gaussian is evaluated on the CPU at the vertices of a
// tensor grid that is dense only where the profile changes; only vertices used
// outside the shape are emitted. Long straight spans stretch cached geometry,
// so panel height animations do not repeatedly integrate the same corners.
// The rasteriser interpolates vertex alpha. Needs no texture, shader or render target, so the
// same code runs inside any host ImGui renderer (D3D11, D3D12, ...).
class MeshShadowRenderer final : public IShadowRenderer {
public:
    void draw(ImDrawList* list, ImVec2 min, ImVec2 max, const CornerRadii& radii, const ShadowParams& params,
              float alpha) override;
    // Builds the mesh on a worker thread. Until it lands, draw() skips that
    // shadow rather than build the same mesh again on the render thread.
    void prepare(ImVec2 size, const CornerRadii& radii, const ShadowParams& params) override;
    // Never builds on the render thread: skipped until prepare()'s mesh lands.
    void draw_stretched(ImDrawList* list, ImVec2 min, ImVec2 max, ImVec2 source, const CornerRadii& radii,
                        const ShadowParams& params, float alpha) override;

    struct Stats {
        std::size_t geometry_builds = 0; // CPU mesh evaluations (cache misses)
        std::size_t draws = 0;
        std::size_t vertices = 0;        // emitted in the last frame window
    };
    const Stats& stats() const { return stats_; }
    void reset_stats() { stats_ = {}; }

private:
    struct Key {
        std::int32_t w, h, r0, r1, r2, r3, sigma, ox, oy;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const noexcept;
    };
    struct Vertex {
        float x, y, value;
    };
    struct Geometry {
        float width = 0.0F;
        float height = 0.0F;
        std::vector<Vertex> vertices;
        std::vector<std::uint32_t> indices;
    };

    struct Shape {
        Key key{};
        float mesh_w = 0.0F, mesh_h = 0.0F;
        CornerRadii radii{};
        ShadowParams params{};
    };
    static Shape shape(float w, float h, const CornerRadii& radii, const ShadowParams& params);
    // Null while the mesh is still being built by prepare().
    const Geometry* geometry(const Shape& shape);
    void insert(const Key& key, Geometry geometry);
    void collect();
    static Geometry build(float w, float h, const CornerRadii& radii, const ShadowParams& params);
    // Writes `g` with each mesh vertex moved by place(vertex) (px from the list origin).
    template <class Place>
    void emit(ImDrawList* list, const Geometry& g, const ShadowParams& params, float strength, Place place);

    std::list<Key> lru_;
    std::unordered_map<Key, std::pair<Geometry, std::list<Key>::iterator>, KeyHash> cache_;
    std::unordered_map<Key, std::future<Geometry>, KeyHash> building_;
    Stats stats_{};
};

} // namespace medicon::gui
