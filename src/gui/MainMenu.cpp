#include <medicon/gui/MainMenu.hpp>
#include <medicon/gui/Draw.hpp>
#include <medicon/gui/Fonts.hpp>
#include <medicon/gui/Glass.hpp>
#include <medicon/gui/GuiContext.hpp>
#include <medicon/gui/Theme.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace medicon::gui {
namespace {
constexpr double tau = 6.28318530717958647692;
constexpr ImU32 pink = IM_COL32(255, 209, 227, 255);
constexpr ImU32 blue = IM_COL32(127, 203, 255, 255);
constexpr ImU32 pale_blue = IM_COL32(217, 242, 255, 255);
constexpr ImU32 white = IM_COL32(255, 255, 255, 255);
constexpr double nominal_speed = 0.065;
constexpr double secondary_amplitude = 0.35;
constexpr double click_cooldown = 0.35;

double safe_time(double elapsed) noexcept { return std::isfinite(elapsed) ? std::max(0.0, elapsed) : 0.0; }
bool valid_size(ImVec2 size) noexcept {
    return std::isfinite(size.x) && std::isfinite(size.y) && size.x > 0.0F && size.y > 0.0F;
}
ImU32 with_alpha(ImU32 color, float alpha) {
    return (color & ~IM_COL32_A_MASK) |
           (static_cast<ImU32>(std::lround(std::clamp(alpha, 0.0F, 1.0F) * 255.0F)) << IM_COL32_A_SHIFT);
}

struct BlobStamp {
    float cx = 0, cy = 0, inv_radius_sq = 0;
    float red = 0, green = 0, blue = 0;
};

float channel(ImU32 color, ImU32 shift) noexcept {
    return static_cast<float>((color >> shift) & 0xFFu);
}

// Five screen-sized disks used to be drawn as separate alpha meshes. Each one
// covered most of the framebuffer, so Present blended the same pixels several
// times. The falloff is smooth, so one opaque grid can composite every blob
// against the pale background. Cells are coarse on purpose: the GPU interpolates
// the quadratic falloff, and a denser grid only taxes the Present thread.
int grid_cells(float extent) noexcept {
    return std::clamp(static_cast<int>(std::lround(extent / 48.0F)), 12, 56);
}

int color_byte(float value) noexcept {
    const float clamped = value < 0.0F ? 0.0F : (value > 255.0F ? 255.0F : value);
    return static_cast<int>(clamped + 0.5F);
}

ImU32 composite_background(float x, float y, const std::array<BlobStamp, 5>& stamps, int alpha) noexcept {
    float red = 217.0F, green = 242.0F, blue = 255.0F;
    for (const auto& stamp : stamps) {
        const float dx = x - stamp.cx, dy = y - stamp.cy;
        const float t2 = (dx * dx + dy * dy) * stamp.inv_radius_sq;
        if (t2 >= 1.0F) continue;
        const float falloff = 1.0F - t2;
        const float alpha = 0.8F * falloff * falloff;
        const float keep = 1.0F - alpha;
        red = stamp.red * alpha + red * keep;
        green = stamp.green * alpha + green * keep;
        blue = stamp.blue * alpha + blue * keep;
    }
    return IM_COL32(color_byte(red), color_byte(green), color_byte(blue), alpha);
}

void draw_background(ImDrawList* list, const MainMenuFrame& frame, ImVec2 size, float opacity = 1.0F) {
    const int alpha = color_byte(std::clamp(opacity, 0.0F, 1.0F) * 255.0F);
    std::array<BlobStamp, 5> stamps{};
    for (std::size_t i = 0; i < frame.blobs.size(); ++i) {
        const auto& blob = frame.blobs[i];
        const float radius = std::max(blob.radius, 1.0F);
        stamps[i] = {blob.center.x, blob.center.y, 1.0F / (radius * radius), channel(blob.color, IM_COL32_R_SHIFT),
                     channel(blob.color, IM_COL32_G_SHIFT), channel(blob.color, IM_COL32_B_SHIFT)};
    }
    const int cols = grid_cells(size.x), rows = grid_cells(size.y);
    const int vertices = (cols + 1) * (rows + 1);
    if (list->_VtxCurrentIdx > 60000u || vertices <= 0) return;
    list->PrimReserve(cols * rows * 6, vertices);
    const auto base = static_cast<unsigned>(list->_VtxCurrentIdx);
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    for (int row = 0; row <= rows; ++row) {
        const float y = row == rows ? size.y : size.y * static_cast<float>(row) / static_cast<float>(rows);
        for (int col = 0; col <= cols; ++col) {
            const float x = col == cols ? size.x : size.x * static_cast<float>(col) / static_cast<float>(cols);
            list->PrimWriteVtx({x, y}, uv, composite_background(x, y, stamps, alpha));
        }
    }
    const auto index = [&](unsigned value) { list->PrimWriteIdx(static_cast<ImDrawIdx>(base + value)); };
    const unsigned stride = static_cast<unsigned>(cols + 1);
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
            const unsigned i = static_cast<unsigned>(row) * stride + static_cast<unsigned>(col);
            index(i);
            index(i + 1);
            index(i + stride);
            index(i + 1);
            index(i + stride + 1);
            index(i + stride);
        }
    }
}

