#include <medicon/core/ModuleManager.hpp>
#include <medicon/gui/GameplayVisuals.hpp>
#include <medicon/modules/VisualModules.hpp>
#include <medicon/sdk/Gameplay.hpp>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace medicon;

int checks = 0;
void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

struct ImGuiScope {
    ImGuiScope() {
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1280.0F, 720.0F);
        io.DeltaTime = 1.0F / 60.0F;
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~ImGuiScope() { ImGui::DestroyContext(); }
};

struct Geometry {
    std::vector<ImDrawVert> vertices;
    std::vector<ImDrawIdx> indices;
};

Geometry render(ModuleManager& modules, const sdk::GameplayFrame& frame, float seconds = 0.0F,
                float partial_tick = 1.0F) {
    ImGui::NewFrame();
    gui::render_gameplay_visuals(modules, frame, seconds, partial_tick);
    const auto& draw = *ImGui::GetBackgroundDrawList();
    Geometry result;
    result.vertices.assign(draw.VtxBuffer.begin(), draw.VtxBuffer.end());
    result.indices.assign(draw.IdxBuffer.begin(), draw.IdxBuffer.end());
    ImGui::Render();
    for (const auto& vertex : result.vertices)
        check(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y), "all rendered vertices are finite");
    return result;
}

sdk::GameplayFrame sample_frame() {
    sdk::GameplayFrame frame;
    frame.ready = frame.projection_ready = true;
    frame.view_matrix = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    frame.fov = {1.0F, 1.0F};
    frame.eye = {0.0F, 1.6F, 0.0F};
    frame.camera = frame.eye;
    frame.local.id = 1;
    frame.local.alive = frame.local.local = frame.local.player = true;
    frame.local.position = {0, 0, 0};
    frame.local.bounds = {{-0.3F, 0, -0.3F}, {0.3F, 1.8F, 0.3F}};
    sdk::ActorSnapshot actor;
    actor.id = 2;
    actor.player = actor.alive = true;
    actor.position = {0, 0, -6};
    actor.bounds = {{-0.3F, 0, -6.3F}, {0.3F, 1.8F, -5.7F}};
    frame.actors.push_back(actor);
    return frame;
}

