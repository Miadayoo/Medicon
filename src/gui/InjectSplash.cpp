#include <medicon/gui/InjectSplash.hpp>
#include <medicon/gui/Fonts.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace medicon::gui {
namespace {
constexpr float pi = 3.14159265358979323846F;
constexpr double POP_DURATION = 0.25;
constexpr double IMAGE_DURATION = 0.35;
constexpr double SHAKE_DURATION = 0.45;
constexpr char title[] = "\xE3\x83\xA1\xE3\x82\xB8\xE3\x82\xB3\xE3\x83\xB3"; // メジコン

float unit(double t) { return static_cast<float>(std::clamp(t, 0.0, 1.0)); }
float ease_out_cubic(double t) {
    const float u = 1.0F - unit(t);
    return 1.0F - u * u * u;
}
float ease_out_back(double t) {
    const float u = unit(t) - 1.0F;
    return 1.0F + 2.70158F * u * u * u + 1.70158F * u * u;
}
float ease_in_cubic(double t) {
    const float u = unit(t);
    return u * u * u;
}

struct Rgb { float r, g, b; };
constexpr Rgb pink{1.0F, 209.0F / 255.0F, 227.0F / 255.0F};
constexpr Rgb blue{127.0F / 255.0F, 203.0F / 255.0F, 1.0F};
constexpr Rgb pale_blue{217.0F / 255.0F, 242.0F / 255.0F, 1.0F};
struct Blob { ImVec2 center; float radius; Rgb color; };

std::array<Blob, 5> moving_blobs(ImVec2 size, double elapsed) {
    struct Motion { float x, y, radius, period, phase; Rgb color; };
    static constexpr std::array<Motion, 5> motions{{
        {0.16F, 0.24F, 0.67F, 8.2F, 0.3F, pink},
        {0.77F, 0.25F, 0.64F, 10.7F, 1.8F, blue},
        {0.51F, 0.72F, 0.70F, 7.1F, 3.1F, pale_blue},
        {0.81F, 0.85F, 0.57F, 11.6F, 4.4F, pink},
        {0.20F, 0.76F, 0.60F, 9.4F, 5.6F, blue},
    }};
    std::array<Blob, 5> blobs{};
    const float short_side = std::min(size.x, size.y);
    for (std::size_t i = 0; i < motions.size(); ++i) {
        const auto& m = motions[i];
        // The second component has another period in the 6–12 second range.
        // Different phases and periods avoid synchronized, repetitive motion.
        const double x_phase = 2.0 * pi * elapsed / m.period;
        const double y_phase = 2.0 * pi * elapsed / (6.3 + 1.15 * static_cast<double>(i));
        blobs[i] = {{size.x * (m.x + 0.09F * static_cast<float>(std::sin(x_phase + m.phase)) +
                                  0.018F * static_cast<float>(std::cos(y_phase + 0.8))),
                     size.y * (m.y + 0.07F * static_cast<float>(std::cos(y_phase + m.phase)) +
                                  0.018F * static_cast<float>(std::sin(x_phase + 1.3)))},
                    short_side * m.radius, m.color};
    }
    return blobs;
}

ImU32 background_color(ImVec2 point, const std::array<Blob, 5>& blobs, float opacity) {
    Rgb color = pale_blue;
    for (const auto& blob : blobs) {
        const float dx = (point.x - blob.center.x) / blob.radius;
        const float dy = (point.y - blob.center.y) / blob.radius;
        const float falloff = std::max(0.0F, 1.0F - dx * dx - dy * dy);
        const float alpha = falloff * falloff;
        // Ordinary source-over alpha compositing, never additive blending.
        color.r += (blob.color.r - color.r) * alpha;
        color.g += (blob.color.g - color.g) * alpha;
        color.b += (blob.color.b - color.b) * alpha;
    }
    return ImGui::ColorConvertFloat4ToU32({color.r, color.g, color.b, opacity});
}

void draw_background(ImDrawList* list, ImVec2 size, double elapsed, const InjectSplashFrame& frame) {
    const ImVec2 center(size.x * 0.5F, size.y * 0.5F);
    const auto blobs = moving_blobs(size, elapsed);
    const float cover_radius = std::hypot(center.x, center.y);
    // Both the entire layer and its circular aperture contract. The aperture
    // gets smaller relative to the rectangle, revealing a circle as it closes.
    const float aperture = cover_radius * frame.scale;

    // Full cover keeps the original ray count so the hold matches the previous
    // mesh. Collapse multiplies positions by scale again, so the same ray count
    // turns into tiny triangles exactly when the circle is leaving the screen.
    constexpr int full_segments = 128;
    constexpr int full_rings = 18;
    int segments = full_segments;
    int rings = full_rings;
    if (frame.scale < 0.999F) {
        const float shown = std::max(1.0F, cover_radius * frame.scale * frame.scale);
        segments = std::clamp(static_cast<int>(std::lround(6.283185307F * shown / 16.0F)), 24, full_segments);
        rings = std::clamp(static_cast<int>(std::lround(shown / 20.0F)), 2, full_rings);
    }
    std::array<float, full_segments + 4> angles{};
    for (int i = 0; i < segments; ++i) angles[i] = 2.0F * pi * static_cast<float>(i) / static_cast<float>(segments);
    // Include the exact rectangle corners so the initial mesh covers every
    // pixel, even on ultrawide or portrait windows.
    const float corner = std::atan2(center.y, center.x);
    angles[static_cast<std::size_t>(segments)] = corner;
    angles[static_cast<std::size_t>(segments + 1)] = pi - corner;
    angles[static_cast<std::size_t>(segments + 2)] = pi + corner;
    angles[static_cast<std::size_t>(segments + 3)] = 2.0F * pi - corner;
    const int angle_count = segments + 4;
    std::sort(angles.begin(), angles.begin() + angle_count);

    // A direction's aperture and rectangle intersection are shared by every
    // ring. Computing them in the vertex loop repeated trig and divisions 19
    // times per direction. Keep the same mesh with one calculation per ray and
    // no temporary heap allocation on the render thread.
    struct Ray { float cs, sn, outer, inner; bool circle_edge; };
    std::array<Ray, full_segments + 4> rays{};
    for (int i = 0; i < angle_count; ++i) {
        const float cs = std::cos(angles[i]), sn = std::sin(angles[i]);
        const float edge = std::min(center.x / std::max(std::fabs(cs), 1e-6F),
                                    center.y / std::max(std::fabs(sn), 1e-6F));
        const float outer = std::min(edge, aperture);
        rays[i] = {cs, sn, outer, std::max(0.0F, outer - 1.0F / frame.scale), aperture < edge};
    }

    const unsigned count = static_cast<unsigned>(angle_count);
    list->PrimReserve(static_cast<int>(count * 3 + count * rings * 6),
                      static_cast<int>(1 + count * (rings + 1)));
    const unsigned base = list->_VtxCurrentIdx;
    const ImVec2 uv = ImGui::GetIO().Fonts->TexUvWhitePixel;
    list->PrimWriteVtx(center, uv, background_color(center, blobs, frame.alpha));
    for (int ring = 1; ring <= rings + 1; ++ring) {
        for (int ray_index = 0; ray_index < angle_count; ++ray_index) {
            const auto& ray = rays[static_cast<std::size_t>(ray_index)];
            const float radius = ring > rings ? ray.outer : ray.inner * static_cast<float>(ring) / static_cast<float>(rings);
            const ImVec2 point(center.x + radius * ray.cs, center.y + radius * ray.sn);
            const float alpha = ring > rings && ray.circle_edge ? 0.0F : frame.alpha;
            list->PrimWriteVtx({center.x + radius * ray.cs * frame.scale, center.y + radius * ray.sn * frame.scale},
                              uv, background_color(point, blobs, alpha));
        }
    }
    const auto index = [&](unsigned i) { list->PrimWriteIdx(static_cast<ImDrawIdx>(base + i)); };
    for (unsigned i = 0; i < count; ++i) {
        index(0); index(1 + i); index(1 + (i + 1) % count);
    }
    for (unsigned ring = 0; ring < static_cast<unsigned>(rings); ++ring) {
        const unsigned a = 1 + ring * count, b = a + count;
        for (unsigned i = 0; i < count; ++i) {
            const unsigned next = (i + 1) % count;
            index(a + i); index(b + i); index(b + next);
            index(a + i); index(b + next); index(a + next);
        }
    }
}

void draw_title(ImDrawList* list, const FontManager& fonts, ImVec2 size, const InjectSplashFrame& frame) {
    const auto face = fonts.face(FontRole::Splash);
    if (!face.font || frame.text_alpha <= 0.0F) return;
    const float short_side = std::min(size.x, size.y);
    const float text_px = short_side * 0.13F;
    const float glyph_scale = text_px / face.font->FontSize;
    float pen = 0.0F;
    ImVec2 minimum(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
    ImVec2 maximum(-minimum.x, -minimum.y);
    for (const ImWchar code : {0x30E1, 0x30B8, 0x30B3, 0x30F3}) {
        if (const auto* glyph = face.font->FindGlyph(code)) {
            minimum.x = std::min(minimum.x, pen + glyph->X0);
            minimum.y = std::min(minimum.y, glyph->Y0);
            maximum.x = std::max(maximum.x, pen + glyph->X1);
            maximum.y = std::max(maximum.y, glyph->Y1);
            pen += glyph->AdvanceX;
        }
    }
    if (minimum.x > maximum.x) return;
    const ImVec2 pivot((minimum.x + maximum.x) * 0.5F * glyph_scale,
                       (minimum.y + maximum.y) * 0.5F * glyph_scale);
    const float text_height = (maximum.y - minimum.y) * glyph_scale;
    const ImVec2 target(size.x * 0.5F + frame.text_shake.x,
                        size.y * 0.5F + frame.text_shake.y +
                        (size.y * 0.5F + text_height) * (1.0F - frame.slide));
    const float alpha = frame.text_alpha * frame.alpha;
    const float outline = std::max(1.0F, short_side * 0.0027F);
    const ImU32 border = ImGui::ColorConvertFloat4ToU32({0.61F, 0.42F, 0.72F, alpha});
    const ImU32 fill = ImGui::ColorConvertFloat4ToU32({1.0F, 1.0F, 1.0F, alpha});
    const int first = list->VtxBuffer.Size;
    // Render near the origin, then transform generated vertices. This avoids
    // ImFont's integer text positioning and screen clipping during slide/pop.
    // The eight offset copies only exist while the layer is still large; the
    // collapse already fades the fill, and repeating them on a shrinking title
    // is what makes the end of the intro expensive.
    const ImVec4 clip(-100000.0F, -100000.0F, 100000.0F, 100000.0F);
    if (frame.scale >= 0.999F || frame.alpha >= 0.35F) {
        for (int i = 0; i < 8; ++i) {
            const float angle = 2.0F * pi * i / 8.0F;
            face.font->RenderText(list, text_px, {std::cos(angle) * outline, std::sin(angle) * outline}, border,
                                  clip, title, nullptr, 0.0F, false);
        }
    }
    face.font->RenderText(list, text_px, {0, 0}, fill, clip, title, nullptr, 0.0F, false);
    const ImVec2 center(size.x * 0.5F, size.y * 0.5F);
    for (int i = first; i < list->VtxBuffer.Size; ++i) {
        auto& point = list->VtxBuffer[i].pos;
        point.x = center.x + ((point.x - pivot.x) * frame.text_scale + target.x - center.x) * frame.scale;
        point.y = center.y + ((point.y - pivot.y) * frame.text_scale + target.y - center.y) * frame.scale;
    }
}
} // namespace

InjectSplashFrame evaluate_inject_splash(double elapsed) noexcept {
    if (!std::isfinite(elapsed)) elapsed = elapsed > 0.0 ? INJECT_SPLASH_DURATION : 0.0;
    elapsed = std::max(0.0, elapsed);
    InjectSplashFrame frame;
    frame.finished = elapsed >= INJECT_SPLASH_DURATION;
    frame.slide = ease_out_cubic(elapsed / SLIDE_IN_DURATION);
    frame.text_alpha = unit(elapsed / SLIDE_IN_DURATION);
    if (elapsed >= SLIDE_IN_DURATION) {
        const double local = elapsed - SLIDE_IN_DURATION;
        frame.text_scale = 1.0F + 0.6F * ease_out_back(local / POP_DURATION);
        frame.image_scale = ease_out_back(local / IMAGE_DURATION);
        frame.image_alpha = unit(local / IMAGE_DURATION);
        const float amplitude = 8.0F * (1.0F - unit(local / SHAKE_DURATION));
        frame.text_shake = {amplitude * static_cast<float>(std::sin(2.0 * pi * 39.0 * local)),
                            amplitude * static_cast<float>(std::sin(2.0 * pi * 43.0 * local))};
    }
    const double collapse = (elapsed - SLIDE_IN_DURATION - HOLD_DURATION) / COLLAPSE_DURATION;
    frame.scale = 1.0F - ease_in_cubic(collapse);
    frame.alpha = 1.0F - unit(collapse);
    if (frame.finished) frame.scale = frame.alpha = 0.0F;
    return frame;
}

void draw_inject_splash(ImDrawList* list, const FontManager& fonts, ImVec2 size, double elapsed) {
    const auto frame = evaluate_inject_splash(elapsed);
    if (!list || frame.finished || frame.scale <= 0.0F || size.x <= 0.0F || size.y <= 0.0F) return;
    if (!std::isfinite(elapsed) || elapsed < 0.0) elapsed = 0.0;
    list->PushClipRect({0, 0}, size, true);
    draw_background(list, size, elapsed, frame);
    const auto& image = fonts.splash_image();
    if (image.valid && image.size.x > 0.0F && image.size.y > 0.0F && frame.image_alpha > 0.0F) {
        const float extent = std::min(size.x, size.y) * 0.45F * frame.image_scale * frame.scale;
        const float factor = extent / std::max(image.size.x, image.size.y);
        const ImVec2 half(image.size.x * factor * 0.5F, image.size.y * factor * 0.5F);
        const ImVec2 center(size.x * 0.5F, size.y * 0.5F);
        const ImU32 tint = ImGui::ColorConvertFloat4ToU32({1, 1, 1, frame.image_alpha * frame.alpha});
        list->AddImage(ImGui::GetIO().Fonts->TexID, {center.x - half.x, center.y - half.y},
                       {center.x + half.x, center.y + half.y}, image.uv_min, image.uv_max, tint);
    }
    draw_title(list, fonts, size, frame);
    list->PopClipRect();
}

void InjectSplash::start() noexcept {
    armed_ = true;
    start_time_.reset();
}

double InjectSplash::elapsed() const noexcept {
    return start_time_ ? std::chrono::duration<double>(std::chrono::steady_clock::now() - *start_time_).count() : 0.0;
}

bool InjectSplash::active() const noexcept { return armed_ && elapsed() < INJECT_SPLASH_DURATION; }

void InjectSplash::render(ImDrawList* list, const FontManager& fonts, ImVec2 size) {
    if (!armed_) return;
    if (!start_time_) start_time_ = std::chrono::steady_clock::now();
    draw_inject_splash(list, fonts, size, elapsed());
}

} // namespace medicon::gui