void draw_logo(ImDrawList* list, const FontManager& fonts, const MainMenuFrame& frame, double elapsed) {
    const auto face = fonts.face(FontRole::MainMenuLogo);
    if (!face.font) return;
    constexpr char text[] = "Medicon";
    const ImVec2 extent = face.font->CalcTextSizeA(frame.logo_size, std::numeric_limits<float>::max(), 0, text);
    const ImVec2 origin(frame.logo_center.x - extent.x * 0.5F, frame.logo_center.y - extent.y * 0.5F);
    const float outline = std::max(1.0F, frame.logo_size * 0.026F);
    // A pale blue outline provides definition on a bright background without
    // introducing black or dark colours. Uses the client's bundled DynaPuff.
    for (int i = 0; i < 8; ++i) {
        const double angle = tau * i / 8;
        list->AddText(face.font, frame.logo_size,
                      {origin.x + static_cast<float>(std::cos(angle)) * outline,
                       origin.y + static_cast<float>(std::sin(angle)) * outline}, with_alpha(blue, 0.65F), text);
    }
    const int first = list->VtxBuffer.Size;
    list->AddText(face.font, frame.logo_size, origin, white, text);
    const float wavelength = std::max(1.0F, extent.x * 0.78F);
    for (int i = first; i < list->VtxBuffer.Size; ++i) {
        auto& vertex = list->VtxBuffer[i];
        vertex.col = main_menu_logo_color(vertex.pos.x - origin.x, wavelength, elapsed);
    }
}

void draw_button(ImDrawList* list, ImFont* font, const MainMenuFrame& frame, const MainMenuRect& rect,
                 const char* label, bool hot, bool held, bool centered) {
    // The plate is shared with the Social screen (Glass.hpp). The small centred
    // Social button has no accent line.
    GlassPlate plate;
    plate.hover = hot ? 1.0F : 0.0F;
    plate.press = held ? 1.0F : 0.0F;
    plate.accent = !centered;
    const float inset = draw_glass_plate(list, rect.min, rect.max, frame.corner_radius, plate);
    if (!font) return;
    const ImVec2 text_size = font->CalcTextSizeA(frame.button_text_size, std::numeric_limits<float>::max(), 0, label);
    const float x = centered ? (rect.min.x + rect.max.x - text_size.x) * 0.5F : rect.min.x + inset * 1.55F;
    // Blue is the darkest colour in the requested pastel palette.
    list->AddText(font, frame.button_text_size, {x, (rect.min.y + rect.max.y - text_size.y) * 0.5F}, blue, label);
}

