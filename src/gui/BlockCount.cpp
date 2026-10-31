#include <medicon/gui/BlockCount.hpp>

#include <medicon/core/ModuleManager.hpp>
#include <medicon/gui/Draw.hpp>
#include <medicon/gui/Glass.hpp>
#include <medicon/gui/LiquidGlass.hpp>
#include <medicon/modules/ScaffoldModule.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace medicon::gui {
namespace {

constexpr float reference_height = 34.0F;
constexpr float reference_padding = 16.0F;
constexpr float vertical_position = 0.66F;       // of the client height, below the crosshair
constexpr float light_angle = -2.19F;            // from the upper left
constexpr float digit_seconds = 0.16F;
constexpr int largest_count = 9999;

constexpr float smooth(float t) {
    t = std::clamp(t, 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

} // namespace

void BlockCountAnimator::set_count(int count) {
    count = std::min(count, largest_count);
    if (count == count_) return;
    const bool first = count_ < 0;
    if (!first) direction_ = count > count_ ? 1.0F : -1.0F;
    count_ = count;
    int places = 0;
    int rest = count;
    for (auto& digit : digits_) {
        const int value = places == 0 || rest > 0 ? rest % 10 : -1;
        if (value >= 0) ++places;
        rest /= 10;
        if (value == digit.value) continue;
        digit.previous = first ? -1 : digit.value;
        digit.value = value;
        digit.progress = first ? 1.0F : 0.0F;
    }
    // Every block dents the glass a little; losing a whole digit flexes it.
    const bool resized = places != places_;
    places_ = places;
    if (!first) press_.velocity = std::min(press_.velocity + (resized ? 13.0F : 3.5F), 18.0F);
}

void BlockCountAnimator::update(int count, const Metrics& metrics, float sway, float dt) {
    const bool wanted = count >= 0;
    if (!shown_ && !wanted) return;
    if (wanted) set_count(count);
    const float rest = std::max(metrics.height * 1.22F, metrics.padding * 2.0F + metrics.digit_width * static_cast<float>(places_));
    if (!shown_) {
        // Materialize at the final width; a pane must not unroll from nothing.
        width_ = {rest, 0.0F};
        shown_ = true;
    }
    width_.step(rest, 250.0F, 0.34F, dt);
    // Liquid keeps its volume: closing in swells it, opening thins it, a beat late.
    squeeze_.step(std::clamp(-width_.velocity / (metrics.height * 2.6F), -1.0F, 1.0F), 420.0F, 0.42F, dt);
    press_.step(0.0F, 320.0F, 0.5F, dt);
    appear_.step(wanted ? 1.0F : 0.0F, 200.0F, 0.62F, dt);
    light_ = follow(light_, light_angle + sway, dt, 0.16F);
    for (auto& digit : digits_) digit.progress = std::min(1.0F, digit.progress + dt / digit_seconds);
    if (!wanted && appear_.value < 0.01F && std::abs(appear_.velocity) < 0.1F) *this = {};
}

float BlockCountAnimator::height(const Metrics& metrics) const noexcept {
    return metrics.height * (1.0F + 0.08F * squeeze_.value - 0.05F * press_.value);
}
float BlockCountAnimator::waist() const noexcept { return 0.08F * squeeze_.value; }
float BlockCountAnimator::energy() const noexcept { return std::clamp(press_.value * 1.6F, 0.0F, 1.0F); }
float BlockCountAnimator::lens() const noexcept { return std::clamp(appear_.value, 0.0F, 1.0F); }
float BlockCountAnimator::opacity() const noexcept { return smooth(appear_.value / 0.3F); }
float BlockCountAnimator::scale() const noexcept { return at_rest() ? 1.0F : 0.72F + 0.28F * appear_.value; }
bool BlockCountAnimator::at_rest() const noexcept {
    return std::abs(width_.velocity) < 0.5F && std::abs(appear_.velocity) < 0.05F && std::abs(appear_.value - 1.0F) < 0.005F;
}

void BlockCount::render(GuiContext& context) {
    const auto* scaffold = dynamic_cast<const ScaffoldModule*>(context.modules.find(ScaffoldModule::module_id));
    const int count = scaffold && scaffold->enabled() ? scaffold->block_count() : -1;
    if (!animator_.visible() && count < 0) return;
    const FontFace face = context.fonts.face(FontRole::BlockCount);
    if (!face.font) return;

    auto& fonts = context.fonts;
    const float s = context.frame.hud_scale;
    float cell = 0.0F;
    for (const char* numeral : {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"})
        cell = std::max(cell, fonts.measure(FontRole::BlockCount, numeral));
    const BlockCountAnimator::Metrics metrics{cell, reference_padding * s, reference_height * s};
    // Turning the view sways the light, the way tilting a phone moves its glints.
    const float sway = scaffold ? 0.75F * std::sin(scaffold->view_yaw() * std::numbers::pi_v<float> / 180.0F) : 0.0F;
    animator_.update(count, metrics, sway, context.frame.dt);
    if (!animator_.visible()) return;

    const ImVec2 center{std::round(context.frame.client_w * 0.5F), std::round(context.frame.client_h * vertical_position)};
    const float scale = animator_.scale();
    LiquidGlassFrame glass;
    glass.center = center;
    glass.size = {animator_.width() * scale, animator_.height(metrics) * scale};
    glass.scale = s;
    glass.waist = animator_.waist();
    glass.lens = animator_.lens();
    glass.energy = animator_.energy();
    glass.light = animator_.light();
    glass.opacity = animator_.opacity();
    publish_liquid_glass(glass);

    // Digits sit in equal cells from the right edge, which the spring moves.
    auto* list = context.draw_list;
    const ScaleAround pop(list, center, scale, animator_.opacity());
    const float right = center.x + animator_.width() * 0.5F - metrics.padding;
    const float baseline = center.y + face.cap_height * 0.5F;
    const float slide = face.cap_height * 0.55F * animator_.direction();
    // A resting digit lands on whole pixels: between them the glyph goes soft.
    const auto stamp = [&](int value, float x, float y, float opacity, bool snap) {
        if (value < 0 || opacity <= 0.0F) return;
        const char text[]{static_cast<char>('0' + value), '\0'};
        const float inset = (cell - fonts.measure(FontRole::BlockCount, text)) * 0.5F;
        fonts.draw(list, FontRole::BlockCount, x + inset, y + 1.0F * s, to_u32(Color{0.0F, 0.0F, 0.0F, 1.0F}, 0.45F * opacity), text, snap);
        fonts.draw(list, FontRole::BlockCount, x + inset, y, to_u32(Color{1.0F, 1.0F, 1.0F, 1.0F}, opacity), text, snap);
    };
    for (std::size_t place = 0; place < BlockCountAnimator::digit_capacity; ++place) {
        const auto& digit = animator_.digit(place);
        const float x = right - cell * static_cast<float>(place + 1);
        const float t = ease_out_cubic(digit.progress);
        stamp(digit.value, x, baseline - slide * (1.0F - t), t, animator_.at_rest() && digit.progress >= 1.0F);
        if (digit.progress < 1.0F) stamp(digit.previous, x, baseline + slide * t, 1.0F - t, false);
    }
}

} // namespace medicon::gui
