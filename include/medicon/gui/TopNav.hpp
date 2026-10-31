#pragma once

#include "medicon/gui/Fonts.hpp"
#include "medicon/gui/GuiMath.hpp"
#include "medicon/gui/Shadow.hpp"

#include <imgui.h>

#include <string_view>

namespace medicon::gui {

// One quiet icon at the top centre of the window: no bar behind it, only a
// short indicator (and a very thin plate) while selected. A new button is a new
// TopNavIcon and one more slot; the look, motion and layout are shared here.
enum class TopNavIcon { Social };

struct TopNavRect {
    ImVec2 min{};
    ImVec2 max{};
};

// Physical px of one slot out of `count`; the group stays centred as a whole.
TopNavRect top_nav_rect(int slot, int count, float client_w, float scale);

void draw_top_nav_icon(ImDrawList* list, TopNavIcon icon, ImVec2 center, float size, ImU32 color);

struct TopNavItem {
    TopNavIcon icon = TopNavIcon::Social;
    std::string_view label; // shown small under the icon while hovered
    TopNavRect rect;
    bool hovered = false;
    bool held = false;
    bool selected = false;
    bool badge = false;
    float hover = 0.0F;
    float press = 0.0F;
    Reversible select;

    bool contains(ImVec2 p) const {
        return p.x >= rect.min.x && p.x < rect.max.x && p.y >= rect.min.y && p.y < rect.max.y;
    }
    void animate(float dt);
    void draw(ImDrawList* list, FontManager& fonts, IShadowRenderer& shadows, float scale, float alpha) const;
};

} // namespace medicon::gui
