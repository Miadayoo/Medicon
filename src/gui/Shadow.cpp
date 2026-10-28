#include <medicon/gui/Shadow.hpp>
#include <medicon/gui/Draw.hpp>
#include <medicon/gui/Theme.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

// Model: the shadow is the shape convolved with an isotropic Gaussian (CSS
// box-shadow semantics; blur radius = 2 sigma, https://dbaron.org/log/20110225-blur-radius).
// For a rounded rectangle the convolution separates per scanline:
//   V(x, y) = integral g(y - t) [Phi((x - l(t))/s) - Phi((x - r(t))/s)] dt
// where [l(t), r(t)] is the shape's extent on scanline t. The straight middle
// span is closed form; the corner spans are integrated numerically. This is the
// same decomposition as Evan Wallace's "fast rounded rectangle shadows" (CC0,
// https://madebyevan.com/shaders/fast-rounded-rectangle-shadows/), written from
// the description; it is evaluated exactly here rather than with his 4-sample
// approximation, and erf is clamped as in https://github.com/linebender/vello/pull/1422.

namespace medicon::gui {
namespace {

constexpr float sqrt_half = 0.70710678118F;
constexpr float inv_sqrt_two_pi = 0.39894228040F;
constexpr float min_sigma = 0.5F;
constexpr float extent_sigmas = 3.0F;     // drawn extent (spec 5.1)
constexpr float integrate_sigmas = 4.0F;  // integration window
constexpr std::size_t cache_limit = 96;

// Abramowitz & Stegun 7.1.26, |error| <= 1.5e-7.
float erf_approx(float x) {
    const float sign = x < 0.0F ? -1.0F : 1.0F;
    x = std::fabs(x);
    const float t = 1.0F / (1.0F + 0.3275911F * x);
    const float poly = ((((1.061405429F * t - 1.453152027F) * t) + 1.421413741F) * t - 0.284496736F) * t + 0.254829592F;
    return sign * (1.0F - poly * t * std::exp(-x * x));
}

float inset(float radius, float distance_from_edge) {
    if (radius <= 0.0F || distance_from_edge >= radius) return 0.0F;
    const float d = radius - std::max(0.0F, distance_from_edge);
    return radius - std::sqrt(std::max(0.0F, radius * radius - d * d));
}

CornerRadii clamp_radii(float w, float h, CornerRadii r) {
    const float limit = std::max(0.0F, std::min(w, h) * 0.5F);
    r.top_left = std::clamp(r.top_left, 0.0F, limit);
    r.top_right = std::clamp(r.top_right, 0.0F, limit);
    r.bottom_right = std::clamp(r.bottom_right, 0.0F, limit);
    r.bottom_left = std::clamp(r.bottom_left, 0.0F, limit);
    return r;
}

void add_range(std::vector<float>& lines, float from, float to, float step) {
    if (to < from) return;
    const int count = std::max(1, static_cast<int>(std::ceil((to - from) / step)));
    for (int k = 0; k <= count; ++k) lines.push_back(from + (to - from) * static_cast<float>(k) / static_cast<float>(count));
}

void add_pixel_lines(std::vector<float>& lines, float from, float to) {
    for (float v = std::ceil(from); v <= to; v += 1.0F) lines.push_back(v);
    lines.push_back(from);
    lines.push_back(to);
}

void finalize(std::vector<float>& lines, float low, float high) {
    for (auto& v : lines) v = std::clamp(v, low, high);
    lines.push_back(low);
    lines.push_back(high);
    std::sort(lines.begin(), lines.end());
    std::vector<float> unique;
    for (const float v : lines) {
        if (unique.empty() || v - unique.back() > 0.05F) unique.push_back(v);
    }
    lines.swap(unique);
}

std::int32_t quantize(float value) { return static_cast<std::int32_t>(std::lround(value * 16.0F)); }

} // namespace

float normal_cdf(float z) {
    z = std::clamp(z, -10.0F, 10.0F);
    return 0.5F * (1.0F + erf_approx(z * sqrt_half));
}

float blurred_rounded_rect(float w, float h, const CornerRadii& input, float sigma, float x, float y) {
    if (w <= 0.0F || h <= 0.0F) return 0.0F;
    sigma = std::max(sigma, min_sigma);
    const CornerRadii r = clamp_radii(w, h, input);
    const float inv = 1.0F / sigma;
    const auto horizontal = [&](float left, float right) {
        return normal_cdf((x - left) * inv) - normal_cdf((x - right) * inv);
    };
    const float top_end = std::max(r.top_left, r.top_right);
    const float bottom_start = h - std::max(r.bottom_left, r.bottom_right);
    const float window = integrate_sigmas * sigma;
    const float max_radius = std::max({r.top_left, r.top_right, r.bottom_left, r.bottom_right});

    // Far from the left/right edges every scanline is fully covered horizontally.
    if (x - max_radius >= window && (w - max_radius) - x >= window) {
        return normal_cdf(y * inv) - normal_cdf((y - h) * inv);
    }

    float value = horizontal(0.0F, w) * (normal_cdf((y - top_end) * inv) - normal_cdf((y - bottom_start) * inv));
    const float norm = inv_sqrt_two_pi * inv;
    const auto integrate = [&](float from, float to, bool top) {
        from = std::max(from, y - window);
        to = std::min(to, y + window);
        if (to <= from) return;
        const float step = std::min(0.5F, sigma * 0.5F);
        const int count = std::max(2, static_cast<int>(std::ceil((to - from) / step)));
        const float dt = (to - from) / static_cast<float>(count);
        for (int k = 0; k < count; ++k) {
            const float t = from + (static_cast<float>(k) + 0.5F) * dt;
            const float edge = top ? t : h - t;
            const float left = inset(top ? r.top_left : r.bottom_left, edge);
            const float right = w - inset(top ? r.top_right : r.bottom_right, edge);
            const float u = (y - t) * inv;
            value += norm * std::exp(-0.5F * u * u) * horizontal(left, right) * dt;
        }
    };
    integrate(0.0F, top_end, true);
    integrate(bottom_start, h, false);
    return std::clamp(value, 0.0F, 1.0F);
}

float rounded_rect_sdf(float w, float h, const CornerRadii& input, float x, float y) {
    const CornerRadii r = clamp_radii(w, h, input);
    const float cx = w * 0.5F;
    const float cy = h * 0.5F;
    const float px = x - cx;
    const float py = y - cy;
    float radius = px < 0.0F ? (py < 0.0F ? r.top_left : r.bottom_left) : (py < 0.0F ? r.top_right : r.bottom_right);
    const float qx = std::fabs(px) - (cx - radius);
    const float qy = std::fabs(py) - (cy - radius);
    const float outside = std::hypot(std::max(qx, 0.0F), std::max(qy, 0.0F));
    return outside + std::min(std::max(qx, qy), 0.0F) - radius;
}

std::size_t MeshShadowRenderer::KeyHash::operator()(const Key& k) const noexcept {
    std::size_t hash = 1469598103934665603ULL;
    for (const std::int32_t v : {k.w, k.h, k.r0, k.r1, k.r2, k.r3, k.sigma, k.ox, k.oy}) {
        hash ^= static_cast<std::size_t>(static_cast<std::uint32_t>(v));
        hash *= 1099511628211ULL;
    }
    return hash;
}

MeshShadowRenderer::Geometry MeshShadowRenderer::build(float w, float h, const CornerRadii& input,
                                                        const ShadowParams& params) {
    Geometry g;
    g.width = w;
    g.height = h;
    std::vector<float> xs, ys, values;
    const CornerRadii r = clamp_radii(w, h, input);
    const float sigma = std::max(params.sigma, min_sigma);
    const float ext = extent_sigmas * sigma;
    const float step = std::clamp(sigma / 3.0F, 0.75F, 4.0F);
    const float ox = params.offset_x;
    const float oy = params.offset_y;
    const float left_r = std::max(r.top_left, r.bottom_left);
    const float right_r = std::max(r.top_right, r.bottom_right);
    const float top_r = std::max(r.top_left, r.top_right);
    const float bottom_r = std::max(r.bottom_left, r.bottom_right);

    // Dense where the shadow profile varies; pixel lines over the shape's corner
    // squares so the excluded interior follows the anti-aliased outline.
    add_range(xs, ox - ext, ox + left_r + ext, step);
    add_range(xs, ox + w - right_r - ext, ox + w + ext, step);
    add_pixel_lines(xs, 0.0F, left_r);
    add_pixel_lines(xs, w - right_r, w);
    finalize(xs, ox - ext, ox + w + ext);
    add_range(ys, oy - ext, oy + top_r + ext, step);
    add_range(ys, oy + h - bottom_r - ext, oy + h + ext, step);
    add_pixel_lines(ys, 0.0F, top_r);
    add_pixel_lines(ys, h - bottom_r, h);
    finalize(ys, oy - ext, oy + h + ext);

    const std::size_t nx = xs.size();
    const std::size_t ny = ys.size();
    values.resize(nx * ny);
    for (std::size_t j = 0; j < ny; ++j) {
        for (std::size_t i = 0; i < nx; ++i) {
            values[j * nx + i] = blurred_rounded_rect(w, h, r, sigma, xs[i] - ox, ys[j] - oy);
        }
    }
    // Interior grid vertices used to be sent to the GPU even though no triangle
    // referenced them. Keep a compact, shared set of exterior vertices instead.
    constexpr std::uint32_t unused = UINT32_MAX;
    std::vector<std::uint32_t> remap(nx * ny, unused);
    const auto vertex = [&](std::size_t index) {
        if (remap[index] == unused) {
            remap[index] = static_cast<std::uint32_t>(g.vertices.size());
            g.vertices.push_back({xs[index % nx], ys[index / nx], values[index]});
        }
        return remap[index];
    };
    const auto quad = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
        g.indices.insert(g.indices.end(), {a, b, c, a, c, d});
    };
    constexpr float visible = 0.25F / 255.0F;
    for (std::size_t j = 0; j + 1 < ny; ++j) {
        for (std::size_t i = 0; i + 1 < nx; ++i) {
            const std::size_t a = j * nx + i;
            const float v[4]{values[a], values[a + 1], values[a + nx + 1], values[a + nx]};
            if (std::max({v[0], v[1], v[2], v[3]}) < visible) continue;
            const float x0 = xs[i], x1 = xs[i + 1], y0 = ys[j], y1 = ys[j + 1];
            const float d[4]{rounded_rect_sdf(w, h, r, x0, y0), rounded_rect_sdf(w, h, r, x1, y0),
                             rounded_rect_sdf(w, h, r, x1, y1), rounded_rect_sdf(w, h, r, x0, y1)};
            const float max_d = std::max({d[0], d[1], d[2], d[3]});
            const float min_d = std::min({d[0], d[1], d[2], d[3]});
            if (max_d <= 1e-4F) continue; // inside the shape: excluded
            // Only the pixel cells over the four corner squares can cut the curved
            // outline; every other cell lies entirely on one side of a straight edge.
            const auto overlaps = [&](float ax0, float ay0, float ax1, float ay1) {
                return x0 < ax1 && x1 > ax0 && y0 < ay1 && y1 > ay0;
            };
            const bool corner = overlaps(0.0F, 0.0F, r.top_left, r.top_left) ||
                                overlaps(w - r.top_right, 0.0F, w, r.top_right) ||
                                overlaps(w - r.bottom_right, h - r.bottom_right, w, h) ||
                                overlaps(0.0F, h - r.bottom_left, r.bottom_left, h);
            const float center = rounded_rect_sdf(w, h, r, (x0 + x1) * 0.5F, (y0 + y1) * 0.5F);
            const float half_diagonal = 0.5F * std::hypot(x1 - x0, y1 - y0);
            if (min_d >= -1e-4F && (!corner || center >= half_diagonal)) {
                quad(vertex(a), vertex(a + 1), vertex(a + nx + 1), vertex(a + nx));
                continue;
            }
            // Cell on the curved outline (1 px): attenuate by the pixel's coverage.
            const float outside = 1.0F - std::clamp(0.5F - center, 0.0F, 1.0F);
            if (outside <= 0.0F) continue;
            const auto first = static_cast<std::uint32_t>(g.vertices.size());
            g.vertices.insert(g.vertices.end(), {{x0, y0, v[0] * outside}, {x1, y0, v[1] * outside},
                                                  {x1, y1, v[2] * outside}, {x0, y1, v[3] * outside}});
            quad(first, first + 1, first + 2, first + 3);
        }
    }
    return g;
}

