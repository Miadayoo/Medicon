#include <medicon/gui/Draw.hpp>
#include <medicon/gui/Theme.hpp>

#include <algorithm>
#include <cmath>

namespace medicon::gui {

Color lerp(const Color& from, const Color& to, float t) {
    t = std::clamp(t, 0.0F, 1.0F);
    return Color{from.r + (to.r - from.r) * t, from.g + (to.g - from.g) * t, from.b + (to.b - from.b) * t,
                 from.a + (to.a - from.a) * t};
}

ImU32 to_u32(const Color& color, float alpha) {
    const auto channel = [](float value) {
        return static_cast<ImU32>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
    };
    return IM_COL32(channel(color.r), channel(color.g), channel(color.b), channel(color.a * alpha));
}

HsvColor rgb_to_hsv(const Color& c) {
    const float maximum = std::max({c.r, c.g, c.b});
    const float minimum = std::min({c.r, c.g, c.b});
    const float delta = maximum - minimum;
    HsvColor hsv{0.0F, maximum > 0.0F ? delta / maximum : 0.0F, maximum};
    if (delta > 0.0F) {
        float h = 0.0F;
        if (maximum == c.r) h = std::fmod((c.g - c.b) / delta, 6.0F);
        else if (maximum == c.g) h = (c.b - c.r) / delta + 2.0F;
        else h = (c.r - c.g) / delta + 4.0F;
        h /= 6.0F;
        hsv.h = h < 0.0F ? h + 1.0F : h;
    }
    return hsv;
}

Color hsv_to_rgb(const HsvColor& hsv, float alpha) {
    const float h = (hsv.h >= 1.0F ? 0.0F : std::max(0.0F, hsv.h)) * 6.0F;
    const float c = hsv.v * hsv.s;
    const float x = c * (1.0F - std::fabs(std::fmod(h, 2.0F) - 1.0F));
    const float m = hsv.v - c;
    float r = 0.0F, g = 0.0F, b = 0.0F;
    switch (static_cast<int>(h)) {
    case 0: r = c; g = x; break;
    case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;
    case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break;
    }
    return Color{std::clamp(r + m, 0.0F, 1.0F), std::clamp(g + m, 0.0F, 1.0F), std::clamp(b + m, 0.0F, 1.0F), alpha};
}

void draw_chevron(ImDrawList* list, ImVec2 center, float scale, float progress, ImU32 color) {
    using namespace theme::metrics;
    const float half_w = chevron_w * 0.5F * scale;
    const float half_h = chevron_h * 0.5F * scale;
    const float angle = std::clamp(progress, 0.0F, 1.0F) * 3.1415927F;
    const float cs = std::cos(angle);
    const float sn = std::sin(angle);
    const auto rotate = [&](float x, float y) {
        return ImVec2(center.x + x * cs - y * sn, center.y + x * sn + y * cs);
    };
    list->AddTriangleFilled(rotate(-half_w, -half_h), rotate(half_w, -half_h), rotate(0.0F, half_h), color);
}

void draw_category_icon(ImDrawList* list, Category category, ImVec2 center, float size, ImU32 color) {
    const auto p = [&](float x, float y) { return ImVec2(center.x + x * size, center.y + y * size); };
    const float stroke = std::max(1.0F, size * 0.085F);
    const auto line = [&](float x0, float y0, float x1, float y1) { list->AddLine(p(x0, y0), p(x1, y1), color, stroke); };
    switch (category) {
    case Category::Visual: {
        const ImVec2 hex[]{p(0, -0.49F), p(0.43F, -0.25F), p(0.43F, 0.25F),
                           p(0, 0.49F), p(-0.43F, 0.25F), p(-0.43F, -0.25F)};
        list->AddPolyline(hex, 6, color, ImDrawFlags_Closed, stroke);
        line(-0.43F, -0.25F, 0, 0); line(0.43F, -0.25F, 0, 0); line(0, 0, 0, 0.49F);
        line(0, -0.49F, 0, 0); line(-0.43F, 0.25F, 0, 0); line(0.43F, 0.25F, 0, 0);
        break;
    }
    case Category::Movement:
        list->AddTriangleFilled(p(-0.06F, -0.36F), p(0.46F, 0), p(-0.06F, 0.36F), color);
        line(-0.45F, -0.22F, -0.18F, -0.22F); line(-0.53F, 0, -0.18F, 0); line(-0.45F, 0.22F, -0.18F, 0.22F);
        break;
    case Category::Player:
        list->AddCircleFilled(p(0, -0.35F), size * 0.13F, color, 12);
        list->AddRectFilled(p(-0.14F, -0.15F), p(0.14F, 0.18F), color);
        line(-0.32F, -0.11F, -0.32F, 0.22F); line(0.32F, -0.11F, 0.32F, 0.22F);
        line(-0.09F, 0.16F, -0.09F, 0.48F); line(0.09F, 0.16F, 0.09F, 0.48F);
        break;
    case Category::Misc:
        line(0, -0.48F, 0.14F, -0.14F); line(0.14F, -0.14F, 0.48F, 0);
        line(0.48F, 0, 0.14F, 0.14F); line(0.14F, 0.14F, 0, 0.48F);
        line(0, 0.48F, -0.14F, 0.14F); line(-0.14F, 0.14F, -0.48F, 0);
        line(-0.48F, 0, -0.14F, -0.14F); line(-0.14F, -0.14F, 0, -0.48F);
        break;
    case Category::Client:
        line(-0.42F, -0.30F, -0.10F, 0); line(-0.10F, 0, -0.42F, 0.30F);
        line(0.05F, 0.30F, 0.46F, 0.30F);
        break;
    case Category::Scripts:
        line(-0.20F, -0.30F, -0.48F, 0); line(-0.48F, 0, -0.20F, 0.30F);
        line(0.20F, -0.30F, 0.48F, 0); line(0.48F, 0, 0.20F, 0.30F);
        line(0.10F, -0.45F, -0.10F, 0.45F);
        break;
    }
}

void draw_checker(ImDrawList* list, ImVec2 min, ImVec2 max, float cell, ImU32 light, ImU32 dark) {
    list->AddRectFilled(min, max, light);
    if (cell < 1.0F) return;
    int row = 0;
    for (float y = min.y; y < max.y; y += cell, ++row) {
        int column = 0;
        for (float x = min.x; x < max.x; x += cell, ++column) {
            if (((row + column) & 1) == 0) continue;
            list->AddRectFilled(ImVec2(x, y), ImVec2(std::min(x + cell, max.x), std::min(y + cell, max.y)), dark);
        }
    }
}

} // namespace medicon::gui