float cross(ImVec2 a, ImVec2 b, ImVec2 c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool covers(const Geometry& geometry, ImVec2 point) {
    for (std::size_t index = 0; index + 2 < geometry.indices.size(); index += 3) {
        const auto a = geometry.vertices[geometry.indices[index]].pos;
        const auto b = geometry.vertices[geometry.indices[index + 1]].pos;
        const auto c = geometry.vertices[geometry.indices[index + 2]].pos;
        const float area = cross(a, b, c);
        if (std::fabs(area) < 0.0001F) continue;
        const float ab = cross(a, b, point), bc = cross(b, c, point), ca = cross(c, a, point);
        if ((ab >= 0 && bc >= 0 && ca >= 0) || (ab <= 0 && bc <= 0 && ca <= 0)) return true;
    }
    return false;
}

float opacity_at(const Geometry& geometry, ImVec2 point) {
    float opacity = 0.0F;
    for (std::size_t index = 0; index + 2 < geometry.indices.size(); index += 3) {
        const auto& a = geometry.vertices[geometry.indices[index]];
        const auto& b = geometry.vertices[geometry.indices[index + 1]];
        const auto& c = geometry.vertices[geometry.indices[index + 2]];
        const float area = cross(a.pos, b.pos, c.pos);
        if (std::abs(area) < 0.00001F) continue;
        const float wa = cross(b.pos, c.pos, point) / area;
        const float wb = cross(c.pos, a.pos, point) / area;
        const float wc = 1.0F - wa - wb;
        if (wa < 0 || wb < 0 || wc < 0) continue;
        const auto alpha = [](const ImDrawVert& vertex) {
            return static_cast<float>((vertex.col >> IM_COL32_A_SHIFT) & 0xffU) / 255.0F;
        };
        opacity = std::max(opacity, wa * alpha(a) + wb * alpha(b) + wc * alpha(c));
    }
    return opacity;
}

void test_esp_glow() {
    ImGuiScope imgui;
    ModuleManager modules;
    auto& esp = modules.add<EspModule>();
    const auto frame = sample_frame();
    check(render(modules, frame).vertices.empty(), "disabled ESP draws no geometry");
    esp.set_enabled(true);
    esp.glow.get<bool>() = false;
    const auto outline = render(modules, frame);
    check(!outline.vertices.empty(), "ESP projects entity bounds");
    esp.glow.get<bool>() = true;
    const auto glow = render(modules, frame);
    check(glow.vertices.size() > outline.vertices.size(), "glow adds halo geometry");
    const auto min_x = [](const Geometry& geometry) {
        float value = std::numeric_limits<float>::max();
        for (const auto& vertex : geometry.vertices) value = std::min(value, vertex.pos.x);
        return value;
    };
    check(min_x(glow) < min_x(outline) - 5.0F, "glow expands outside the outline");
    check(std::any_of(glow.vertices.begin(), glow.vertices.end(), [](const auto& vertex) {
        const auto alpha = (vertex.col >> IM_COL32_A_SHIFT) & 0xffU;
        return alpha > 0 && alpha < 200;
    }), "halo has translucent fading colors");
    sdk::Vec2 corner;
    check(sdk::world_to_screen(frame, {-0.3F, 1.8F, -5.7F}, {1280, 720}, corner), "near box corner projects");
    const auto halo_opacity = [&](float distance) {
        return opacity_at(glow, ImVec2(corner.x - esp.line_width.get<float>() * 0.5F - distance, 401.37F));
    };
    const float core = halo_opacity(1.0F), shoulder = halo_opacity(7.0F), tail = halo_opacity(20.0F);
    check(core > shoulder && shoulder > tail && tail > 0.0F, "bloom fades smoothly from the core into a broad visible tail");
    check(tail > 0.03F && halo_opacity(28.0F) > 0.015F,
          "exponential glow loses its luminous tail too quickly away from the core");
    float previous = core;
    for (float distance = 1.25F; distance <= 44.0F; distance += 0.25F) {
        const float current = halo_opacity(distance);
        check(current <= previous + 0.0001F && previous - current < 0.06F,
              "glow has no separate stroke bands or sudden opacity steps");
        previous = current;
    }
    check(previous < 0.001F, "outer glow fades completely to transparent");
    check(!covers(glow, ImVec2(corner.x - 36.0F, corner.y - 36.0F)), "light spreads radially at corners instead of forming square halos");
    check(glow.vertices.size() < 1600, "glow mesh remains bounded per actor");
    sdk::Vec2 center;
    check(sdk::world_to_screen(frame, {0.0F, 0.9F, -6.0F}, {1280.0F, 720.0F}, center), "box center projects");
    check(!covers(glow, ImVec2(center.x, center.y)), "outline and halo leave the box center clear");
    esp.filled.get<bool>() = true;
    check(covers(render(modules, frame), ImVec2(center.x, center.y)), "optional fill covers box interior");

    // A narrow, distant actor still gets a halo only around its boundary.
    auto small = frame;
    small.actors[0].position = {0, 0, -100};
    small.actors[0].bounds = {{-0.3F, 0, -100.3F}, {0.3F, 1.8F, -99.7F}};
    esp.range.get<float>() = 256.0F;
    esp.filled.get<bool>() = false;
    check(sdk::world_to_screen(small, {0.0F, 0.9F, -100.0F}, {1280.0F, 720.0F}, center), "distant box center projects");
    check(!covers(render(modules, small), ImVec2(center.x, center.y)), "glow leaves distant box center clear");
}

void test_filters_and_projection() {
    ImGuiScope imgui;
    ModuleManager modules;
    auto& esp = modules.add<EspModule>();
    esp.set_enabled(true);
    auto frame = sample_frame();
    frame.ready = false;
    check(render(modules, frame).vertices.empty(), "no world means no visuals");
    frame.ready = true;
    frame.projection_ready = false;
    check(render(modules, frame).vertices.empty(), "missing projection means no visuals");
    frame = sample_frame();
    frame.actors[0].alive = false;
    check(render(modules, frame).vertices.empty(), "dead actors are excluded");
    frame = sample_frame();
    frame.actors[0].local = true;
    check(render(modules, frame).vertices.empty(), "local actor is excluded");
    frame = sample_frame();
    frame.actors[0].bot = true;
    check(render(modules, frame).vertices.empty(), "AntiBot bots are excluded");
    frame = sample_frame();
    frame.actors[0].player = false;
    check(render(modules, frame).vertices.empty(), "Players Only excludes nonplayers");
    esp.players_only.get<bool>() = false;
    check(!render(modules, frame).vertices.empty(), "Players Only can include nonplayers");
    frame = sample_frame();
    esp.range.get<float>() = 4.0F;
    check(render(modules, frame).vertices.empty(), "range excludes distant actors");
    esp.range.get<float>() = 64.0F;
    frame.actors[0].bounds = {{-0.3F, 0, 5.7F}, {0.3F, 1.8F, 6.3F}};
    check(render(modules, frame).vertices.empty(), "behind-camera bounds are excluded");
    frame.actors[0].bounds = {{-0.3F, 0, -0.5F}, {0.3F, 1.8F, 0.5F}};
    check(render(modules, frame).vertices.empty(), "near-plane crossing cannot produce an inverted box");
    frame.actors[0].bounds.min.x = std::numeric_limits<float>::quiet_NaN();
    check(render(modules, frame).vertices.empty(), "invalid bounds cannot reach ImGui");
}

void test_close_esp_glow_spread() {
    ImGuiScope imgui;
    ModuleManager modules;
    auto& esp = modules.add<EspModule>();
    esp.set_enabled(true);
    const auto min_x = [](const Geometry& geometry) {
        float value = std::numeric_limits<float>::max();
        for (const auto& vertex : geometry.vertices) value = std::min(value, vertex.pos.x);
        return value;
    };
    const auto spread = [&](float distance) {
        auto frame = sample_frame();
        auto& actor = frame.actors.front();
        actor.position.z = -distance;
        actor.bounds.min.z = -distance - 0.3F;
        actor.bounds.max.z = -distance + 0.3F;
        esp.glow.get<bool>() = false;
        const auto outline = render(modules, frame);
        esp.glow.get<bool>() = true;
        const auto glow = render(modules, frame);
        check(!glow.vertices.empty(), "nearby ESP keeps its glow");
        return min_x(outline) - min_x(glow);
    };
    const float distant = spread(12), normal = spread(6), approaching = spread(3), near = spread(2);
    check(std::abs(distant - normal) < 0.01F, "distant and normal-size ESP retain the original glow spread");
    check(approaching > normal && near > approaching && near > normal * 1.2F,
          "glow does not gradually widen as the actor fills the screen");
    const float close = spread(1), closest = spread(0.5F);
    check(close > near && close < normal * 1.5F && std::abs(close - closest) < 0.01F,
          "close-range glow must stay capped instead of growing without limit");
}

void test_many_glowing_entities() {
    ImGuiScope imgui;
    // Both native D3D backends support VtxOffset; bloom must keep working when
    // a crowd takes the draw list past the 16-bit index boundary.
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    ModuleManager modules;
    auto& esp = modules.add<EspModule>();
    esp.set_enabled(true);
    auto frame = sample_frame();
    const auto actor = frame.actors.front();
    for (int i = 1; i < 80; ++i) {
        auto copy = actor;
        copy.id = static_cast<std::uintptr_t>(i + 2);
        frame.actors.push_back(copy);
    }
    const auto geometry = render(modules, frame);
    check(geometry.vertices.size() > 65536, "crowd fixture crosses the 16-bit vertex boundary");
    bool used_offset = false;
    const auto* data = ImGui::GetDrawData();
    for (int i = 0; i < data->CmdListsCount; ++i) {
        const auto& list = *data->CmdLists[i];
        for (const auto& command : list.CmdBuffer) {
            used_offset = used_offset || command.VtxOffset > 0;
            for (unsigned index = 0; index < command.ElemCount; ++index)
                check(command.VtxOffset + list.IdxBuffer[command.IdxOffset + index] <
                          static_cast<unsigned>(list.VtxBuffer.Size),
                      "crowd glow references vertices outside its draw list");
        }
    }
    check(used_offset, "crowd glow did not split vertex offsets for the renderer");
}

void test_hat() {
    ImGuiScope imgui;
    ModuleManager modules;
    auto& hat = modules.add<ChinaHatModule>();
    hat.set_enabled(true);
    auto frame = sample_frame();
    check(render(modules, frame).vertices.empty(), "hat is hidden in first person");
    frame.camera = {0, 2.6F, 4.0F};
    check(!render(modules, frame).vertices.empty(), "hat projects above the local player in third person");
    hat.rainbow.get<bool>() = true;
    const auto first = render(modules, frame, 0.0F);
    const auto next = render(modules, frame, 2.0F);
    check(first.vertices.front().col != next.vertices.front().col, "rainbow changes hat color over time");
    hat.radius.get<float>() = std::numeric_limits<float>::quiet_NaN();
    check(!render(modules, frame).vertices.empty(), "invalid radius falls back to a finite value");
    frame.local.alive = false;
    check(render(modules, frame).vertices.empty(), "hat is hidden when the local player is dead");
}

void test_hat_camera_and_interpolation() {
    ImGuiScope imgui;
    ModuleManager modules;
    modules.add<ChinaHatModule>().set_enabled(true);
    auto frame = sample_frame();
    // Third person with the render origin left at the eye: the view matrix
    // carries the offset to a camera at (0, 2.6, 4).
    frame.view_matrix[7] = -1.0F;
    frame.view_matrix[11] = -4.0F;
    const auto camera = sdk::camera_position(frame);
    check(std::abs(camera.y - 2.6F) < 0.001F && std::abs(camera.z - 4.0F) < 0.001F,
          "the camera position includes the view translation");
    const auto current = render(modules, frame);
    check(!current.vertices.empty(), "hat shows in third person when the offset is in the view matrix");

    const auto same_place = [](const Geometry& a, const Geometry& b) {
        return !a.vertices.empty() && a.vertices.size() == b.vertices.size() &&
               std::abs(a.vertices.front().pos.x - b.vertices.front().pos.x) < 0.001F &&
               std::abs(a.vertices.front().pos.y - b.vertices.front().pos.y) < 0.001F;
    };

    // Without a render position, drawn between the last two ticks.
    frame.local.previous_position = {0.0F, 0.0F, 1.0F};
    frame.local.has_previous = true;
    const auto start = render(modules, frame, 0.0F, 0.0F);
    const auto end = render(modules, frame, 0.0F, 1.0F);
    check(!start.vertices.empty() && start.vertices.size() == end.vertices.size() && same_place(end, current) &&
          (std::abs(start.vertices.front().pos.x - end.vertices.front().pos.x) > 0.5F ||
           std::abs(start.vertices.front().pos.y - end.vertices.front().pos.y) > 0.5F),
          "hat follows the position between the last two ticks");

    // The game draws the model a little past the tick snapshot: the hat sits
    // on that render position whatever the tick timing.
    auto drawn = frame;
    drawn.local.has_previous = false;
    drawn.local.position.z += 0.5F;
    drawn.local.bounds.min.z += 0.5F;
    drawn.local.bounds.max.z += 0.5F;
    const auto on_model = render(modules, drawn);
    frame.local.render_position = drawn.local.position;
    frame.local.has_render_position = true;
    check(same_place(render(modules, frame, 0.0F, 0.0F), on_model) &&
          same_place(render(modules, frame, 0.0F, 1.0F), on_model) && !same_place(on_model, current),
          "hat sits on the render position instead of the trailing tick position");
}
} // namespace

int main() {
    try {
        test_esp_glow();
        test_filters_and_projection();
        test_close_esp_glow_spread();
        test_many_glowing_entities();
        test_hat();
        test_hat_camera_and_interpolation();
        std::cout << checks << " gameplay visual checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