void MeshShadowRenderer::insert(const Key& key, Geometry geometry) {
    if (cache_.size() >= cache_limit) {
        cache_.erase(lru_.back());
        lru_.pop_back();
    }
    lru_.push_front(key);
    auto& entry = cache_[key];
    entry.first = std::move(geometry);
    entry.second = lru_.begin();
}

void MeshShadowRenderer::collect() {
    for (auto it = building_.begin(); it != building_.end();) {
        if (it->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        insert(it->first, it->second.get());
        it = building_.erase(it);
    }
}

const MeshShadowRenderer::Geometry* MeshShadowRenderer::geometry(const Shape& shape) {
    if (!building_.empty()) collect();
    if (const auto found = cache_.find(shape.key); found != cache_.end()) {
        lru_.splice(lru_.begin(), lru_, found->second.second);
        return &found->second.first;
    }
    if (building_.contains(shape.key)) return nullptr;
    ++stats_.geometry_builds;
    insert(shape.key, build(shape.mesh_w, shape.mesh_h, shape.radii, shape.params));
    return &cache_.find(shape.key)->second.first;
}

MeshShadowRenderer::Shape MeshShadowRenderer::shape(float w, float h, const CornerRadii& radii, const ShadowParams& params) {
    Shape result;
    result.params = params;
    result.params.sigma = std::max(params.sigma, min_sigma);
    const auto& p = result.params;
    result.radii = clamp_radii(w, h, radii);
    const auto& r = result.radii;
    const float radius = std::max({r.top_left, r.top_right, r.bottom_right, r.bottom_left});
    // Once opposite corner profiles are separated by 4 sigma on either side,
    // they no longer overlap our integration window. Cache that smallest mesh
    // and stretch only its straight middle span. The remaining Gaussian tail
    // error is well below one 8-bit alpha level, even at full opacity. Small
    // rectangles and large blurs keep their dimension-specific exact meshes.
    // Offset is included to keep the unshifted excluded outline in each band.
    result.mesh_w = std::min(w, 2.0F * radius + 8.0F * p.sigma + 2.0F * std::fabs(p.offset_x) + 2.0F);
    result.mesh_h = std::min(h, 2.0F * radius + 8.0F * p.sigma + 2.0F * std::fabs(p.offset_y) + 2.0F);
    result.key = Key{quantize(result.mesh_w), quantize(result.mesh_h), quantize(r.top_left), quantize(r.top_right),
                     quantize(r.bottom_right), quantize(r.bottom_left), quantize(p.sigma),
                     quantize(p.offset_x), quantize(p.offset_y)};
    return result;
}

void MeshShadowRenderer::prepare(ImVec2 size, const CornerRadii& radii, const ShadowParams& params) {
    if (!(size.x > 0.0F) || !(size.y > 0.0F) || !(params.opacity > 0.0F)) return;
    if (!building_.empty()) collect();
    const Shape s = shape(size.x, size.y, radii, params);
    if (cache_.contains(s.key) || building_.contains(s.key)) return;
    ++stats_.geometry_builds;
    // build() is a pure function of its arguments; the worker shares nothing.
    building_.emplace(s.key, std::async(std::launch::async, &MeshShadowRenderer::build, s.mesh_w, s.mesh_h, s.radii, s.params));
}

void MeshShadowRenderer::draw(ImDrawList* list, ImVec2 min, ImVec2 max, const CornerRadii& radii,
                              const ShadowParams& params, float alpha) {
    const float w = max.x - min.x;
    const float h = max.y - min.y;
    const float strength = std::clamp(params.opacity * alpha, 0.0F, 1.0F);
    if (!list || w <= 0.0F || h <= 0.0F || strength <= 0.0F) return;
    const float ext = extent_sigmas * std::max(params.sigma, min_sigma);
    const ImVec2 clip_min = list->GetClipRectMin(), clip_max = list->GetClipRectMax();
    if (max.x + params.offset_x + ext <= clip_min.x || min.x + params.offset_x - ext >= clip_max.x ||
        max.y + params.offset_y + ext <= clip_min.y || min.y + params.offset_y - ext >= clip_max.y) return;

    const Geometry* mesh = geometry(shape(w, h, radii, params));
    if (!mesh) return;
    const float dx = w - mesh->width, dy = h - mesh->height;
    emit(list, *mesh, params, strength, [&](const Vertex& vertex) {
        const float x = vertex.x + (vertex.x > mesh->width * 0.5F ? dx : 0.0F);
        const float y = vertex.y + (vertex.y > mesh->height * 0.5F ? dy : 0.0F);
        return ImVec2(min.x + x, min.y + y);
    });
}

void MeshShadowRenderer::draw_stretched(ImDrawList* list, ImVec2 min, ImVec2 max, ImVec2 source,
                                        const CornerRadii& radii, const ShadowParams& params, float alpha) {
    const float w = max.x - min.x;
    const float h = max.y - min.y;
    const float strength = std::clamp(params.opacity * alpha, 0.0F, 1.0F);
    if (!list || w <= 0.0F || h <= 0.0F || !(source.x > 0.0F) || !(source.y > 0.0F) || strength <= 0.0F) return;
    const float k = h / source.y;
    const float ext = extent_sigmas * std::max(params.sigma, min_sigma) * k;
    const ImVec2 clip_min = list->GetClipRectMin(), clip_max = list->GetClipRectMax();
    if (max.x + params.offset_x * k + ext <= clip_min.x || min.x + params.offset_x * k - ext >= clip_max.x ||
        max.y + params.offset_y * k + ext <= clip_min.y || min.y + params.offset_y * k - ext >= clip_max.y) return;

    const Shape s = shape(source.x, source.y, radii, params);
    if (!building_.empty()) collect();
    const auto found = cache_.find(s.key);
    if (found == cache_.end()) return;
    lru_.splice(lru_.begin(), lru_, found->second.second);
    const Geometry& g = found->second.first;
    // Source px: the mesh's straight middle widened back to `source`. Then
    // scale to the target height and widen the middle again to the target width.
    const float source_dx = source.x - g.width, source_dy = source.y - g.height;
    const float stretch = std::max(0.0F, w - source.x * k);
    emit(list, g, params, strength, [&](const Vertex& vertex) {
        const bool right = vertex.x > g.width * 0.5F;
        const float x = (vertex.x + (right ? source_dx : 0.0F)) * k + (right ? stretch : 0.0F);
        const float y = (vertex.y + (vertex.y > g.height * 0.5F ? source_dy : 0.0F)) * k;
        return ImVec2(min.x + x, min.y + y);
    });
}

template <class Place>
void MeshShadowRenderer::emit(ImDrawList* list, const Geometry& g, const ShadowParams& params, float strength, Place place) {
    const std::size_t vertices = g.vertices.size();
    const std::size_t indices = g.indices.size();
    if (indices == 0 || vertices >= 65536) return;

    if (profiler_) profiler_->begin(list);
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    // The mesh is colourless: a tint changes only the vertex colour, never the cache key.
    const ImU32 rgb = (params.tint ? params.tint : to_u32(theme::shadow)) & ~IM_COL32_A_MASK;
    const float peak = params.tint ? static_cast<float>((params.tint >> IM_COL32_A_SHIFT) & 0xFFu) / 255.0F : theme::shadow.a;
    const auto color = [strength, rgb, peak](float value) {
        const float opacity = peak * std::clamp(value * strength, 0.0F, 1.0F);
        return rgb | (static_cast<ImU32>(std::lround(std::clamp(opacity, 0.0F, 1.0F) * 255.0F)) << IM_COL32_A_SHIFT);
    };
    list->PrimReserve(static_cast<int>(indices), static_cast<int>(vertices));
    const auto base = static_cast<ImDrawIdx>(list->_VtxCurrentIdx);
    for (const auto& vertex : g.vertices) list->PrimWriteVtx(place(vertex), uv, color(vertex.value));
    for (const std::uint32_t index : g.indices) list->PrimWriteIdx(static_cast<ImDrawIdx>(base + index));
    if (profiler_) profiler_->end(list);
    ++stats_.draws;
    stats_.vertices += vertices;
}

} // namespace medicon::gui
