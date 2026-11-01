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
#include <utility>

namespace medicon::gui {
void ToastManager::remove_owner(std::string_view owner,std::uint64_t generation) {
    const auto match=[&](const auto& entry){return entry.toast.owner==owner&&entry.toast.generation==generation;};
    std::erase_if(visible_,match); std::erase_if(queued_,match);
    if(clicked_&&clicked_->owner==owner&&clicked_->generation==generation) clicked_.reset();
    pressed_id_=0; hit_areas_.clear(); music_anchor_.reset();
}
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

// Mid-change width of a shown toast whose content changed (spec toast-anim 4).
float final_width(const ToastManager::Entry& entry) {
    const float duration = style::timing::toast_resize;
    if (entry.resize >= duration) return entry.width;
    return std::lerp(entry.width_from, entry.width, ease_out_cubic(entry.resize / duration));
}

} // namespace

ToastManager::Morph ToastManager::morph(float anim) {
    namespace timing = style::timing;
    const float pop = std::clamp(anim / timing::toast_pop, 0.0F, 1.0F);
    const float stretch = std::clamp((anim - timing::toast_stretch_start) / timing::toast_stretch, 0.0F, 1.0F);
    const float content = std::clamp((anim - timing::toast_content_start) / timing::toast_content, 0.0F, 1.0F);
    Morph m;
    m.scale = std::max(0.0F, ease_out_back(pop, timing::toast_pop_back));
    m.opacity = std::clamp(anim / timing::toast_fade, 0.0F, 1.0F);
    m.stretch = ease_out_back(stretch, timing::toast_stretch_back);
    m.round = ease_out_cubic(stretch);
    m.content = ease_out_cubic(content);
    return m;
}

