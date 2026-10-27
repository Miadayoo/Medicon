#pragma once

#include "medicon/core/Setting.hpp"

#include <imgui.h>

namespace medicon::gui {

Color lerp(const Color& from, const Color& to, float t);
// Theme colour to ImGui colour, multiplying alpha (used for the global GUI fade).
ImU32 to_u32(const Color& color, float alpha = 1.0F);

struct HsvColor {
    float h = 0.0F;
    float s = 0.0F;
    float v = 0.0F;
};
HsvColor rgb_to_hsv(const Color& color);
Color hsv_to_rgb(const HsvColor& hsv, float alpha);

// Filled disclosure triangle. progress 0 = down (closed), 1 = up (open).
void draw_chevron(ImDrawList* list, ImVec2 center, float scale, float progress, ImU32 color);
// Small vector category icons, with no external icon font dependency.
void draw_category_icon(ImDrawList* list, Category category, ImVec2 center, float size, ImU32 color);
// Checkerboard used behind translucent colours.
void draw_checker(ImDrawList* list, ImVec2 min, ImVec2 max, float cell, ImU32 light, ImU32 dark);

} // namespace medicon::gui