void draw_buttons(ImDrawList* list, const FontManager& fonts, const MainMenuFrame& frame, int hovered, int pressed) {
    const auto face = fonts.face(FontRole::MainMenuButton);
    for (std::size_t i = 0; i < frame.buttons.size(); ++i) {
        const bool hot = hovered == static_cast<int>(i), held = pressed == static_cast<int>(i) && hot;
        draw_button(list, face.font, frame, frame.buttons[i], main_menu_action_name(static_cast<MainMenuAction>(i)), hot,
                    held, false);
    }
    if (frame.show_alts) {
        const bool hot = hovered == MainMenu::alts_button, held = pressed == MainMenu::alts_button && hot;
        draw_button(list, face.font, frame, frame.alts, "AltManager", hot, held, true);
    }
    if (!frame.show_social) return;
    const bool hot = hovered == MainMenu::social_button, held = pressed == MainMenu::social_button && hot;
    draw_button(list, face.font, frame, frame.social, "Social", hot, held, true);
    if (frame.social_badge) {
        // Unread DMs or pending requests: a small dot on the top-right corner.
        const float radius = std::max(3.0F, frame.corner_radius * 0.42F);
        const ImVec2 center{frame.social.max.x - frame.corner_radius * 0.55F, frame.social.min.y + frame.corner_radius * 0.55F};
        list->AddCircleFilled(center, radius + std::max(1.0F, radius * 0.3F), white, 20);
        list->AddCircleFilled(center, radius, to_u32(theme::social::alert), 20);
    }
}
} // namespace

const char* main_menu_action_name(MainMenuAction action) noexcept {
    switch (action) {
    case MainMenuAction::Play: return "Play";
    case MainMenuAction::Settings: return "Settings";
    case MainMenuAction::Quit: return "Quit";
    }
    return "Unknown";
}

bool MainMenuRect::contains(ImVec2 point) const noexcept {
    return point.x >= min.x && point.x < max.x && point.y >= min.y && point.y < max.y;
}

const std::array<MainMenuBlobSpec, 5>& main_menu_blob_specs() noexcept {
    static const std::array<MainMenuBlobSpec, 5> specs{{
        {{0.18F, 0.22F}, 0.67F, 6.8, 8.3, 0.3, 2.7, 1.1, 4.4, pink},
        {{0.78F, 0.23F}, 0.62F, 9.1, 10.7, 1.8, 0.5, 3.8, 2.4, blue},
        {{0.50F, 0.69F}, 0.70F, 11.4, 7.3, 3.1, 4.6, 0.6, 5.7, pale_blue},
        {{0.83F, 0.79F}, 0.55F, 8.0, 11.8, 4.4, 1.3, 5.2, 0.8, pink},
        {{0.16F, 0.78F}, 0.59F, 10.3, 9.6, 5.6, 3.2, 2.3, 4.9, blue},
    }};
    return specs;
}

void layout_main_menu_text(ImVec2 size, MainMenuFrame& frame) noexcept {
    if (!valid_size(size)) return;
    const float short_side = std::min(size.x, size.y);
    const float scale = short_side / 720.0F;
    const float margin = std::min(size.x * 0.07F, 74.0F * scale);
    const float width = std::min(310.0F * scale, size.x - margin * 2.0F);
    const float height = 62.0F * scale, gap = 15.0F * scale;
    const float top = std::min(size.y * 0.45F, size.y - (height * 3 + gap * 2) - margin);
    for (std::size_t i = 0; i < frame.buttons.size(); ++i) {
        const float y = top + static_cast<float>(i) * (height + gap);
        frame.buttons[i] = {{margin, y}, {margin + width, y + height}};
    }
    // Spec 4.6: bottom centre, 24 px (at 1080 tall) above the bottom edge.
    // Smaller than the left column: 220 x 58 px at 1920 x 1080.
    const float social_scale = std::min(size.x / 1920.0F, size.y / 1080.0F);
    const float social_width = std::min(220.0F * social_scale, size.x * 0.8F);
    const float social_height = 58.0F * social_scale;
    const float social_bottom = size.y - 24.0F * size.y / 1080.0F;
    frame.social = {{(size.x - social_width) * 0.5F, social_bottom - social_height},
                    {(size.x + social_width) * 0.5F, social_bottom}};
    frame.logo_center = {size.x * 0.5F, size.y * 0.20F};
    frame.logo_size = std::min(108.0F * scale, size.x * 0.17F);
    frame.button_text_size = 24.0F * scale;
    frame.corner_radius = 13.0F * scale;
    // Social stays centred; AltManager follows it, wide enough for its longer label.
    const float alts_gap = 14.0F * social_scale;
    const float alts_left = frame.social.max.x + alts_gap;
    frame.alts = {{alts_left, frame.social.min.y},
                  {std::min(alts_left + frame.button_text_size * 6.7F, size.x - alts_gap), frame.social.max.y}};
}

