#pragma once

#include <medicon/gui/Draw.hpp>
#include <medicon/gui/Shadow.hpp>
#include <medicon/gui/SocialFonts.hpp>
#include <medicon/gui/SocialWidgets.hpp>
#include <medicon/gui/Theme.hpp>

#include <algorithm>
#include <cmath>
#include <string_view>

// The panel the Social screen and the AltManager share: its size, shadows and
// opening animation. Sizes scale with the window like the rest of the client:
// height / 1080, or less in a window narrower than 16:9.
namespace medicon::gui::social_panel {

inline float layout_scale(ImVec2 display) {
    return std::min(display.y / theme::social::metrics::reference_height, display.x / 1920.0F);
}

inline ImVec2 panel_size(ImVec2 display, bool login) {
    namespace metrics = theme::social::metrics;
    const float s = layout_scale(display);
    return {std::round(std::min((login ? metrics::login_panel_width : metrics::panel_width) * s,
                                display.x * metrics::panel_max_ratio)),
            std::round(std::min((login ? metrics::login_panel_height : metrics::panel_height) * s,
                                display.y * metrics::panel_max_ratio))};
}

// A wide, soft blue shadow (never black) and a faint pale-blue glow around the panel.
inline ShadowParams panel_shadow(float s) {
    namespace style = theme::social;
    return {style::panel_shadow.opacity, style::panel_shadow.blur * 0.5F * s, 0.0F, style::panel_shadow.offset_y * s,
            to_u32(style::shadow_tint)};
}

inline ShadowParams panel_glow(float s) {
    namespace style = theme::social;
    return {style::panel_glow_opacity, style::panel_glow_blur * 0.5F * s, 0.0F, 0.0F, to_u32(style::panel_glow)};
}

inline float sidebar_width(float panel_width, float s) {
    return std::round(std::min(panel_width * 0.4F, theme::social::metrics::sidebar_width * s));
}

// The open/close animation scales finished geometry around the panel centre
// (spec 4.6: 0.96 -> 1) and fades it, instead of laying the panel out again at
// another size, which would also need fonts rasterised at that size.
inline void scale_from(ImDrawList* list, int first_vertex, int first_command, ImVec2 center, float scale, float alpha) {
    for (int i = first_vertex; i < list->VtxBuffer.Size; ++i) {
        auto& vertex = list->VtxBuffer[i];
        vertex.pos.x = center.x + (vertex.pos.x - center.x) * scale;
        vertex.pos.y = center.y + (vertex.pos.y - center.y) * scale;
        const auto a = static_cast<float>((vertex.col >> IM_COL32_A_SHIFT) & 0xFFu);
        vertex.col = (vertex.col & ~IM_COL32_A_MASK) |
                     (static_cast<ImU32>(std::lround(a * alpha)) << IM_COL32_A_SHIFT);
    }
    for (int i = first_command; i < list->CmdBuffer.Size; ++i) {
        auto& clip = list->CmdBuffer[i].ClipRect;
        clip.x = center.x + (clip.x - center.x) * scale;
        clip.y = center.y + (clip.y - center.y) * scale;
        clip.z = center.x + (clip.z - center.x) * scale;
        clip.w = center.y + (clip.w - center.y) * scale;
    }
}

inline void draw_centered(ImDrawList* list, const SocialFonts& fonts, SocialText role, const SocialRect& rect, ImU32 color,
                          std::string_view text) {
    fonts.draw(list, role, rect.min.x + (rect.width() - fonts.measure(role, text)) * 0.5F,
               rect.min.y + (rect.height() - fonts.line_height(role)) * 0.5F, color, text);
}

} // namespace medicon::gui::social_panel