void ToastManager::push(Toast toast) {
    if (toast.kind == ToastKind::Dm && toast.sender != 0) {
        const auto same_sender = [&](const Entry& entry) {
            return entry.toast.kind == ToastKind::Dm && entry.toast.sender == toast.sender;
        };
        // Spec 4.7: one toast per sender; show the newest text and restart its timer.
        if (const auto found = std::find_if(visible_.begin(), visible_.end(), same_sender); found != visible_.end()) {
            found->toast.body = std::move(toast.body);
            ++found->merged;
            found->relayout = true;
            found->dismissed = false;
            // A leaving toast grows back from where it is; its timer restarts once it is whole.
            if (found->phase == Phase::Leaving) found->phase = Phase::Entering;
            else if (found->phase == Phase::Holding) found->time = 0.0F;
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

void ToastManager::advance(float dt, bool in_menu, int hovered) {
    namespace timing = style::timing;
    const float hold = in_menu ? timing::toast_hold_menu : timing::toast_hold_game;
    // Animations step at most 100 ms per frame so a stall does not skip them;
    // the shown timer keeps wall time.
    const float step = accessibility_preferences.reduced_motion ? timing::toast_enter : clamp_dt(dt) * animation_speed_;
    for (std::size_t i = 0; i < visible_.size();) {
        auto& entry = visible_[i];
        const bool under = in_menu && static_cast<int>(i) == hovered;
        entry.resize = std::min(entry.resize + step, timing::toast_resize);
        // The timeline is continuous, so turning back mid-way never jumps.
        if (entry.phase == Phase::Leaving && under && !entry.dismissed) entry.phase = Phase::Entering;
        if (entry.phase == Phase::Entering) {
            entry.anim += step;
            if (entry.anim >= timing::toast_enter) {
                entry.anim = timing::toast_enter;
                entry.phase = Phase::Holding;
                entry.time = 0.0F;
            }
        } else if (entry.phase == Phase::Holding) {
            if (!under) entry.time += dt;
            if (entry.time >= hold) entry.phase = Phase::Leaving;
        } else {
            entry.anim -= step * timing::toast_exit_speed;
            if (entry.anim <= 0.0F) {
                visible_.erase(visible_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
        }
        ++i;
    }
    promote();
}

void ToastManager::layout(Entry& entry, const SocialFonts& fonts, float scale, float min_width, float max_width) {
    const bool resized = entry.layout_scale != scale || entry.layout_revision != fonts.revision();
    if (!entry.relayout && !resized) return;
    namespace metrics = style::metrics;
    const float count_gap = metrics::toast_count_gap * scale;
    entry.count = entry.merged > 0 ? merged_label(entry.merged) : std::string{};
    const float count_room = entry.count.empty() ? 0.0F : count_gap + fonts.measure(SocialText::Title, entry.count);
    const float content = std::max(fonts.measure(SocialText::Title, entry.toast.title) + count_room,
                                   fonts.measure(SocialText::Body, entry.toast.body));
    const float padding = (metrics::toast_padding_left + metrics::toast_padding_right) * scale;
    const float width = std::round(std::clamp(content + padding, min_width, max_width));
    // Only new content on a toast that is already whole eases to its width;
    // while entering, or after a window resize, the width is just replaced.
    const bool ease = entry.relayout && !resized && entry.phase == Phase::Holding && width != entry.width;
    entry.width_from = ease ? final_width(entry) : width;
    entry.resize = ease ? 0.0F : style::timing::toast_resize;
    entry.width = width;

    const float text_width = width - padding;
    entry.title_line = fonts.ellipsize(SocialText::Title, entry.toast.title, text_width - count_room, scratch_);
    entry.body_line = fonts.ellipsize(SocialText::Body, entry.toast.body, text_width, scratch_);
    entry.count_x = entry.count.empty() ? 0.0F : fonts.measure(SocialText::Title, entry.title_line) + count_gap;
    entry.layout_scale = scale;
    entry.layout_revision = fonts.revision();
    entry.relayout = false;
}

void ToastManager::render(ImDrawList* list, const SocialFonts& fonts, IShadowRenderer& shadows, const ToastFrame& frame) {
    hit_areas_.clear();
    music_anchor_.reset();
    if (empty() || !list || !(frame.display.x > 0.0F) || !(frame.display.y > 0.0F)) return;
    namespace metrics = style::metrics;
    // Spec 4.6: Social sizes scale with the window height alone.
    const float scale = frame.display.y / metrics::reference_height;
    const float height = metrics::toast_height * scale;
    const float gap = metrics::toast_gap * scale;
    const float top = (metrics::toast_top + (frame.in_menu ? 0.0F : frame.offset_y)) * scale;
    const float reserve = frame.music_companion ? 80.0F * scale : 0.0F;
    const float available = std::max(1.0F, (frame.display.x - gap * 2.0F - reserve * 2.0F) /
                                                   (frame.music_companion ? 1.05F : 1.0F));
    const float min_width = std::min(metrics::toast_min_width * scale, available);
    const float max_width = std::max(min_width, std::min(frame.display.x * metrics::toast_max_width_ratio,
                                                         available));
    // The first frame after none were shown starts every animation at its beginning.
    const float dt = std::exchange(idle_, false) ? 0.0F : frame.dt;

    struct Layout {
        ImVec2 min{}, max{};
        Morph morph{};
    };
    std::array<Layout, max_visible> layouts{};
    int hovered = -1;
    const float center_x = frame.display.x * 0.5F;
    for (std::size_t i = 0; i < visible_.size(); ++i) {
        auto& entry = visible_[i];
        layout(entry, fonts, scale, min_width, max_width);
        // Leaving toasts keep their slot until removed; the ones below slide up after.
        const float target = static_cast<float>(i) * (height + gap);
        entry.slot_y = entry.slot_y < 0.0F
                           ? target
                           : follow(entry.slot_y, target, clamp_dt(dt) * animation_speed_, style::timing::toast_reflow_tau);
        // Scale and stretch both grow from the toast's centre.
        const Morph m = morph(entry.anim);
        const float half_w = (height + (final_width(entry) - height) * m.stretch) * m.scale * 0.5F;
        const float half_h = height * m.scale * 0.5F;
        const float center_y = top + entry.slot_y + height * 0.5F;
        if (i == 0) music_anchor_ = ImVec2{center_x - std::max(final_width(entry) * 0.5F, half_w), center_y};
        layouts[i] = {{center_x - half_w, center_y - half_h}, {center_x + half_w, center_y + half_h}, m};
        if (m.scale < 0.001F) continue;
        const ImVec2 min = layouts[i].min, max = layouts[i].max;
        if (frame.pointer.valid) hit_areas_.emplace_back(min, max);
        const auto& pointer = frame.pointer;
        if (hovered < 0 && pointer.valid && pointer.position.x >= min.x && pointer.position.x < max.x &&
            pointer.position.y >= min.y && pointer.position.y < max.y)
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
                entry.dismissed = true;
            }
            pressed_id_ = 0;
        }
    }

    const float radius = metrics::toast_radius * scale;
    // A soft blue shadow, as under the Main Menu's plates (never black). One
    // blurred circle of diameter H, nine-sliced onto every frame of every toast.
    // Built once on a worker at the 1080p reference size (skipped until it
    // lands) and scaled with the toast, blur and offset included, so its cost
    // does not grow with the window.
    const ShadowParams shadow{style::shadow_toast.opacity, style::shadow_toast.blur * 0.5F, 0.0F,
                              style::shadow_toast.offset_y, to_u32(style::shadow_tint)};
    const ImVec2 shadow_source{metrics::toast_height, metrics::toast_height};
    const CornerRadii circle = CornerRadii::all(metrics::toast_height * 0.5F);
    shadows.prepare(shadow_source, circle, shadow);
    const float title_height = fonts.line_height(SocialText::Title);
    const float body_height = fonts.line_height(SocialText::Body);
    const float line_gap = metrics::toast_line_gap * scale;
    // One texture for shapes and text: a single draw call for every toast.
    const SocialFonts::Scope scope(fonts, list);
    for (std::size_t i = 0; i < visible_.size(); ++i) {
        const auto& entry = visible_[i];
        const auto& [min, max, m] = layouts[i];
        const float alpha = m.opacity;
        if (m.scale < 0.001F || alpha <= 0.0F) continue;
        const float rounding = std::lerp(height * 0.5F, radius, m.round) * m.scale;
        const Color& kind = band_color(entry.toast.kind);
        shadows.draw_stretched(list, min, max, shadow_source, circle, shadow, alpha);
        list->AddRectFilled(min, max, to_u32(style::toast_background, alpha), rounding);
        list->AddRect(min, max, to_u32(theme::white, alpha * 0.9F), rounding, 0, 1.0F);

        // The kind's colour as a dot: centred in the circle, then riding the
        // left end while it stretches, fading out as the content comes in.
        const float center_y = (min.y + max.y) * 0.5F;
        if (const float dot = alpha * (1.0F - m.content); dot > 0.0F) {
            list->AddCircleFilled({min.x + (max.y - min.y) * 0.5F, center_y},
                                  metrics::toast_dot * 0.5F * scale * m.scale, to_u32(kind, dot));
        }
        const float content = alpha * m.content;
        if (content <= 0.0F) continue;
        list->PushClipRect(min, max, true);
        // Clip a full card to the band so the strip follows the rounded corners.
        list->PushClipRect(min, {min.x + metrics::toast_band * scale, max.y}, true);
        list->AddRectFilled(min, max, to_u32(kind, content), rounding);
        list->PopClipRect();

        const float text_x = std::round(min.x + metrics::toast_padding_left * scale);
        const float title_top = center_y - (title_height + line_gap + body_height) * 0.5F;
        const float body_top = title_top + title_height + line_gap;
        fonts.draw(list, SocialText::Title, text_x, title_top, to_u32(style::heading, content), entry.title_line);
        if (!entry.count.empty()) {
            fonts.draw(list, SocialText::Title, text_x + entry.count_x, title_top, to_u32(style::text_secondary, content),
                       entry.count);
        }
        fonts.draw(list, SocialText::Body, text_x, body_top, to_u32(style::text, content), entry.body_line);
        list->PopClipRect();
    }

    advance(dt, frame.in_menu, hovered);
    if (visible_.empty()) idle_ = true;
}

} // namespace medicon::gui
