#include <medicon/gui/Watermark.hpp>

#include <medicon/core/ModuleManager.hpp>
#include <medicon/gui/Draw.hpp>
#include <medicon/gui/Fonts.hpp>
#include <medicon/gui/Glass.hpp>
#include <medicon/gui/Shadow.hpp>
#include <medicon/gui/Theme.hpp>
#include <medicon/modules/ClientModules.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>

namespace medicon::gui {
namespace {

namespace style = theme::watermark;

constexpr char title[] = "Medicon";
constexpr char japanese_title[] = "\xE3\x82\x81\xE3\x81\x98\xE3\x81\x93\xE3\x82\x93"; // めじこん

constexpr float pi = 3.14159265358979323846F;

// Offset copies of the letters, outermost and faintest first: they add up to a
// soft falloff. Radii are reference px, multiplied like the rest of the layout.
struct GlowRing {
    float radius;
    int copies;
    float opacity;
};
constexpr GlowRing glow_rings[]{
    {11.0F, 22, 0.012F}, {9.0F, 20, 0.020F}, {7.0F, 16, 0.034F}, {5.0F, 14, 0.050F}, {3.0F, 12, 0.070F},
};

struct JapaneseLayout {
    float scale;
    float enter;       // 0 to 1 with the entrance
    bool settled;
    bool glow;
    float opacity;
    const char* const* details;
    int detail_count;
};

void draw_japanese(GuiContext& context, const JapaneseLayout& layout) {
    auto& fonts = context.fonts;
    auto* list = context.draw_list;
    const FontFace face = fonts.face(FontRole::WatermarkJapanese);
    if (!face.font) return;
    // Ink bounds rather than the line box: the face's metrics leave the kana
    // off centre. No glyphs means the atlas has not caught up with the mode yet.
    ImVec2 ink_min{FLT_MAX, FLT_MAX};
    ImVec2 ink_max{-FLT_MAX, -FLT_MAX};
    float pen = 0.0F;
    for (const ImWchar code : {0x3081, 0x3058, 0x3053, 0x3093}) {
        const auto* glyph = face.font->FindGlyphNoFallback(code);
        if (!glyph) return;
        ink_min = {std::min(ink_min.x, pen + glyph->X0), std::min(ink_min.y, glyph->Y0)};
        ink_max = {std::max(ink_max.x, pen + glyph->X1), std::max(ink_max.y, glyph->Y1)};
        pen += glyph->AdvanceX;
    }

    const float s = layout.scale;
    const float margin = std::round(style::margin * s);
    const float rise = layout.settled ? 0.0F : -style::rise * s * (1.0F - layout.enter);
    const ImVec2 origin{margin - ink_min.x, margin - ink_min.y + rise};
    const float ink_right = origin.x + ink_max.x;
    const float ink_middle = origin.y + (ink_min.y + ink_max.y) * 0.5F;

    const ScaleAround scope(list, {(margin + ink_right) * 0.5F, ink_middle},
                            layout.settled ? 1.0F : style::start_scale + (1.0F - style::start_scale) * layout.enter,
                            layout.enter * layout.opacity);

    // Built at the origin and moved afterwards, as in the inject title:
    // ImFont::RenderText truncates positions, which would flatten the rings.
    const ImVec4 clip(-100000.0F, -100000.0F, 100000.0F, 100000.0F);
    const auto stamp = [&](ImVec2 offset, ImU32 color) {
        const int first = list->VtxBuffer.Size;
        face.font->RenderText(list, face.size, {0.0F, 0.0F}, color, clip, japanese_title, nullptr, 0.0F, false);
        for (int i = first; i < list->VtxBuffer.Size; ++i) {
            list->VtxBuffer[i].pos.x += origin.x + offset.x;
            list->VtxBuffer[i].pos.y += origin.y + offset.y;
        }
    };
    const auto ring = [&](float radius, int copies, ImU32 color) {
        for (int i = 0; i < copies; ++i) {
            const float angle = 2.0F * pi * static_cast<float>(i) / static_cast<float>(copies);
            stamp({std::cos(angle) * radius, std::sin(angle) * radius}, color);
        }
    };
    if (layout.glow) {
        for (const auto& glow : glow_rings) ring(glow.radius * s, glow.copies, to_u32(style::title_glow, glow.opacity));
    }
    const float outline = std::max(1.0F, face.size * style::title_outline_em);
    ring(outline, 8, to_u32(style::title_outline));
    stamp({}, to_u32(style::title_fill));

    // Details beside the letters, centred on them, outlined to read over any scene.
    const float small_cap = fonts.face(FontRole::WatermarkSmall).cap_height;
    const float baseline = layout.settled ? std::round(ink_middle + small_cap * 0.5F) : ink_middle + small_cap * 0.5F;
    const float gap = style::gap * s;
    const ImU32 edge = to_u32(style::title_outline, 0.6F);
    float x = ink_right + style::title_gap * s;
    for (int i = 0; i < layout.detail_count; ++i) {
        if (i > 0) {
            x += gap * 0.5F;
            const ImVec2 center{x + style::dot * s * 0.5F, baseline - small_cap * 0.5F};
            list->AddCircleFilled(center, style::dot * s * 0.5F + 1.0F, edge, 12);
            list->AddCircleFilled(center, style::dot * s * 0.5F, to_u32(style::title_fill), 12);
            x += style::dot * s + gap * 0.5F;
        }
        for (int k = 0; k < 8; ++k) {
            const float angle = 2.0F * pi * static_cast<float>(k) / 8.0F;
            // Fractional even when settled: a whole pixel closes up letters this small.
            fonts.draw(list, FontRole::WatermarkSmall, x + std::cos(angle) * 0.7F, baseline + std::sin(angle) * 0.7F, edge,
                       layout.details[i], false);
        }
        fonts.draw(list, FontRole::WatermarkSmall, x, baseline, to_u32(style::title_fill), layout.details[i], layout.settled);
        x += fonts.measure(FontRole::WatermarkSmall, layout.details[i]);
    }
}

} // namespace

Watermark::Watermark() : version_("v" MEDICON_VERSION) {}

void Watermark::set_version(std::string_view text) {
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) text.remove_prefix(1);
    if (!text.empty()) version_ = "v" + std::string(text);
}

