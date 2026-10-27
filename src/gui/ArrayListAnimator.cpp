#include <medicon/gui/ArrayList.hpp>
#include <medicon/gui/GuiMath.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace medicon::gui {
namespace {

constexpr float height_in = 0.200F;
constexpr float slide_in = 0.240F;
constexpr float text_delay = 0.060F;
constexpr float text_in = 0.160F;
constexpr float pulse_travel = 0.220F;
constexpr float light_decay = 1.200F;
constexpr float slide_out = 0.200F;
constexpr float collapse_delay = 0.080F;
constexpr float height_out = 0.200F;
constexpr float layout_tau = 0.060F;

float ease_in_cubic(float t) {
    t = std::clamp(t, 0.0F, 1.0F);
    return t * t * t;
}

float mix(float a, float b, float t) { return a + (b - a) * t; }

float phase(float elapsed, float duration) {
    // Accumulated frame deltas can miss an exact millisecond boundary by a few
    // float ULPs. Finish at that boundary, rather than waiting another frame.
    if (elapsed + 0.000001F >= duration) return 1.0F;
    return std::max(0.0F, elapsed / duration);
}

float decay(float elapsed) {
    return 1.0F - ease_out_cubic(phase(elapsed, light_decay));
}

void begin_transition(ArrayListAnimator::Row& row, bool enabled, bool pulse) {
    row.dir = enabled ? 1 : -1;
    row.event_time = 0.0F;
    row.start_height = row.height;
    row.start_slide = row.slide;
    row.start_text_alpha = row.text_alpha;
    row.start_opacity = row.opacity;
    row.start_activity = row.activity;
    row.pulse_event = pulse;
    row.pulse_wait = pulse && !enabled ? pulse_travel : 0.0F;
    row.settled = false;
}

} // namespace

float ArrayListAnimator::Row::value() const { return p; }

bool ArrayListAnimator::before(const Row& a, const Row& b, bool sort_by_width) {
    if (sort_by_width && a.width != b.width) return a.width > b.width;
    return a.order < b.order;
}

