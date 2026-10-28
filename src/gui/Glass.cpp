#include <medicon/gui/Glass.hpp>

#include <medicon/gui/Draw.hpp>
#include <medicon/gui/Shadow.hpp>
#include <medicon/gui/Theme.hpp>

#include <algorithm>
#include <cmath>

namespace medicon::gui {

namespace glass = theme::glass;

float draw_glass_plate(ImDrawList* list, ImVec2 min, ImVec2 max, float radius, const GlassPlate& plate) {
    const float hover = std::clamp(plate.hover, 0.0F, 1.0F);
    const float press = std::clamp(plate.press, 0.0F, 1.0F);
    const float alpha = plate.alpha;
    if (plate.glow && plate.glow_sigma > 0.0F) {
        const float strength = glass::glow_idle + (glass::glow_hover - glass::glow_idle) * hover;
        plate.glow->draw(list, min, max, CornerRadii::all(radius),
                         ShadowParams{strength, plate.glow_sigma, 0.0F, 0.0F, to_u32(glass::glow)}, alpha);
    }
    if (plate.drop) {
        list->AddRectFilled({min.x, min.y + radius * 0.35F}, {max.x, max.y + radius * 0.35F},
                            to_u32(glass::drop, alpha), radius);
    }
    float fill = glass::fill_idle + (glass::fill_hover - glass::fill_idle) * hover;
    fill += (glass::fill_pressed - fill) * press;
    if (plate.fill_opacity > 0.0F) fill = plate.fill_opacity;
    list->AddRectFilled(min, max, to_u32(plate.fill, fill * alpha), radius);
    const float rim = glass::rim_idle + (glass::rim_hover - glass::rim_idle) * hover;
    list->AddRect(min, max, to_u32(theme::white, rim * alpha), radius, 0, 1.0F);
    const float inset = plate.accent_inset > 0.0F ? plate.accent_inset : std::max(8.0F, (max.y - min.y) * 0.34F);
    if (plate.accent) {
        const Color line = hover <= 0.0F ? glass::accent : hover >= 1.0F ? glass::accent_hover
                                                                          : lerp(glass::accent, glass::accent_hover, hover);
        list->AddRectFilled({min.x + inset * 0.55F, min.y + inset},
                            {min.x + inset * 0.55F + std::max(2.0F, radius * 0.22F), max.y - inset}, to_u32(line, alpha),
                            2.0F);
    }
    return inset;
}

ScaleAround::ScaleAround(ImDrawList* list, ImVec2 center, float scale, float alpha)
    : list_(list), center_(center), scale_(scale), alpha_(alpha), first_vertex_(list->VtxBuffer.Size) {
    list->AddDrawCmd();
    first_command_ = list->CmdBuffer.Size - 1;
}

ScaleAround::~ScaleAround() {
    if (scale_ != 1.0F || alpha_ != 1.0F) {
        for (int i = first_vertex_; i < list_->VtxBuffer.Size; ++i) {
            auto& vertex = list_->VtxBuffer[i];
            vertex.pos.x = center_.x + (vertex.pos.x - center_.x) * scale_;
            vertex.pos.y = center_.y + (vertex.pos.y - center_.y) * scale_;
            if (alpha_ != 1.0F) {
                const auto a = static_cast<float>((vertex.col >> IM_COL32_A_SHIFT) & 0xFFu);
                vertex.col = (vertex.col & ~IM_COL32_A_MASK) |
                             (static_cast<ImU32>(std::lround(a * alpha_)) << IM_COL32_A_SHIFT);
            }
        }
        for (int i = first_command_; i < list_->CmdBuffer.Size; ++i) {
            auto& clip = list_->CmdBuffer[i].ClipRect;
            clip.x = center_.x + (clip.x - center_.x) * scale_;
            clip.y = center_.y + (clip.y - center_.y) * scale_;
            clip.z = center_.x + (clip.z - center_.x) * scale_;
            clip.w = center_.y + (clip.w - center_.y) * scale_;
        }
    }
    // Whatever follows must not land in a command whose clip rectangle was scaled.
    list_->AddDrawCmd();
}

} // namespace medicon::gui
