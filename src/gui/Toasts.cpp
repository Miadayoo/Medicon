#include <medicon/gui/Toasts.hpp>

#include <medicon/gui/Draw.hpp>
#include <medicon/gui/GuiMath.hpp>
#include <medicon/gui/Shadow.hpp>
#include <medicon/gui/SocialFonts.hpp>
#include <medicon/gui/Theme.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace medicon::gui {
namespace {

namespace style = theme::social;

const Color& band_color(ToastKind kind) {
    switch (kind) {
    case ToastKind::Dm: return style::toast_band_dm;
    case ToastKind::Friend: return style::toast_band_friend;
    case ToastKind::News: return style::toast_band_news;
    case ToastKind::System: return style::toast_band_system;
    }
    return style::toast_band_news;
}

std::string merged_label(int merged) { return "+" + std::to_string(merged); }

} // namespace

void ToastManager::push(Toast toast) {
    if (toast.kind == ToastKind::Dm && toast.sender != 0) {
        const auto same_sender = [&](const Entry& entry) {
            return entry.toast.kind == ToastKind::Dm && entry.toast.sender == toast.sender;
        };
        // Spec 4.7: one toast per sender; show the newest text and restart its timer.
        if (const auto found = std::find_if(visible_.begin(), visible_.end(), same_sender); found != visible_.end()) {
            found->toast.body = std::move(toast.body);
            ++found->merged;
            if (found->phase != Phase::Entering) {
                found->phase = Phase::Holding;
                found->time = 0.0F;
            }
            return;
        }
        if (const auto found = std::find_if(queued_.begin(), queued_.end(), same_sender); found != queued_.end()) {
            found->toast.body = std::move(toast.body);
            ++found->merged;
            return;
        }
    }
    // A burst after a long absence keeps the newest notifications.
    if (queued_.size() >= max_queued) queued_.pop_front();
    Entry entry;
    entry.toast = std::move(toast);
    entry.id = next_id_++;
    queued_.push_back(std::move(entry));
    promote();
}

void ToastManager::promote() {
    while (visible_.size() < max_visible && !queued_.empty()) {
        visible_.push_back(std::move(queued_.front()));
        queued_.pop_front();
    }
}

std::optional<Toast> ToastManager::consume_click() {
    auto clicked = std::move(clicked_);
    clicked_.reset();
    return clicked;
}

bool ToastManager::covers(ImVec2 point) const noexcept {
    for (const auto& [min, max] : hit_areas_)
        if (point.x >= min.x && point.x < max.x && point.y >= min.y && point.y < max.y) return true;
    return false;
}

float ToastManager::opacity(const Entry& entry) {
    switch (entry.phase) {
    case Phase::Entering: return ease_out_cubic(entry.time / style::timing::toast_fade_in);
    case Phase::Holding: return 1.0F;
    case Phase::Leaving: return 1.0F - std::clamp(entry.time / style::timing::toast_fade_out, 0.0F, 1.0F);
    }
    return 1.0F;
}

float ToastManager::slide(const Entry& entry) {
    if (entry.phase != Phase::Entering) return 0.0F;
    return -(1.0F - ease_out_cubic(entry.time / style::timing::toast_fade_in)) * style::metrics::toast_slide;
}

