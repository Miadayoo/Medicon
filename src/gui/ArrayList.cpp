#include <medicon/gui/ArrayList.hpp>

#include <medicon/core/ModuleManager.hpp>
#include <medicon/gui/Draw.hpp>
#include <medicon/gui/EdgeLit.hpp>
#include <medicon/gui/GuiMath.hpp>
#include <medicon/modules/ClientModules.hpp>

#include <algorithm>
#include <cmath>

namespace medicon::gui {
namespace {

ImVec4 rgba(const Color& c, float alpha = 1.0F) { return {c.r, c.g, c.b, alpha}; }

EdgeLitText text(FontManager& fonts, FontRole role, const std::string& value,
                 float x, float row_top, float row_height, bool settled,
                 const Color& color, float opacity) {
    const auto face = fonts.face(role);
    float baseline = row_top + (row_height + face.cap_height) * 0.5F;
    // Keep animation coordinates fractional. Only a stationary baseline snaps.
    if (settled) baseline = std::round(baseline);
    return {face.font, face.size, {x, baseline - face.baseline}, value, rgba(color, opacity)};
}

} // namespace

void ArrayList::render(GuiContext& context) {
    auto* module = dynamic_cast<ArrayListModule*>(context.modules.find(ArrayListModule::module_id));
    if (!module || !module->enabled()) {
        animator_.clear();
        publish_edge_lit_frame({});
        return;
    }

    // These are physical framebuffer pixels, independent of ClickGUI's DPI scale.
    const float k = module->scale.get<float>();
    const float row_height = 24.0F * k;
    const float plate_right = context.frame.client_w - 9.0F * k;
    const Color accent_a = module->accent_a.get<Color>();
    const Color accent_b = module->accent_b.get<Color>();
    auto& fonts = context.fonts;

    // Reuse owning strings as well as vector capacity. Clearing the vector used
    // to free longer module names/suffixes and allocate them again every frame.
    std::size_t entry_count = 0;
    int order = 0;
    for (const auto& candidate : context.modules.all()) {
        const int index = order++;
        if (!candidate->show_in_arraylist()) continue;
        if (entry_count == entries_.size()) entries_.emplace_back();
        auto& entry = entries_[entry_count++];
        entry.id = candidate->id();
        entry.name = candidate->name();
        entry.order = index;
        entry.enabled = candidate->enabled();
        entry.width = 0.0F;
        entry.suffix.clear();
        if (entry.enabled) {
            entry.suffix = candidate->suffix();
            entry.width = 22.0F * k + fonts.measure(FontRole::ArrayList, entry.name);
            if (!entry.suffix.empty()) {
                entry.width += 7.0F * k + fonts.measure(FontRole::ArrayListMode, entry.suffix);
            }
        }
    }
    entries_.resize(entry_count);

    ArrayListAnimator::Params params;
    params.top = 8.0F * k;
    params.row_height = row_height;
    params.pulse = module->pulse.get<bool>();
    animator_.update(entries_, params, context.frame.dt);

    EdgeLitFrame frame;
    frame.screenWidth = context.frame.client_w;
    frame.top = params.top;
    frame.height = animator_.height();
    frame.scale = k;
    frame.accentA = rgba(accent_a);
    frame.accentB = rgba(accent_b);
    frame.glow = module->glow.get<float>();
    frame.shadow = module->shadow.get<float>();
    const auto& rows = animator_.rows();
    frame.rows.reserve(rows.size());
    float first_center = params.top;
    float last_center = params.top;
    if (!rows.empty()) {
        first_center = last_center = rows.front().y + rows.front().height * 0.5F;
        for (const auto& row : rows) {
            const float center = row.y + row.height * 0.5F;
            first_center = std::min(first_center, center);
            last_center = std::max(last_center, center);
        }
    }
    const float span = last_center - first_center;

    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        const float t = span > 0.001F ? std::clamp((row.y + row.height * 0.5F - first_center) / span, 0.0F, 1.0F) : 0.0F;
        const Color c = lerp(accent_a, accent_b, t);
        const float slide = row.slide * (row.display_width + 12.0F * k);
        EdgeLitRow plate;
        plate.right = plate_right + slide;
        plate.left = plate.right - row.display_width;
        plate.top = row.y;
        plate.bottom = row.y + row.height;
        plate.roundTopLeft = i == 0 ? 5.0F * k : 0.0F;
        plate.roundBottomLeft = i + 1 == rows.size() || rows[i + 1].display_width < row.display_width - 0.01F ? 5.0F * k : 0.0F;
        plate.opacity = row.opacity;
        plate.lighting = row.activity;
        plate.color = rgba(c);
        const Color white{1, 1, 1, 1};
        const Color name_color = lerp(c, white, 0.88F - 0.10F * row.activity);
        const Color mode_color = lerp(c, white, 0.38F + 0.30F * row.activity);
        const float text_x = plate.left + 10.0F * k;
        plate.name = text(fonts, FontRole::ArrayList, row.name, text_x, row.y, row_height,
                          row.settled, name_color, row.text_alpha);
        if (!row.suffix.empty()) {
            const float mode_x = text_x + fonts.measure(FontRole::ArrayList, row.name) + 7.0F * k;
            plate.mode = text(fonts, FontRole::ArrayListMode, row.suffix, mode_x, row.y, row_height,
                              row.settled, mode_color, row.text_alpha);
        }
        frame.rows.push_back(std::move(plate));
    }
    for (const auto& pulse : animator_.pulses()) {
        const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.id == pulse.row_id; });
        if (found == rows.end()) continue;
        const auto i = static_cast<std::size_t>(found - rows.begin());
        frame.pulses.push_back({pulse.y, pulse.strength, frame.rows[i].color});
    }
    publish_edge_lit_frame(std::move(frame));
}

} // namespace medicon::gui
