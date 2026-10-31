#include <medicon/gui/TopNav.hpp>

#include <medicon/gui/Draw.hpp>
#include <medicon/gui/Glass.hpp>
#include <medicon/gui/Theme.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace medicon::gui {

namespace style = theme::top_nav;

namespace {

void stroke_arc(ImDrawList* list, ImVec2 center, float radius, float from, float to, ImU32 color, float stroke) {
    list->PathArcTo(center, radius, from, to, 16);
    list->PathStroke(color, ImDrawFlags_None, stroke);
}

// Two figures side by side: a whole one in front, and the one behind cut off
// where they meet, drawn as open strokes so they stay light at 18 px.
void draw_social_icon(ImDrawList* list, ImVec2 center, float size, ImU32 color) {
    constexpr float pi = std::numbers::pi_v<float>;
    const auto p = [&](float x, float y) { return ImVec2(center.x + x * size, center.y + y * size); };
    const float stroke = std::max(1.0F, size * 0.083F);
    list->AddCircle(p(-0.13F, -0.20F), size * 0.185F, color, 20, stroke);
    stroke_arc(list, p(-0.13F, 0.44F), size * 0.34F, -pi * 0.98F, -pi * 0.02F, color, stroke);
    stroke_arc(list, p(0.16F, -0.20F), size * 0.185F, -pi * 0.46F, pi * 0.46F, color, stroke);
    stroke_arc(list, p(0.14F, 0.44F), size * 0.34F, -pi * 0.30F, -pi * 0.02F, color, stroke);
}

} // namespace

TopNavRect top_nav_rect(int slot, int count, float client_w, float scale) {
    const float width = px(style::width, scale);
    const float height = px(style::height, scale);
    const float gap = px(style::gap, scale);
    const float total = width * static_cast<float>(count) + gap * static_cast<float>(count - 1);
    const float x = std::round((client_w - total) * 0.5F) + (width + gap) * static_cast<float>(slot);
    const float top = px(style::top, scale);
    return {{x, top}, {x + width, top + height}};
}

void draw_top_nav_icon(ImDrawList* list, TopNavIcon icon, ImVec2 center, float size, ImU32 color) {
    switch (icon) {
    case TopNavIcon::Social: draw_social_icon(list, center, size, color); break;
    }
}

void TopNavItem::animate(float dt) {
    hover = tween(hover, hovered || held, dt, style::hover_seconds);
    press = tween(press, held, dt, style::press_seconds);
    select.step(selected, dt, style::select_seconds, style::select_seconds);
}

void TopNavItem::draw(ImDrawList* list, FontManager& fonts, IShadowRenderer& shadows, float scale, float alpha) const {
    if (alpha <= 0.0F) return;
    const float hovering = ease_out_cubic(hover);
    const float selection = select.value();
    const float width = rect.max.x - rect.min.x;
    const float center_x = (rect.min.x + rect.max.x) * 0.5F;
    const float lift = style::hover_lift * scale * hovering;
    const ImVec2 min{rect.min.x, rect.min.y - lift};
    const ImVec2 max{rect.max.x, rect.max.y - lift};
    const ScaleAround pressed(list, {center_x, (min.y + max.y) * 0.5F}, 1.0F - style::press_shrink * ease_out_cubic(press));

    const float plate = std::max(style::plate_hover * hovering, style::plate_selected * selection);
    if (plate > 0.0F) {
        const float radius = style::radius * scale;
        const ShadowParams soft{style::shadow_opacity * std::max(hovering, selection), style::shadow_blur * 0.5F * scale, 0.0F,
                                style::shadow_offset_y * scale, to_u32(style::shadow_tint)};
        shadows.draw(list, min, max, CornerRadii::all(radius), soft, alpha);
        list->AddRectFilled(min, max, to_u32(style::plate, plate * alpha), radius);
    }

    const float size = std::round(style::icon_size * scale);
    const ImVec2 center{center_x, min.y + std::round(style::icon_y * scale)};
    Color color = lerp(style::icon, style::icon_hover, hovering);
    color = lerp(color, style::icon_selected, selection);
    // A thin matte edge keeps the pale icon readable over a bright world. The
    // selected colour is dark enough to stand on the pastel screen by itself.
    const float edge = style::icon_edge_opacity * (1.0F - selection);
    if (edge > 0.0F) {
        const float offset = std::max(1.0F, scale);
        draw_top_nav_icon(list, icon, {center.x, center.y + offset}, size, to_u32(style::icon_edge, edge * alpha));
    }
    draw_top_nav_icon(list, icon, center, size, to_u32(color, alpha));

    if (selection > 0.0F) {
        const float bar_w = px(style::indicator_start_width + (style::indicator_width - style::indicator_start_width) * selection, scale);
        const float bar_h = line_px(style::indicator_height, scale);
        const float bar_x = std::round(center_x - bar_w * 0.5F);
        const float bar_y = min.y + px(style::indicator_y + style::indicator_rise * (1.0F - selection), scale);
        list->AddRectFilled({bar_x, bar_y}, {bar_x + bar_w, bar_y + bar_h}, to_u32(style::icon_selected, selection * alpha), bar_h * 0.5F);
    }
    if (badge) {
        const ImVec2 dot{center.x + size * 0.52F, center.y - size * 0.40F};
        list->AddCircleFilled(dot, std::max(2.0F, style::badge_radius * scale), to_u32(theme::social::alert, alpha), 16);
    }
    if (hovering > 0.0F && !label.empty()) {
        const float x = std::round(rect.min.x + (width - fonts.measure(FontRole::Label, label)) * 0.5F);
        const float top = max.y + px(style::label_gap, scale);
        const float baseline = fonts.centered_baseline(FontRole::Label, top, px(theme::metrics::font_label, scale));
        const float offset = std::max(1.0F, scale);
        if (edge > 0.0F) fonts.draw(list, FontRole::Label, x, baseline + offset, to_u32(style::icon_edge, edge * hovering * alpha), label);
        fonts.draw(list, FontRole::Label, x, baseline, to_u32(color, hovering * alpha), label);
    }
}

} // namespace medicon::gui
