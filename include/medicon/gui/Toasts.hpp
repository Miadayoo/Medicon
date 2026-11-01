#pragma once

#include "medicon/gui/GuiContext.hpp"

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace medicon::gui {

class SocialFonts;
class IShadowRenderer;

enum class ToastKind { Dm, Friend, News, System };
// What a click on the toast opens in the Social screen (spec 4.7).
enum class ToastTarget { Home, News, Changelog, Friends, Dm };

struct Toast {
    ToastKind kind = ToastKind::News;
    ToastTarget target = ToastTarget::Home;
    std::string title;       // sender name, Friend request, News ...
    std::string body;        // one line; ellipsized to fit
    std::int64_t sender = 0; // later DMs from the same sender merge into this toast
    std::string owner;
    std::uint64_t generation = 0;
};

struct ToastFrame {
    ImVec2 display{};
    float dt = 0.0F;
    // Menus: shown for 4 s and hovering pauses the timer. Playing: 3 s, moved
    // down by offset_y (reference px, notify.inGameOffsetY).
    bool in_menu = false;
    float offset_y = 0.0F;
    // Valid only while the client draws a cursor. Otherwise toasts have no hit area.
    PointerInput pointer{};
    // A music companion needs room left of the stack on narrow windows.
    bool music_companion = false;
};

// Notification popups at the top centre (spec 4.7). At most three are visible;
// the rest wait in a queue. Each toast appears as a circle, stretches into the
// card and then shows its content; leaving plays the same timeline backwards.
class ToastManager {
public:
    static constexpr std::size_t max_visible = 3;
    static constexpr std::size_t max_queued = 32;

    // Entering: anim runs up to timing::toast_enter. Holding: the shown timer
    // runs. Leaving: anim runs back to 0, then the toast is removed.
    enum class Phase { Entering, Holding, Leaving };
    struct Entry {
        Toast toast;
        std::uint64_t id = 0;
        int merged = 0;        // DMs folded into this toast, shown as "+N"
        Phase phase = Phase::Entering;
        float anim = 0.0F;     // seconds into the morph timeline, 0..toast_enter
        float time = 0.0F;     // seconds shown; counts only while Holding
        float slot_y = -1.0F;  // animated stack offset in px; negative until first placed
        bool dismissed = false; // clicked: hovering no longer brings it back

        // Text laid out once at the final width; redone only when it changes.
        bool relayout = true;  // the content changed since the last layout
        float layout_scale = 0.0F;
        std::uint32_t layout_revision = 0;
        float width = 0.0F;      // final width W in px
        float width_from = 0.0F; // W before a content change while shown
        float resize = 0.0F;     // seconds into that change, up to toast_resize
        std::string title_line, body_line, count;
        float count_x = 0.0F;    // "+N" offset from the start of the title
    };

    // One frame of the morph, from Entry::anim alone. Width and radius are
    // fractions of the way from the circle (H, H/2) to the card (W, R).
    struct Morph {
        float scale = 0.0F;   // about the toast centre, overshoots ~10 %
        float opacity = 0.0F;
        float stretch = 0.0F; // overshoots ~4 %
        float round = 0.0F;
        float content = 0.0F;
    };
    static Morph morph(float anim);

    void push(Toast toast);
    void remove_owner(std::string_view owner, std::uint64_t generation);
    bool empty() const noexcept { return visible_.empty() && queued_.empty(); }
    // Draws, handles hover and click, then advances the timers by frame.dt.
    void render(ImDrawList* list, const SocialFonts& fonts, IShadowRenderer& shadows, const ToastFrame& frame);
    // A toast the user clicked in a menu. It is already leaving.
    std::optional<Toast> consume_click();
    // A visible toast was drawn at `point` in the last render.
    bool covers(ImVec2 point) const noexcept;
    // First card's reserved left edge and actual animated centre Y. Valid
    // after render(), including the entry/exit animation, not just Holding.
    std::optional<ImVec2> music_anchor() const noexcept { return music_anchor_; }
    // Debug: multiplies animation time (not the shown timer), e.g. 0.25 to inspect each stage.
    void set_animation_speed(float speed) noexcept { animation_speed_ = speed; }

    // Timing only. render() passes the index under the cursor, or -1. In
    // menus the cursor pauses a shown toast and brings a leaving one back.
    void advance(float dt, bool in_menu, int hovered);
    const std::vector<Entry>& visible() const noexcept { return visible_; }
    std::size_t queued() const noexcept { return queued_.size(); }
    static float opacity(const Entry& entry) { return morph(entry.anim).opacity; }

private:
    void promote();
    std::optional<ImVec2> music_anchor_;
    void layout(Entry& entry, const SocialFonts& fonts, float scale, float min_width, float max_width);

    std::vector<Entry> visible_;
    std::deque<Entry> queued_;
    std::optional<Toast> clicked_;
    std::vector<std::pair<ImVec2, ImVec2>> hit_areas_; // last render, for covers()
    std::uint64_t next_id_ = 1;
    std::uint64_t pressed_id_ = 0;
    float animation_speed_ = 1.0F;
    bool idle_ = true; // no toast was drawn last frame: the next first frame has dt = 0
    std::string scratch_;
};

} // namespace medicon::gui