MainMenuFrame evaluate_main_menu(ImVec2 size, double elapsed) noexcept {
    MainMenuFrame frame;
    if (!valid_size(size)) return frame;
    elapsed = safe_time(elapsed);
    const float short_side = std::min(size.x, size.y);
    const auto& specs = main_menu_blob_specs();
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const auto& spec = specs[i];
        const double wx = tau / spec.period_x, wy = tau / spec.period_y;
        const double wx2 = wx / 1.37, wy2 = wy / 1.61;
        // Choose the amplitude analytically for 0.065 RMS normalized screen
        // units/second. A secondary component contributes 35% of amplitude.
        const double a = nominal_speed / std::sqrt((wx * wx + wy * wy +
            secondary_amplitude * secondary_amplitude * (wx2 * wx2 + wy2 * wy2)) * 0.5);
        const auto phase = [elapsed](double frequency, double offset) {
            return std::remainder(elapsed, tau / frequency) * frequency + offset;
        };
        const double x = phase(wx, spec.phase_x), x2 = phase(wx2, spec.phase_x2);
        const double y = phase(wy, spec.phase_y), y2 = phase(wy2, spec.phase_y2);
        frame.blobs[i] = {
            {size.x * static_cast<float>(spec.anchor.x + a * std::sin(x) + a * secondary_amplitude * std::cos(x2)),
             size.y * static_cast<float>(spec.anchor.y + a * std::cos(y) + a * secondary_amplitude * std::sin(y2))},
            {size.x * static_cast<float>(a * wx * std::cos(x) - a * secondary_amplitude * wx2 * std::sin(x2)),
             size.y * static_cast<float>(-a * wy * std::sin(y) + a * secondary_amplitude * wy2 * std::cos(y2))},
            short_side * spec.radius_fraction, spec.color};
    }
    layout_main_menu_text(size, frame);
    return frame;
}

void main_menu_text_sizes(ImVec2 size, float& logo_size, float& button_text_size) noexcept {
    logo_size = 0.0F;
    button_text_size = 0.0F;
    if (!valid_size(size)) return;
    const float short_side = std::min(size.x, size.y);
    if (!std::isfinite(short_side) || short_side <= 0.0F) return;
    const float scale = short_side / 720.0F;
    logo_size = std::min(108.0F * scale, size.x * 0.17F);
    button_text_size = 24.0F * scale;
}

ImU32 main_menu_logo_color(float x, float wavelength, double elapsed) noexcept {
    if (!std::isfinite(x)) x = 0.0F;
    if (!std::isfinite(wavelength) || wavelength <= 0.0F) wavelength = 1.0F;
    const double phase = std::remainder(static_cast<double>(x) / wavelength +
                                        std::remainder(safe_time(elapsed), 3.6) / 3.6, 1.0);
    // x + time moves every colour band towards decreasing x (right to left).
    const float t = static_cast<float>(0.5 - 0.5 * std::cos(tau * phase));
    return IM_COL32(255, static_cast<int>(std::lround(209.0F + 46.0F * t)),
                    static_cast<int>(std::lround(227.0F + 28.0F * t)), 255);
}

void draw_main_menu_frame(ImDrawList* list, const FontManager& fonts, const MainMenuFrame& frame,
                            ImVec2 size, double elapsed, int hovered, int pressed) {
    if (!list || !valid_size(size)) return;
    elapsed = safe_time(elapsed);
    list->PushClipRect({0, 0}, size, false);
    draw_background(list, frame, size);
    draw_logo(list, fonts, frame, elapsed);
    draw_buttons(list, fonts, frame, hovered, pressed);
    list->PopClipRect();
}

