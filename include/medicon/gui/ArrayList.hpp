#pragma once

#include "medicon/gui/GuiContext.hpp"

#include <string>
#include <vector>

namespace medicon {
class ArrayListModule;
}

namespace medicon::gui {

// Rendering-independent Edge-lit layout, transitions and tube pulses.
class ArrayListAnimator {
public:
    struct Entry {
        std::string id;     // module id (unique)
        std::string name;   // display name
        std::string suffix; // empty when hidden
        float width = 0.0F; // complete plate width, including padding, px
        int order = 0;      // registration order
        bool enabled = false;
        bool operator==(const Entry&) const = default;
    };
    struct Params {
        bool animate = true;
        bool sort_by_width = true;
        bool pulse = true;
        float top = 8.0F;
        float row_height = 24.0F;
        float gap = 0.0F;
        bool operator==(const Params&) const = default;
    };
    struct Row {
        std::string id;
        std::string name;
        std::string suffix;
        float width = 0.0F;
        int order = 0;
        float p = 0.0F; // visible slide fraction, retained for callers of value()
        int dir = 1;    // +1 entering/visible, -1 leaving
        float y = 0.0F; // current top, px
        float target_y = 0.0F;
        float display_width = 0.0F; // follows measured width when a suffix changes
        float height = 0.0F;       // current occupied layout height
        float slide = 1.0F;        // 0 visible, 1 beyond the right edge
        float text_alpha = 0.0F;   // delayed ON reveal; includes OFF fade
        float opacity = 1.0F;      // surface opacity (OFF fade)
        float activity = 0.0F;     // light intensity L, exactly zero at rest
        bool settled = false;     // positions, dimensions and visibility at rest
        float value() const;

        // Transition snapshots keep rapid reversals continuous.
        float event_time = 0.0F;
        float start_height = 0.0F;
        float start_slide = 1.0F;
        float start_text_alpha = 0.0F;
        float start_opacity = 1.0F;
        float start_activity = 0.0F;
        float pulse_wait = 0.0F;
        bool pulse_event = false;
    };
    struct Pulse {
        std::string row_id;
        float y = 0.0F;          // absolute center, in the same coordinates as Row::y
        float strength = 1.0F;   // traveling intensity, then decaying arrival light
        bool travelling = true;
        float age = 0.0F;
        float peak = 1.0F;
    };

    void update(const std::vector<Entry>& entries, const Params& params, float dt);
    const std::vector<Row>& rows() const { return rows_; }
    const std::vector<Pulse>& pulses() const { return pulses_; }
    float height() const { return height_; }
    void clear() { rows_.clear(); pulses_.clear(); height_ = 0.0F; inputs_valid_ = false; idle_ = false; }

    static bool before(const Row& a, const Row& b, bool sort_by_width);

private:
    std::vector<Row> rows_;
    std::vector<Pulse> pulses_;
    float height_ = 0.0F;
    // Module inputs usually stay identical for thousands of rendered frames.
    // Keep their owning snapshot so neither row matching nor sorting is repeated
    // while only the animation clock advances (or the complete list is at rest).
    std::vector<Entry> last_entries_;
    Params last_params_{};
    bool inputs_valid_ = false;
    bool idle_ = false;
};

class ArrayList {
public:
    void render(GuiContext& context);
    const ArrayListAnimator& animator() const { return animator_; }

private:
    ArrayListAnimator animator_;
    std::vector<ArrayListAnimator::Entry> entries_;
};

} // namespace medicon::gui
