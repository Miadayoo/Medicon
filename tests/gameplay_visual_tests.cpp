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

    // Drawn where the model is between ticks, not where the last tick left it.
    frame.local.previous_position = {0.0F, 0.0F, 1.0F};
    frame.local.has_previous = true;
    const auto start = render(modules, frame, 0.0F, 0.0F);
    const auto end = render(modules, frame, 0.0F, 1.0F);
    check(!start.vertices.empty() && start.vertices.size() == end.vertices.size() &&
          std::abs(end.vertices.front().pos.x - current.vertices.front().pos.x) < 0.001F &&
          std::abs(end.vertices.front().pos.y - current.vertices.front().pos.y) < 0.001F &&
          (std::abs(start.vertices.front().pos.x - end.vertices.front().pos.x) > 0.5F ||
           std::abs(start.vertices.front().pos.y - end.vertices.front().pos.y) > 0.5F),
          "hat follows the position between the last two ticks");
}
} // namespace

int main() {
    try {
        test_esp_glow();
        test_filters_and_projection();
        test_hat();
        test_hat_camera_and_interpolation();
        std::cout << checks << " gameplay visual checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