void ToastManager::advance(float dt, bool in_menu, int hovered) {
    const float hold = in_menu ? style::timing::toast_hold_menu : style::timing::toast_hold_game;
    for (std::size_t i = 0; i < visible_.size();) {
        auto& entry = visible_[i];
        const bool paused = in_menu && entry.phase == Phase::Holding && static_cast<int>(i) == hovered;
        if (!paused) entry.time += dt;
        if (entry.phase == Phase::Entering && entry.time >= style::timing::toast_fade_in) {
            entry.phase = Phase::Holding;
            entry.time = 0.0F;
        } else if (entry.phase == Phase::Holding && entry.time >= hold) {
            entry.phase = Phase::Leaving;
            entry.time = 0.0F;
        } else if (entry.phase == Phase::Leaving && entry.time >= style::timing::toast_fade_out) {
            visible_.erase(visible_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
    promote();
}

void ToastManager::render(ImDrawList* list, const SocialFonts& fonts, IShadowRenderer& shadows, const ToastFrame& frame) {
    hit_areas_.clear();
    if (empty() || !list || !(frame.display.x > 0.0F) || !(frame.display.y > 0.0F)) return;
    namespace metrics = style::metrics;
    // Spec 4.6: Social sizes scale with the window height alone.
    const float scale = frame.display.y / metrics::reference_height;
    const float height = metrics::toast_height * scale;
    const float gap = metrics::toast_gap * scale;
    const float top = (metrics::toast_top + (frame.in_menu ? 0.0F : frame.offset_y)) * scale;
    const float min_width = std::min(metrics::toast_min_width * scale, frame.display.x - gap * 2.0F);
    const float max_width = std::max(min_width, std::min(frame.display.x * metrics::toast_max_width_ratio,
                                                         frame.display.x - gap * 2.0F));
    const float padding = (metrics::toast_padding_left + metrics::toast_padding_right) * scale;
    const float count_gap = metrics::toast_count_gap * scale;

    struct Layout {
        ImVec2 min{}, max{};
    };
    std::array<Layout, max_visible> layouts{};
    int hovered = -1;
    for (std::size_t i = 0; i < visible_.size(); ++i) {
        auto& entry = visible_[i];
        const float count_width = entry.merged > 0 ? count_gap + fonts.measure(SocialText::Title, merged_label(entry.merged)) : 0.0F;
        const float content = std::max(fonts.measure(SocialText::Title, entry.toast.title) + count_width,
                                       fonts.measure(SocialText::Body, entry.toast.body));
        const float width = std::round(std::clamp(content + padding, min_width, max_width));
        const float target = static_cast<float>(i) * (height + gap);
        entry.slot_y = entry.slot_y < 0.0F ? target : follow(entry.slot_y, target, frame.dt, style::timing::toast_reflow_tau);
        const float x = std::round((frame.display.x - width) * 0.5F);
        const float y = top + entry.slot_y + slide(entry) * scale;
        layouts[i] = {{x, y}, {x + width, y + height}};
        if (frame.pointer.valid) hit_areas_.emplace_back(layouts[i].min, layouts[i].max);
        const auto& pointer = frame.pointer;
        if (hovered < 0 && pointer.valid && pointer.position.x >= x && pointer.position.x < x + width &&
            pointer.position.y >= y && pointer.position.y < y + height)
            hovered = static_cast<int>(i);
    }

    // Clicks count only where the client draws a cursor (pointer.valid): while
    // playing, toasts have no hit area at all (spec 4.7).
    const auto& left = frame.pointer.buttons[0];
    if (!frame.pointer.valid) {
        pressed_id_ = 0;
    } else {
        if (left.pressed) pressed_id_ = hovered >= 0 ? visible_[static_cast<std::size_t>(hovered)].id : 0;
        if (left.released) {
            if (hovered >= 0 && visible_[static_cast<std::size_t>(hovered)].id == pressed_id_) {
                auto& entry = visible_[static_cast<std::size_t>(hovered)];
                clicked_ = entry.toast;
                entry.phase = Phase::Leaving;
                entry.time = 0.0F;
            }
            pressed_id_ = 0;
        }
    }

    const float radius = metrics::toast_radius * scale;
    // A soft blue shadow, as under the Main Menu's plates (never black).
    const ShadowParams shadow{style::shadow_toast.opacity, style::shadow_toast.blur * 0.5F * scale, 0.0F,
                              style::shadow_toast.offset_y * scale, to_u32(style::shadow_tint)};
    const float title_height = fonts.line_height(SocialText::Title);
    const float body_height = fonts.line_height(SocialText::Body);
    const float line_gap = metrics::toast_line_gap * scale;
    // One texture for shapes and text: a single draw call for every toast.
    const SocialFonts::Scope scope(fonts, list);
    for (std::size_t i = 0; i < visible_.size(); ++i) {
        const auto& entry = visible_[i];
        const auto [min, max] = layouts[i];
        const float alpha = opacity(entry);
        if (alpha <= 0.0F) continue;
        shadows.draw(list, min, max, CornerRadii::all(radius), shadow, alpha);
        list->AddRectFilled(min, max, to_u32(style::toast_background, alpha), radius);
        list->AddRect(min, max, to_u32(theme::white, alpha * 0.9F), radius, 0, 1.0F);
        // Clip a full card to the band so the strip follows the rounded corners.
        list->PushClipRect(min, {min.x + metrics::toast_band * scale, max.y}, true);
        list->AddRectFilled(min, max, to_u32(band_color(entry.toast.kind), alpha), radius);
        list->PopClipRect();

        const float text_x = min.x + metrics::toast_padding_left * scale;
        const float text_width = max.x - text_x - metrics::toast_padding_right * scale;
        const float title_top = min.y + (height - title_height - line_gap - body_height) * 0.5F;
        const float body_top = title_top + title_height + line_gap;
        std::string count;
        float count_width = 0.0F;
        if (entry.merged > 0) {
            count = merged_label(entry.merged);
            count_width = fonts.measure(SocialText::Title, count);
        }
        const float title_room = text_width - (count_width > 0.0F ? count_width + count_gap : 0.0F);
        const auto title = fonts.ellipsize(SocialText::Title, entry.toast.title, title_room, title_scratch_);
        fonts.draw(list, SocialText::Title, text_x, title_top, to_u32(style::heading, alpha), title);
        if (!count.empty()) {
            fonts.draw(list, SocialText::Title, text_x + fonts.measure(SocialText::Title, title) + count_gap, title_top,
                       to_u32(style::text_secondary, alpha), count);
        }
        const auto body = fonts.ellipsize(SocialText::Body, entry.toast.body, text_width, body_scratch_);
        fonts.draw(list, SocialText::Body, text_x, body_top, to_u32(style::text, alpha), body);
    }

    advance(frame.dt, frame.in_menu, hovered);
}

} // namespace medicon::gui
