#pragma once

#include "medicon/core/Setting.hpp"

#include <imgui.h>

namespace medicon::gui {

class IShadowRenderer;

// The Main Menu's glass plate, shared with the Social screen so both read as one
// app: a soft blue drop under a translucent white fill, a white rim and a thin
// accent line on the left (blue, pink while hovered).
struct GlassPlate {
    float hover = 0.0F;   // 0..1, may be animated
    float press = 0.0F;   // 0..1, may be animated
    bool accent = true;   // the line on the left
    // Gap above and below the accent line; 0 = the Main Menu's (a third of the
    // height). Shorter plates use less so the line keeps its proportion.
    float accent_inset = 0.0F;
    bool drop = true;     // the soft blue drop under the plate
    Color fill{1.0F, 1.0F, 1.0F, 1.0F}; // white on the Main Menu; Social tints it toward blue
    float alpha = 1.0F;   // fades the whole plate
    // Optional pale-blue glow around the plate, stronger while hovered.
    IShadowRenderer* glow = nullptr;
    float glow_sigma = 0.0F;
};

// Draws the plate and returns the accent inset (where the Main Menu starts its label).
float draw_glass_plate(ImDrawList* list, ImVec2 min, ImVec2 max, float radius, const GlassPlate& plate);

// Scales everything drawn between construction and destruction around `center`
// (a 0.98 press, a 0.96 opening), clip rectangles included.
class ScaleAround {
public:
    ScaleAround(ImDrawList* list, ImVec2 center, float scale, float alpha = 1.0F);
    ~ScaleAround();
    ScaleAround(const ScaleAround&) = delete;
    ScaleAround& operator=(const ScaleAround&) = delete;

private:
    ImDrawList* list_;
    ImVec2 center_;
    float scale_, alpha_;
    int first_vertex_, first_command_;
};

} // namespace medicon::gui
