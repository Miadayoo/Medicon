#include "medicon/gui/GameplayVisuals.hpp"

#include "medicon/core/ModuleManager.hpp"
#include "medicon/gui/Draw.hpp"
#include "medicon/modules/VisualModules.hpp"
#include "medicon/sdk/Gameplay.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace medicon::gui {
namespace {

constexpr float pi = 3.14159265358979323846F;

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
               float line_width, float spread, float intensity) {
    if (intensity <= 0.0F || color.a <= 0.0F) return;
    const int layers = static_cast<int>(std::ceil(spread));
    const float step = spread / static_cast<float>(layers);
    for (int layer = layers; layer >= 1; --layer) {
        const float distance = step * static_cast<float>(layer);
        const float falloff = 1.0F - distance / (spread + step);
        const float opacity = std::min(0.6F, 0.22F * intensity * falloff * falloff);
        // Each stroke lies outside the main border. The halo never paints the
        // center of the box, including when the actor is small on screen.
        const float expansion = line_width * 0.5F + distance;
        draw.AddRect(ImVec2(minimum.x - expansion, minimum.y - expansion),
                     ImVec2(maximum.x + expansion, maximum.y + expansion),
                     to_u32(color, opacity), 0.0F, 0, step + 0.5F);
    }
}

void render_esp(ImDrawList& draw, const EspModule& module, const sdk::GameplayFrame& frame,
                sdk::Vec2 viewport) {
    const float range = finite_clamp(module.range.get<float>(), 4.0F, 256.0F, 64.0F);
    const float line_width = finite_clamp(module.line_width.get<float>(), 1.0F, 4.0F, 1.5F);
    const float spread = finite_clamp(module.glow_width.get<float>(), 1.0F, 32.0F, 10.0F);
    const float intensity = finite_clamp(module.glow_intensity.get<float>(), 0.0F, 2.5F, 1.0F);
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
        if (!screen_bounds(frame, actor.bounds, viewport, minimum, maximum)) continue;
        const float margin = module.glow.get<bool>() ? spread + line_width : line_width;
        if (maximum.x < -margin || maximum.y < -margin ||
            minimum.x > viewport.x + margin || minimum.y > viewport.y + margin) continue;

        if (module.filled.get<bool>() && fill_opacity > 0.0F)
            draw.AddRectFilled(minimum, maximum, to_u32(color, fill_opacity));
        if (module.glow.get<bool>()) draw_glow(draw, minimum, maximum, color, line_width, spread, intensity);
        draw.AddRect(minimum, maximum, to_u32(color), 0.0F, 0, line_width);
    }
}

void render_hat(ImDrawList& draw, const ChinaHatModule& module, const sdk::GameplayFrame& frame,
                sdk::Vec2 viewport, float seconds) {
    const auto& bounds = frame.local.bounds;
    // In first person the camera sits inside the local player's bounds. A hat
    // there would cover the entire view; the cone is intended for third person.
    if (!frame.local.alive || !valid_bounds(bounds) || !finite(frame.camera) || contains(bounds, frame.camera)) return;

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
        const double dx = static_cast<double>(center.x) + std::cos(angle) * radius - frame.camera.x;
        const double dz = static_cast<double>(center.z) + std::sin(angle) * radius - frame.camera.z;
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

void render_gameplay_visuals(ModuleManager& modules, const sdk::GameplayFrame& frame, float seconds) {
    if (!ImGui::GetCurrentContext() || !frame.ready || !frame.projection_ready) return;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (!std::isfinite(display.x) || !std::isfinite(display.y) || display.x < 1.0F || display.y < 1.0F) return;
    const sdk::Vec2 viewport{display.x, display.y};
    auto& draw = *ImGui::GetBackgroundDrawList();
    draw.PushClipRect(ImVec2(0.0F, 0.0F), display, true);
    if (auto* module = dynamic_cast<EspModule*>(modules.find(EspModule::module_id)); module && module->enabled())
        render_esp(draw, *module, frame, viewport);
    if (auto* module = dynamic_cast<ChinaHatModule*>(modules.find(ChinaHatModule::module_id)); module && module->enabled())
        render_hat(draw, *module, frame, viewport, seconds);
    draw.PopClipRect();
}

} // namespace medicon::gui
