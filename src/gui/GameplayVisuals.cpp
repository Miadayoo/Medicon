#include "medicon/gui/GameplayVisuals.hpp"

#include "medicon/core/ModuleManager.hpp"
#include "medicon/gui/Draw.hpp"
#include "medicon/modules/ChinaHatModule.hpp"
#include "medicon/modules/EspModule.hpp"
#include "medicon/sdk/Gameplay.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace medicon::gui {
namespace {

constexpr float pi = 3.14159265358979323846F;

// ESP bloom tuning lives only here; never expose it as module/config settings.
constexpr float esp_glow_width = 40.0F; // physical screen pixels
constexpr float esp_glow_intensity = 1.0F;
constexpr float esp_glow_close_scale = 1.4F;
constexpr float esp_glow_scale_start = 0.25F; // box height / viewport height
constexpr float esp_glow_scale_end = 0.75F;
constexpr int esp_glow_bands = 32;
constexpr int esp_glow_corner_segments = 8;

struct GlowSample { float distance, opacity; };

float esp_glow_scale(ImVec2 minimum, ImVec2 maximum, float viewport_height) {
    // A fixed pixel radius looks smaller as a nearby actor fills the screen.
    // Ease up the spread without changing brightness, with a modest close cap.
    const float height = (maximum.y - minimum.y) / viewport_height;
    const float t = std::clamp((height - esp_glow_scale_start) /
        (esp_glow_scale_end - esp_glow_scale_start), 0.0F, 1.0F);
    return 1.0F + (esp_glow_close_scale - 1.0F) * t * t * (3.0F - 2.0F * t);
}

const auto& esp_glow_profile() {
    static const auto samples = [] {
        std::array<GlowSample, esp_glow_bands + 1> result{};
        for (int i = 0; i <= esp_glow_bands; ++i) {
            const float t = static_cast<float>(i) / esp_glow_bands;
            // More samples near the bright core; vertex alpha interpolates
            // continuously between bands instead of drawing separate strokes.
            const float distance = esp_glow_width * t * std::sqrt(t);
            const float radius = distance / esp_glow_width;
            const auto exponential = [radius](float decay) {
                return std::exp(-radius / decay);
            };
            // Exponential-style bloom: a tight core with a long luminous tail,
            // rather than Gaussian lobes that rapidly vanish away from the line.
            // This is our lightweight ESP approximation, not the plugin's pipeline.
            float energy = 0.48F * exponential(0.04F) +
                           0.26F * exponential(0.15F) +
                           0.22F * exponential(0.35F);
            // Fade the finite mesh boundary with zero slope so the long tail
            // never ends in a visible ring, even over a dark background.
            const float tail = std::clamp((radius - 0.7F) / 0.3F, 0.0F, 1.0F);
            energy *= 1.0F - tail * tail * (3.0F - 2.0F * tail);
            result[i] = {distance, i == esp_glow_bands ? 0.0F
                : -std::expm1(-energy * esp_glow_intensity)};
        }
        return result;
    }();
    return samples;
}

float finite_clamp(float value, float minimum, float maximum, float fallback) {
    return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
}

Color safe_color(Color value) {
    value.r = finite_clamp(value.r, 0.0F, 1.0F, 1.0F);
    value.g = finite_clamp(value.g, 0.0F, 1.0F, 1.0F);
    value.b = finite_clamp(value.b, 0.0F, 1.0F, 1.0F);
    value.a = finite_clamp(value.a, 0.0F, 1.0F, 1.0F);
    return value;
}

bool finite(sdk::Vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool valid_bounds(const sdk::Bounds& bounds) {
    return finite(bounds.min) && finite(bounds.max) &&
           bounds.min.x < bounds.max.x && bounds.min.y < bounds.max.y &&
           bounds.min.z < bounds.max.z;
}

bool contains(const sdk::Bounds& bounds, sdk::Vec3 point) {
    return point.x >= bounds.min.x && point.x <= bounds.max.x &&
           point.y >= bounds.min.y && point.y <= bounds.max.y &&
           point.z >= bounds.min.z && point.z <= bounds.max.z;
}

// Where the game draws an actor this frame relative to its tick position: back
// toward the previous tick by the part of the tick interval not yet elapsed.
sdk::Vec3 drawn_offset(const sdk::ActorSnapshot& actor, float partial_tick) {
    if (!actor.has_previous || !finite(actor.previous_position) || !finite(actor.position)) return {};
    const float back = 1.0F - finite_clamp(partial_tick, 0.0F, 1.0F, 1.0F);
    return {(actor.previous_position.x - actor.position.x) * back,
            (actor.previous_position.y - actor.position.y) * back,
            (actor.previous_position.z - actor.position.z) * back};
}

// The local player sits where the game draws it this frame, its render position.
// The tick snapshot is copied in baseTick, ahead of that tick's movement, so
// interpolating it left the hat a tick behind the model.
sdk::Vec3 local_drawn_offset(const sdk::ActorSnapshot& local, float partial_tick) {
    if (local.has_render_position && finite(local.render_position) && finite(local.position))
        return {local.render_position.x - local.position.x, local.render_position.y - local.position.y,
                local.render_position.z - local.position.z};
    return drawn_offset(local, partial_tick);
}

sdk::Bounds moved(const sdk::Bounds& bounds, sdk::Vec3 offset) {
    return {{bounds.min.x + offset.x, bounds.min.y + offset.y, bounds.min.z + offset.z},
            {bounds.max.x + offset.x, bounds.max.y + offset.y, bounds.max.z + offset.z}};
}

// ImGui clips to the viewport. Reject pathological coordinates before they can
// reach its tessellator when a projected object is almost on the near plane.
bool project(const sdk::GameplayFrame& frame, sdk::Vec3 point, sdk::Vec2 viewport,
             ImVec2& result) {
    sdk::Vec2 screen{};
    if (!sdk::world_to_screen(frame, point, viewport, screen) ||
        !std::isfinite(screen.x) || !std::isfinite(screen.y) ||
        std::fabs(screen.x) > viewport.x * 16.0F ||
        std::fabs(screen.y) > viewport.y * 16.0F) return false;
    result = ImVec2(screen.x, screen.y);
    return true;
}

bool screen_bounds(const sdk::GameplayFrame& frame, const sdk::Bounds& bounds,
                   sdk::Vec2 viewport, ImVec2& minimum, ImVec2& maximum) {
    if (!valid_bounds(bounds)) return false;
    minimum = ImVec2(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
    maximum = ImVec2(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest());
    for (int corner = 0; corner < 8; ++corner) {
        const sdk::Vec3 world{(corner & 1) ? bounds.max.x : bounds.min.x,
                              (corner & 2) ? bounds.max.y : bounds.min.y,
                              (corner & 4) ? bounds.max.z : bounds.min.z};
        ImVec2 screen;
        // Conservatively omit boxes crossing the camera near plane. Connecting
        // corners across that plane would produce an inverted, giant outline.
        if (!project(frame, world, viewport, screen)) return false;
        minimum.x = std::min(minimum.x, screen.x);
        minimum.y = std::min(minimum.y, screen.y);
        maximum.x = std::max(maximum.x, screen.x);
        maximum.y = std::max(maximum.y, screen.y);
    }
    return maximum.x - minimum.x >= 1.0F && maximum.y - minimum.y >= 1.0F;
}

void draw_glow(ImDrawList& draw, ImVec2 minimum, ImVec2 maximum, Color color,
               float line_width, float glow_scale) {
    if (esp_glow_intensity <= 0.0F || color.a <= 0.0F) return;
    constexpr int points_per_corner = esp_glow_corner_segments + 1;
    constexpr int points_per_ring = 4 * points_per_corner;
    static const auto normals = [] {
        std::array<ImVec2, points_per_ring> result{};
        for (int corner = 0; corner < 4; ++corner) {
            for (int segment = 0; segment < points_per_corner; ++segment) {
                const float angle = (static_cast<float>(corner + 2) +
                    static_cast<float>(segment) / esp_glow_corner_segments) * pi * 0.5F;
                result[corner * points_per_corner + segment] = ImVec2(std::cos(angle), std::sin(angle));
            }
        }
        return result;
    }();
    const std::array<ImVec2, 4> corners{minimum, ImVec2(maximum.x, minimum.y), maximum,
                                     ImVec2(minimum.x, maximum.y)};
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    draw.PrimReserve(esp_glow_bands * points_per_ring * 6, (esp_glow_bands + 1) * points_per_ring);
    const unsigned base = draw._VtxCurrentIdx;
    for (const auto& sample : esp_glow_profile()) {
        const float radius = line_width * 0.5F + sample.distance * glow_scale;
        const ImU32 tint = to_u32(color, sample.opacity);
        for (int point = 0; point < points_per_ring; ++point) {
            const auto corner = corners[point / points_per_corner];
            draw.PrimWriteVtx(ImVec2(corner.x + normals[point].x * radius,
                                    corner.y + normals[point].y * radius), uv, tint);
        }
    }
    // A single exterior mesh: shared vertices, rounded light spread and no
    // overlapping strokes. Keep the interior empty even for distant actors.
    for (int band = 0; band < esp_glow_bands; ++band) {
        for (int point = 0; point < points_per_ring; ++point) {
            const int next = (point + 1) % points_per_ring;
            const unsigned inner = base + band * points_per_ring;
            const unsigned outer = inner + points_per_ring;
            draw.PrimWriteIdx(static_cast<ImDrawIdx>(inner + point));
            draw.PrimWriteIdx(static_cast<ImDrawIdx>(inner + next));
            draw.PrimWriteIdx(static_cast<ImDrawIdx>(outer + next));
            draw.PrimWriteIdx(static_cast<ImDrawIdx>(inner + point));
            draw.PrimWriteIdx(static_cast<ImDrawIdx>(outer + next));
            draw.PrimWriteIdx(static_cast<ImDrawIdx>(outer + point));
        }
    }
}

void render_esp(ImDrawList& draw, const EspModule& module, const sdk::GameplayFrame& frame,
                sdk::Vec2 viewport, float partial_tick) {
    const float range = finite_clamp(module.range.get<float>(), 4.0F, 256.0F, 64.0F);
    const float line_width = finite_clamp(module.line_width.get<float>(), 1.0F, 4.0F, 1.5F);
    const float fill_opacity = finite_clamp(module.fill_opacity.get<float>(), 0.0F, 0.5F, 0.08F);
    const Color color = safe_color(module.color.get<Color>());
    if (!finite(frame.local.position) || color.a <= 0.0F) return;

    for (const auto& actor : frame.actors) {
        if (!actor.alive || actor.local || (actor.id != 0 && actor.id == frame.local.id) ||
            (module.players_only.get<bool>() && !actor.player) || !finite(actor.position)) continue;
        const double dx = static_cast<double>(actor.position.x) - frame.local.position.x;
        const double dy = static_cast<double>(actor.position.y) - frame.local.position.y;
        const double dz = static_cast<double>(actor.position.z) - frame.local.position.z;
        if (dx * dx + dy * dy + dz * dz > static_cast<double>(range) * range) continue;

        ImVec2 minimum, maximum;
        if (!screen_bounds(frame, moved(actor.bounds, drawn_offset(actor, partial_tick)), viewport, minimum, maximum))
            continue;
        const float glow_scale = esp_glow_scale(minimum, maximum, viewport.y);
        const float margin = module.glow.get<bool>() ? esp_glow_width * glow_scale + line_width : line_width;
        if (maximum.x < -margin || maximum.y < -margin ||
            minimum.x > viewport.x + margin || minimum.y > viewport.y + margin) continue;

        if (module.filled.get<bool>() && fill_opacity > 0.0F)
            draw.AddRectFilled(minimum, maximum, to_u32(color, fill_opacity));
        if (module.glow.get<bool>()) draw_glow(draw, minimum, maximum, color, line_width, glow_scale);
        draw.AddRect(minimum, maximum, to_u32(color), 0.0F, 0, line_width);
    }
}

void render_hat(ImDrawList& draw, const ChinaHatModule& module, const sdk::GameplayFrame& frame,
                sdk::Vec2 viewport, float seconds, float partial_tick) {
    // Where the model is drawn this frame, not where the last tick left it.
    const auto bounds = moved(frame.local.bounds, local_drawn_offset(frame.local, partial_tick));
    // In first person the camera sits inside the local player's bounds. A hat
    // there would cover the entire view; the cone is intended for third person.
    // The render origin alone can stay at the eye in third person, with the
    // camera offset carried by the view matrix, so use the real camera.
    const auto camera = sdk::camera_position(frame);
    if (!frame.local.alive || !valid_bounds(bounds) || !finite(camera) || contains(bounds, camera)) return;

    const float radius = finite_clamp(module.radius.get<float>(), 0.2F, 2.0F, 0.8F);
    const float height = finite_clamp(module.height.get<float>(), 0.1F, 1.5F, 0.35F);
    const float offset = finite_clamp(module.offset.get<float>(), 0.0F, 1.0F, 0.1F);
    const float opacity = finite_clamp(module.opacity.get<float>(), 0.0F, 1.0F, 0.3F);
    Color color = safe_color(module.color.get<Color>());
    if (module.rainbow.get<bool>()) {
        const float hue = std::isfinite(seconds) ? std::fmod(std::max(0.0F, seconds) * 0.12F, 1.0F) : 0.0F;
        color = hsv_to_rgb({hue, 0.65F, 1.0F}, color.a);
    }
    if (color.a <= 0.0F || opacity <= 0.0F) return;

    const sdk::Vec3 center{bounds.min.x + (bounds.max.x - bounds.min.x) * 0.5F,
                            bounds.max.y + offset,
                            bounds.min.z + (bounds.max.z - bounds.min.z) * 0.5F};
    ImVec2 tip;
    if (!project(frame, {center.x, center.y + height, center.z}, viewport, tip)) return;

    constexpr std::size_t segments = 48;
    std::array<ImVec2, segments> brim{};
    for (std::size_t index = 0; index < segments; ++index) {
        const float angle = 2.0F * pi * static_cast<float>(index) / static_cast<float>(segments);
        if (!project(frame, {center.x + std::cos(angle) * radius, center.y,
                              center.z + std::sin(angle) * radius}, viewport, brim[index])) return;
    }

    // Back-to-front triangles produce a translucent cone instead of a flat
    // ellipse. Sorting by camera distance also keeps its near side consistent.
    std::array<std::size_t, segments> order{};
    std::array<double, segments> distances{};
    for (std::size_t index = 0; index < segments; ++index) {
        order[index] = index;
        const float angle = 2.0F * pi * (static_cast<float>(index) + 0.5F) / static_cast<float>(segments);
        const double dx = static_cast<double>(center.x) + std::cos(angle) * radius - camera.x;
        const double dz = static_cast<double>(center.z) + std::sin(angle) * radius - camera.z;
        distances[index] = dx * dx + dz * dz;
    }
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return distances[a] > distances[b]; });
    for (const auto index : order)
        draw.AddTriangleFilled(tip, brim[index], brim[(index + 1) % segments], to_u32(color, opacity));
    draw.AddPolyline(brim.data(), static_cast<int>(segments), to_u32(color, std::max(opacity, 0.6F)),
                     ImDrawFlags_Closed, 1.25F);

    // A few thin seams make the cone's shape visible without a dense wire mesh.
    for (std::size_t index = 0; index < segments; index += segments / 4)
        draw.AddLine(tip, brim[index], to_u32(color, opacity * 0.5F), 1.0F);
}

} // namespace

void render_gameplay_visuals(ModuleManager& modules, const sdk::GameplayFrame& frame, float seconds,
                             float partial_tick) {
    if (!ImGui::GetCurrentContext() || !frame.ready || !frame.projection_ready) return;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (!std::isfinite(display.x) || !std::isfinite(display.y) || display.x < 1.0F || display.y < 1.0F) return;
    const sdk::Vec2 viewport{display.x, display.y};
    auto& draw = *ImGui::GetBackgroundDrawList();
    draw.PushClipRect(ImVec2(0.0F, 0.0F), display, true);
    if (auto* module = dynamic_cast<EspModule*>(modules.find(EspModule::module_id)); module && module->enabled())
        render_esp(draw, *module, frame, viewport, partial_tick);
    if (auto* module = dynamic_cast<ChinaHatModule*>(modules.find(ChinaHatModule::module_id)); module && module->enabled())
        render_hat(draw, *module, frame, viewport, seconds, partial_tick);
    draw.PopClipRect();
}

} // namespace medicon::gui
