#pragma once

#include "medicon/gui/Fonts.hpp"
#include "medicon/gui/Shadow.hpp"

#include <imgui.h>

#include <array>

namespace medicon {
class ModuleManager;
}

namespace medicon::gui {

struct MouseButton {
    bool down = false;
    bool pressed = false;  // DOWN this frame (double-clicks count as presses)
    bool released = false; // UP this frame
};

// Pointer state for one frame in client-area physical pixels — the same space
// ImGui uses for drawing, so hit tests and drawing share coordinates.
struct PointerInput {
    ImVec2 position{-1.0F, -1.0F};
    bool valid = false;
    std::array<MouseButton, 5> buttons{}; // left, right, middle, x1, x2
    float wheel = 0.0F;                   // notches (delta / 120), fractional for precise wheels
    bool owned_by_imgui = false;          // another ImGui window has the mouse
};

PointerInput read_pointer_input();

struct FrameInfo {
    float client_w = 1920.0F;
    float client_h = 1080.0F;
    float dpi = 96.0F;
    float auto_scale = 1.0F; // before GUI Scale and clamping
    float hud_scale = 1.0F;  // clamp(auto_scale)
    float ui_scale = 1.0F;   // clamp(auto_scale * GUI Scale)
    float dt = 0.0F;
};

struct GuiContext {
    ModuleManager& modules;
    FontManager& fonts;
    IShadowRenderer& shadows;
    FrameInfo frame{};
    PointerInput pointer{};
    ImDrawList* draw_list = nullptr;
};

} // namespace medicon::gui
