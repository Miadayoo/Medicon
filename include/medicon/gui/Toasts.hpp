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
};

// Notification popups at the top centre (spec 4.7). At most three are visible;
// the rest wait in a queue.
class ToastManager {
public:
    static constexpr std::size_t max_visible = 3;
    static constexpr std::size_t max_queued = 32;

    enum class Phase { Entering, Holding, Leaving };
    struct Entry {
        Toast toast;
        std::uint64_t id = 0;
        int merged = 0;        // DMs folded into this toast, shown as "+N"
        Phase phase = Phase::Entering;
        float time = 0.0F;     // seconds in the current phase
        float slot_y = -1.0F;  // animated stack offset in px; negative until first placed
    };

    void push(Toast toast);
    bool empty() const noexcept { return visible_.empty() && queued_.empty(); }
    // Draws, handles hover and click, then advances the timers by frame.dt.
    void render(ImDrawList* list, const SocialFonts& fonts, IShadowRenderer& shadows, const ToastFrame& frame);
    // A toast the user clicked in a menu. It is already fading out.
    std::optional<Toast> consume_click();
    // A visible toast was drawn at `point` in the last render.
    bool covers(ImVec2 point) const noexcept;

    // Timing only. render() passes the index under the cursor, or -1.
    void advance(float dt, bool in_menu, int hovered);
    const std::vector<Entry>& visible() const noexcept { return visible_; }
    std::size_t queued() const noexcept { return queued_.size(); }
    static float opacity(const Entry& entry);
    // Vertical offset in reference px: from -toast_slide up to 0 while entering.
    static float slide(const Entry& entry);

private:
    void promote();

    std::vector<Entry> visible_;
    std::deque<Entry> queued_;
    std::optional<Toast> clicked_;
    std::vector<std::pair<ImVec2, ImVec2>> hit_areas_; // last render, for covers()
    std::uint64_t next_id_ = 1;
    std::uint64_t pressed_id_ = 0;
    std::string title_scratch_, body_scratch_;
};

} // namespace medicon::gui
