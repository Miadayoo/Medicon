#include <medicon/gui/Watermark.hpp>

#include <medicon/core/ModuleManager.hpp>
#include <medicon/gui/Draw.hpp>
#include <medicon/gui/Fonts.hpp>
#include <medicon/gui/Glass.hpp>
#include <medicon/gui/Shadow.hpp>
#include <medicon/gui/Theme.hpp>
#include <medicon/modules/ClientModules.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace medicon::gui {
namespace {

namespace style = theme::watermark;

constexpr char title[] = "Medicon";

} // namespace

const char* Watermark::version() noexcept { return "v" MEDICON_VERSION; }

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
    if (module->show_version.get<bool>()) details[detail_count++] = version();
    if (module->show_fps.get<bool>() && fps_ > 0) {
        fps_text_ = std::to_string(fps_) + " FPS";
        details[detail_count++] = fps_text_.c_str();
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