void Watermark::hide() noexcept {
    fade_.snap(false);
    width_ = 0.0F;
}

void Watermark::render(GuiContext& context, float wall_dt) {
    auto* module = dynamic_cast<WatermarkModule*>(context.modules.find(WatermarkModule::module_id));
    const bool on = module && module->enabled();
    fade_.step(on, context.frame.dt, style::fade, style::fade);

    // Frames counted over half a second, so the digits change calmly.
    if (on && std::isfinite(wall_dt) && wall_dt > 0.0F) {
        fps_time_ += wall_dt;
        ++fps_frames_;
        if (fps_time_ >= style::fps_window) {
            fps_ = static_cast<int>(std::lround(static_cast<float>(fps_frames_) / fps_time_));
            fps_time_ = 0.0F;
            fps_frames_ = 0;
        }
    }
    if (!module || !visible() || !context.draw_list) return;

    auto& fonts = context.fonts;
    auto* list = context.draw_list;
    const float s = context.frame.hud_scale * std::clamp(module->scale.get<float>(), 0.75F, 1.5F);
    const float t = fade_.value();
    const bool settled = fade_.settled(true);

    // Details after the name, separated by small dots; the plate fits them.
    // TODO(watermark): Show Ping draws nothing until the client can read the
    // server latency (no ping source exists in the SDK yet).
    const char* details[2]{};
    int detail_count = 0;
    if (module->show_version.get<bool>()) details[detail_count++] = version_.c_str();
    if (module->show_fps.get<bool>() && fps_ > 0) {
        fps_text_ = std::to_string(fps_) + " FPS";
        details[detail_count++] = fps_text_.c_str();
    }
    if (module->japanese()) {
        draw_japanese(context, {s, t, settled, module->glow.get<bool>(),
                                std::clamp(module->opacity.get<float>(), 0.5F, 1.0F), details, detail_count});
        return;
    }
    const float gap = style::gap * s;
    const float dot_room = style::dot * s + gap;
    float content = fonts.measure(FontRole::Watermark, title);
    for (int i = 0; i < detail_count; ++i)
        content += (i == 0 ? gap : dot_room) + fonts.measure(FontRole::WatermarkSmall, details[i]);
    const float target = std::round((style::padding_left + style::padding_right) * s + content);
    // A new FPS digit eases the width; a new scale or a fresh entrance snaps it.
    if (width_ <= 0.0F || scale_ != s) width_ = target;
    else width_ = follow(width_, target, context.frame.dt, style::width_tau);
    scale_ = s;

    const float height = std::round(style::height * s);
    const float radius = style::radius * s;
    const float margin = std::round(style::margin * s);
    const float rise = settled ? 0.0F : -style::rise * s * (1.0F - t);
    const ImVec2 min{margin, margin + rise};
    const ImVec2 max{min.x + std::round(width_), min.y + height};
    const float opacity = std::clamp(module->opacity.get<float>(), 0.5F, 1.0F);
    const bool shadow = module->shadow.get<bool>();
    const bool glow = module->glow.get<bool>();

    // Shadow and glow meshes are built off the render thread once per size and
    // reused: the entrance scales the drawn vertices, never the shapes.
    const ShadowParams drop{style::shadow_opacity, style::shadow_blur * 0.5F * s, 0.0F, style::shadow_offset_y * s,
                            to_u32(style::shadow)};
    const ShadowParams light{style::glow_opacity, style::glow_blur * 0.5F * s, 0.0F, 0.0F,
                             to_u32(theme::glass::glow)};
    const ImVec2 size{max.x - min.x, max.y - min.y};
    const CornerRadii corners = CornerRadii::all(radius);
    if (shadow) context.shadows.prepare(size, corners, drop);
    if (glow) context.shadows.prepare(size, corners, light);

    const ScaleAround scope(list, {(min.x + max.x) * 0.5F, (min.y + max.y) * 0.5F},
                            settled ? 1.0F : style::start_scale + (1.0F - style::start_scale) * t, t * opacity);
    // The glow lies all around and the shadow below: separate jobs, both outside the plate.
    if (glow) context.shadows.draw(list, min, max, corners, light, 1.0F);
    if (shadow) context.shadows.draw(list, min, max, corners, drop, 1.0F);
    GlassPlate plate;
    plate.accent_inset = style::accent_inset * s;
    plate.drop = shadow;
    plate.fill = style::fill;
    plate.fill_opacity = style::fill_opacity;
    const float inset = draw_glass_plate(list, min, max, radius, plate);
    if (glow) {
        // The same rectangle draw_glass_plate gives its accent line.
        const ImVec2 line_min{min.x + inset * 0.55F, min.y + inset};
        const ImVec2 line_max{line_min.x + std::max(2.0F, radius * 0.22F), max.y - inset};
        context.shadows.draw(list, line_min, line_max, CornerRadii::all(2.0F),
                             ShadowParams{style::accent_glow_opacity, style::accent_glow_blur * 0.5F * s, 0.0F, 0.0F,
                                          to_u32(theme::glass::accent)},
                             1.0F);
    }

    // One baseline for the name and the details; fractional while it moves.
    const float baseline = fonts.centered_baseline(FontRole::Watermark, min.y, height, settled);
    float x = min.x + style::padding_left * s;
    fonts.draw(list, FontRole::Watermark, x, baseline, to_u32(style::text), title, settled);
    x += fonts.measure(FontRole::Watermark, title);
    const float small_cap = fonts.face(FontRole::WatermarkSmall).cap_height;
    for (int i = 0; i < detail_count; ++i) {
        if (i > 0) {
            x += gap * 0.5F;
            list->AddCircleFilled({x + style::dot * s * 0.5F, baseline - small_cap * 0.5F}, style::dot * s * 0.5F,
                                  to_u32(style::detail, 0.7F), 12);
            x += style::dot * s + gap * 0.5F;
        } else {
            x += gap;
        }
        fonts.draw(list, FontRole::WatermarkSmall, x, baseline, to_u32(style::detail), details[i], settled);
        x += fonts.measure(FontRole::WatermarkSmall, details[i]);
    }
}

} // namespace medicon::gui