void draw_main_menu_backdrop(ImDrawList* list, ImVec2 size, double elapsed, float alpha) {
    if (!list || !valid_size(size) || !(alpha > 0.0F)) return;
    draw_background(list, evaluate_main_menu(size, safe_time(elapsed)), size, alpha);
}

void draw_main_menu(ImDrawList* list, const FontManager& fonts, ImVec2 size,
                    double elapsed, int hovered, int pressed) {
    if (!list || !valid_size(size)) return;
    draw_main_menu_frame(list, fonts, evaluate_main_menu(size, elapsed), size, elapsed, hovered, pressed);
}

void MainMenu::set_active(bool active) noexcept {
    if (active_ == active) return;
    active_ = active;
    pointer_armed_ = false;
    pressed_button_ = -1;
    last_action_time_ = -1.0;
    pending_action_.reset();
    social_requested_ = false;
    alts_requested_ = false;
    start_time_ = std::chrono::steady_clock::now();
}

void MainMenu::render(ImDrawList* list, const FontManager& fonts, ImVec2 size, const PointerInput& pointer) {
    render_at(list, fonts, size, pointer, elapsed());
}

double MainMenu::elapsed() const noexcept {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time_).count();
}

void MainMenu::render_at(ImDrawList* list, const FontManager& fonts, ImVec2 size,
                         const PointerInput& pointer, double elapsed) {
    if (!active_) return;
    elapsed = safe_time(elapsed);
    // Evaluate once per frame and share the buttons between hit testing and
    // drawing instead of running the blob sinusoids twice.
    auto frame = valid_size(size) ? evaluate_main_menu(size, elapsed) : MainMenuFrame{};
    frame.show_social = social_shown_;
    frame.social_badge = social_badge_;
    frame.show_alts = alts_shown_;
    int hovered = -1;
    if (valid_size(size) && pointer.valid) {
        for (std::size_t i = 0; i < frame.buttons.size(); ++i)
            if (frame.buttons[i].contains(pointer.position)) hovered = static_cast<int>(i);
        if (social_shown_ && frame.social.contains(pointer.position)) hovered = social_button;
        if (alts_shown_ && frame.alts.contains(pointer.position)) hovered = alts_button;
    }
    const auto& left = pointer.buttons[0];
    // Require a neutral frame after activation/focus loss. A click which was
    // already held in the previous screen cannot activate a new menu button.
    if (!pointer.valid) { pressed_button_ = -1; pointer_armed_ = false; }
    else if (!pointer_armed_) { if (!left.down && !left.pressed && !left.released) pointer_armed_ = true; }
    else {
        if (left.pressed) pressed_button_ = hovered;
        if (left.released) {
            if (pressed_button_ >= 0 && pressed_button_ == hovered && !pending_action_ && !social_requested_ &&
                !alts_requested_ && (last_action_time_ < 0.0 || elapsed - last_action_time_ >= click_cooldown)) {
                if (pressed_button_ == social_button) social_requested_ = true;
                else if (pressed_button_ == alts_button) alts_requested_ = true;
                else pending_action_ = static_cast<MainMenuAction>(pressed_button_);
                last_action_time_ = elapsed;
            }
            pressed_button_ = -1;
        } else if (!left.down && !left.pressed) pressed_button_ = -1;
    }
    draw_main_menu_frame(list, fonts, frame, size, elapsed, hovered, pressed_button_);
}

std::optional<MainMenuAction> MainMenu::consume_action() noexcept {
    auto action = pending_action_;
    pending_action_.reset();
    return action;
}

void MainMenu::set_social(bool shown, bool badge) noexcept {
    social_shown_ = shown;
    social_badge_ = shown && badge;
    if (!shown) {
        social_requested_ = false;
        if (pressed_button_ == social_button) pressed_button_ = -1;
    }
}

bool MainMenu::consume_social_request() noexcept { return std::exchange(social_requested_, false); }

void MainMenu::set_alts(bool shown) noexcept {
    alts_shown_ = shown;
    if (!shown) {
        alts_requested_ = false;
        if (pressed_button_ == alts_button) pressed_button_ = -1;
    }
}

bool MainMenu::consume_alts_request() noexcept { return std::exchange(alts_requested_, false); }

} // namespace medicon::gui