void ArrayListAnimator::update(const std::vector<Entry>& entries, const Params& params, float dt) {
    dt = std::isfinite(dt) ? std::max(0.0F, dt) : 0.0F;
    const bool entries_changed = !inputs_valid_ || entries != last_entries_;
    const bool sort_changed = !inputs_valid_ || params.sort_by_width != last_params_.sort_by_width;
    if (!entries_changed && params == last_params_ && idle_) return;
    if (entries_changed) last_entries_ = entries;
    last_params_ = params;
    inputs_valid_ = true;
    std::vector<std::string> fresh_ids;
    const auto trigger = [&](Row& row, bool enabled) {
        begin_transition(row, enabled, params.animate && params.pulse);
        if (row.pulse_event) {
            if (pulses_.size() == 4) pulses_.erase(pulses_.begin());
            Pulse pulse;
            pulse.row_id = row.id;
            pulse.y = params.top;
            pulse.peak = enabled ? 1.0F : 0.6F;
            pulses_.push_back(std::move(pulse));
        }
    };
    if (entries_changed) {
        for (const auto& entry : entries) {
            auto row = std::find_if(rows_.begin(), rows_.end(), [&](const Row& r) { return r.id == entry.id; });
            if (row == rows_.end()) {
                if (!entry.enabled) continue;
                Row fresh;
                fresh.id = entry.id;
                fresh.name = entry.name;
                fresh.suffix = entry.suffix;
                fresh.width = std::max(0.0F, entry.width);
                fresh.display_width = fresh.width;
                fresh.order = entry.order;
                fresh_ids.push_back(entry.id);
                rows_.push_back(std::move(fresh));
                trigger(rows_.back(), true);
            } else {
                row->order = entry.order;
                if (entry.enabled) {
                    row->name = entry.name;
                    row->suffix = entry.suffix;
                    row->width = std::max(0.0F, entry.width);
                }
                if (entry.enabled != (row->dir > 0)) trigger(*row, entry.enabled);
            }
        }
        for (auto& row : rows_) {
            const auto entry = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.id == row.id; });
            if (entry == entries.end() && row.dir > 0) trigger(row, false);
        }
    }
    if (!params.animate || !params.pulse) pulses_.clear();

    for (auto& row : rows_) {
        row.event_time = std::min(2.0F, row.event_time + dt);
        if (!params.animate) {
            row.event_time = 2.0F;
            row.pulse_event = false;
            row.height = row.dir > 0 ? params.row_height : 0.0F;
            row.slide = row.dir > 0 ? 0.0F : 1.0F;
            row.text_alpha = row.opacity = row.dir > 0 ? 1.0F : 0.0F;
            row.display_width = row.width;
            row.activity = 0.0F;
        } else {
            if (!params.pulse) {
                row.pulse_event = false;
                row.pulse_wait = 0.0F;
            }
            const float elapsed = row.event_time;
            if (row.dir > 0) {
                row.height = mix(row.start_height, params.row_height, ease_out_cubic(phase(elapsed, height_in)));
                row.slide = mix(row.start_slide, 0.0F, ease_out_cubic(phase(elapsed, slide_in)));
                row.text_alpha = mix(row.start_text_alpha, 1.0F, ease_out_cubic(phase(elapsed - text_delay, text_in)));
                row.opacity = mix(row.start_opacity, 1.0F, ease_out_cubic(phase(elapsed, slide_in)));
            } else {
                const float departure = elapsed - row.pulse_wait;
                const float fade = ease_in_cubic(phase(departure, slide_out));
                row.slide = mix(row.start_slide, 1.0F, fade);
                row.text_alpha = mix(row.start_text_alpha, 0.0F, fade);
                row.opacity = mix(row.start_opacity, 0.0F, fade);
                row.height = mix(row.start_height, 0.0F, ease_in_cubic(phase(departure - collapse_delay, height_out)));
            }
            row.display_width = follow(row.display_width, row.width, dt, layout_tau);
            if (!row.pulse_event) row.activity = 0.0F;
            else if (phase(elapsed, pulse_travel) < 1.0F) row.activity = row.start_activity * decay(elapsed);
            else row.activity = (row.dir > 0 ? 1.0F : 0.6F) * decay(elapsed - pulse_travel);
        }
        row.p = 1.0F - row.slide;
    }

    if (entries_changed || sort_changed) {
        std::stable_sort(rows_.begin(), rows_.end(), [&](const Row& a, const Row& b) {
            return before(a, b, params.sort_by_width);
        });
    }
    float target = params.top;
    for (auto& row : rows_) {
        row.target_y = target;
        const bool fresh = std::find(fresh_ids.begin(), fresh_ids.end(), row.id) != fresh_ids.end();
        row.y = fresh || !params.animate ? target : follow(row.y, target, dt, layout_tau);
        target += row.height + (row.height > 0.0F ? params.gap : 0.0F);
        row.settled = row.dir > 0 && row.slide == 0.0F && row.height == params.row_height &&
                      row.text_alpha == 1.0F && row.opacity == 1.0F && row.y == row.target_y &&
                      row.width == row.display_width;
    }
    const float target_height = std::max(0.0F, target - params.top - (rows_.empty() ? 0.0F : params.gap));
    height_ = params.animate ? follow(height_, target_height, dt, layout_tau) : target_height;

    for (auto& pulse : pulses_) {
        pulse.age += dt;
        const auto row = std::find_if(rows_.begin(), rows_.end(), [&](const Row& r) { return r.id == pulse.row_id; });
        if (row == rows_.end()) { pulse.strength = 0.0F; continue; }
        const float center = row->y + 0.5F * row->height;
        pulse.travelling = phase(pulse.age, pulse_travel) < 1.0F;
        pulse.y = mix(params.top, center, ease_out_cubic(phase(pulse.age, pulse_travel)));
        pulse.strength = pulse.travelling ? 1.0F : pulse.peak * decay(pulse.age - pulse_travel);
    }
    std::erase_if(pulses_, [](const Pulse& pulse) { return pulse.strength <= 0.0F; });
    std::erase_if(rows_, [&](const Row& row) {
        return row.dir < 0 && row.height == 0.0F && row.opacity == 0.0F;
    });
    // A removed row has no surface to illuminate; drop its remaining pulse too.
    std::erase_if(pulses_, [&](const Pulse& pulse) {
        return std::none_of(rows_.begin(), rows_.end(), [&](const Row& row) { return row.id == pulse.row_id; });
    });
    idle_ = pulses_.empty() && height_ == target_height &&
        std::all_of(rows_.begin(), rows_.end(), [](const Row& row) {
            return row.settled && row.activity == 0.0F && row.event_time >= 2.0F;
        });
}

} // namespace medicon::gui
