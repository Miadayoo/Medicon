#pragma once

#include "medicon/gui/GuiContext.hpp"
#include "medicon/gui/SocialFonts.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <future>
#include <string>
#include <string_view>
#include <vector>

namespace medicon::gui {

class IShadowRenderer;

struct SocialRect {
    ImVec2 min{}, max{};
    float width() const noexcept { return max.x - min.x; }
    float height() const noexcept { return max.y - min.y; }
    bool contains(ImVec2 point) const noexcept {
        return point.x >= min.x && point.x < max.x && point.y >= min.y && point.y < max.y;
    }
};

// Clicks for the Social screen, as in the Main Menu: the left button goes down
// and comes back up over the same widget. A press already held when the screen
// opened, or when the pointer came back, never counts.
class SocialInput {
public:
    void reset() noexcept;
    // Once per frame, before any widget.
    void begin(const PointerInput& pointer) noexcept;
    // While a popup is up, the widgets under it neither hover nor click.
    void set_blocked(bool blocked) noexcept { blocked_ = blocked; }
    // After every widget: lets go of a widget whose button is up.
    void end() noexcept;
    const PointerInput& pointer() const noexcept { return pointer_; }
    bool hover(const SocialRect& rect) const noexcept { return !blocked_ && pointer_.valid && rect.contains(pointer_.position); }
    // True on the release that completes a click on `id`.
    bool click(std::uint32_t id, const SocialRect& rect) noexcept;
    // The right button came up over `rect` (context menus).
    bool right_click(const SocialRect& rect) const noexcept {
        return !blocked_ && armed_ && pointer_.valid && pointer_.buttons[1].released && rect.contains(pointer_.position);
    }
    // A press this frame anywhere (closes popups).
    bool pressed_any() const noexcept {
        return armed_ && pointer_.valid && (pointer_.buttons[0].pressed || pointer_.buttons[1].pressed);
    }
    // True while `id` holds the left button after a press inside `rect`.
    bool drag(std::uint32_t id, const SocialRect& rect) noexcept;
    bool held(std::uint32_t id) const noexcept { return active_ == id && pointer_.buttons[0].down; }
    // The left button went down this frame over no widget (unfocuses text input).
    bool pressed_elsewhere() const noexcept { return armed_ && pointer_.buttons[0].pressed && !claimed_; }

private:
    PointerInput pointer_{};
    bool armed_ = false;
    bool claimed_ = false;
    bool blocked_ = false;
    std::uint32_t active_ = 0;
};

// A wheel- and thumb-scrolled region clipped to `view`.
class SocialScroll {
public:
    // Pushes the clip rectangle; returns where the content's top edge is drawn.
    float begin(ImDrawList* list, const SocialRect& view, float content_height, const PointerInput& pointer, float dt,
                float scale);
    // Pops the clip rectangle and draws the thumb, which can be dragged.
    void end(ImDrawList* list, SocialInput& input, std::uint32_t id, float scale, float alpha);
    float offset() const noexcept { return offset_; }
    float max_offset() const noexcept { return std::max(0.0F, content_ - (view_.max.y - view_.min.y)); }
    void reset() noexcept { offset_ = target_ = 0.0F; }

private:
    SocialRect view_{};
    float content_ = 0.0F;
    float offset_ = 0.0F;
    float target_ = 0.0F;
    float grab_ = 0.0F; // pointer offset inside the thumb while dragging
};

// Single-line text editing (spec 4.8; IME composition comes with Global chat).
// Text is UTF-16 and the caret moves by grapheme cluster, so edits never split a
// surrogate pair, a combining sequence or an emoji. Keys arrive as virtual-key
// codes from Overlay::key_event and characters through ImGui's queue.
//
// Typed characters animate like a key press: they start slightly grey, dip by
// about an eighth of the font size, and rise back into their colour (180 ms,
// measured in real time). Each cluster keeps its own start time in spawn_,
// parallel to the text, so edits elsewhere never restart or move it.
class SocialTextInput {
public:
    // masked: shows one * per character (the license key).
    explicit SocialTextInput(std::size_t max_code_points, bool masked = false)
        : max_code_points_(max_code_points), masked_(masked) {}
    ~SocialTextInput();
    SocialTextInput(const SocialTextInput&) = delete;
    SocialTextInput& operator=(const SocialTextInput&) = delete;

    void set_focused(bool focused) noexcept;
    bool focused() const noexcept { return focused_; }
    void clear();
    const std::wstring& text() const noexcept { return text_; }
    std::string utf8() const;
    std::size_t length() const noexcept; // code points
    // Changes with every edit of the text (or its composition).
    std::uint32_t revision() const noexcept { return revision_; }
    // Off: new characters appear at once and drawing takes the plain path.
    void set_animation(bool enabled) noexcept { animate_ = enabled; }
    // Several lines (the Admin tab's body): Enter breaks the line instead of
    // submitting, Up/Down move between lines, a paste keeps its line breaks.
    void set_multiline(bool multiline) noexcept { multiline_ = multiline; }
    // Replaces the text without animating it.
    void set_text(std::wstring_view text);

    // Backspace, Delete, arrows, Home/End, Enter and Ctrl+A/C/V/X. Returns true when handled.
    bool key(int virtual_key, bool ctrl);
    // Printable characters typed this frame; control characters are dropped.
    void type(const ImVector<ImWchar>& characters);
    // Text the IME confirmed: inserted, and animated, like typed characters.
    void type(std::wstring_view text);
    // The IME's unconfirmed text, drawn underlined at the caret; never inserted
    // or animated. `cursor` is a UTF-16 index inside it.
    void set_composition(std::wstring text, int cursor);
    bool composing() const noexcept { return !composition_.empty(); }
    // Where the last draw put the caret (client px): the IME opens its candidates there.
    ImVec2 caret_position() const noexcept { return caret_position_; }
    float caret_height() const noexcept { return caret_height_; }
    // Finishes a paste once the clipboard was read off the render thread.
    void update();
    // Inserts pasted text: the first line, without tabs. Only the first 24
    // characters animate, each 12 ms after the one before.
    void paste(std::wstring_view text);
    // Enter was pressed since the last call.
    bool consume_submit() noexcept;

    // `now` is ImGui time (caret blink, focus glow). `glow` may be null.
    void draw(ImDrawList* list, const SocialFonts& fonts, const SocialRect& rect, float scale, float alpha,
              std::string_view placeholder, bool enabled, double now, IShadowRenderer* glow = nullptr) const;

    // --- Typing animation -------------------------------------------------------
    static constexpr double animation_seconds = 0.180;
    static constexpr double paste_stagger = 0.012; // between pasted characters
    static constexpr std::size_t paste_animated = 24;
    static constexpr std::size_t max_animating = 64;
    struct Pose {
        float dip = 0.0F;  // downward offset in px
        float blend = 1.0F; // 0 = the grey start colour, 1 = the normal colour
    };
    // The pose `elapsed` seconds after a character appeared, for `font_px` text.
    static Pose pose(double elapsed, float font_px) noexcept;
    // Real time in seconds (QueryPerformanceCounter), or the clock set for
    // deterministic captures and tests.
    static double clock() noexcept;
    static void set_clock(double (*source)()) noexcept;
    // Appearance time of the UTF-16 unit at `index`; 0 = not animating.
    double spawn_time(std::size_t index) const noexcept { return index < spawn_.size() ? spawn_[index] : 0.0; }
    // Some character is still animating at `now`.
    bool animating(double now) const noexcept { return now < animation_end_; }

private:
    enum class Source { Typed, Pasted };
    void insert(std::wstring_view text, Source source);
    void erase(std::size_t begin, std::size_t end);
    void erase_selection();
    void edited() noexcept;
    std::size_t previous(std::size_t index) const noexcept;
    std::size_t next(std::size_t index) const noexcept;
    // Units [begin, end) of text_ on one line starting at x.
    void draw_animated(ImDrawList* list, const SocialFonts& fonts, float x, float top, float fade, double now,
                       std::size_t begin, std::size_t end) const;
    void draw_multiline(ImDrawList* list, const SocialFonts& fonts, const SocialRect& rect, float scale, float fade,
                        std::string_view placeholder, bool enabled, double now) const;
    // Start of the line holding `index`, and its end (the '\n' or the end of the text).
    std::size_t line_start(std::size_t index) const noexcept;
    std::size_t line_end(std::size_t index) const noexcept;

    std::size_t max_code_points_;
    bool masked_ = false;
    bool multiline_ = false;
    mutable float scroll_ = 0.0F; // multi-line: px scrolled down to keep the caret in view
    bool animate_ = true;
    std::wstring text_;
    std::vector<double> spawn_;  // per UTF-16 unit; the same value across a cluster
    double animation_end_ = 0.0; // when the last spawned character settles
    std::size_t caret_ = 0;      // UTF-16 index, always on a cluster boundary
    bool all_selected_ = false;  // Ctrl+A selects everything; there is no partial selection
    bool focused_ = false;
    bool submit_ = false;
    double edited_at_ = 0.0;     // restarts the caret blink
    std::uint32_t revision_ = 0;
    double focused_at_ = -1.0;   // ImGui time of the last focus change, for the glow
    std::wstring composition_;
    int composition_cursor_ = 0;
    mutable ImVec2 caret_position_{};
    mutable float caret_height_ = 0.0F;
    std::future<std::wstring> paste_;
    std::future<void> copy_;
};

// End of the grapheme cluster starting at `index`: a code point with the marks,
// variation selectors, skin tones and zero-width-joined parts that follow it.
std::size_t social_cluster_end(std::wstring_view text, std::size_t index) noexcept;

enum class SocialButton { Normal, Primary, Danger };

// Hover and press of one widget, eased so glows and the press brighten and
// settle smoothly instead of switching.
struct SocialMotion {
    float hover = 0.0F;
    float press = 0.0F;
    void step(bool hovered, bool held, float dt) noexcept;
};

// The Main Menu's glass button (Glass.hpp): translucent white-blue plate, blue
// accent line (pink on hover), a pale glow that grows on hover, 0.98 when pressed.
void draw_social_button(ImDrawList* list, const SocialFonts& fonts, IShadowRenderer* glow, const SocialRect& rect,
                        std::string_view label, SocialButton style, const SocialMotion& motion, bool enabled,
                        float scale, float alpha);
// The glow a button of this size draws (for IShadowRenderer::prepare).
float social_button_glow_sigma(float scale) noexcept;
void draw_social_toggle(ImDrawList* list, const SocialRect& rect, bool on, float hover, float alpha);
// value in [0, 1].
void draw_social_slider(ImDrawList* list, const SocialRect& rect, float value, bool hovered, float scale, float alpha);
// A small gear for the settings button.
void draw_social_gear(ImDrawList* list, ImVec2 center, float radius, ImU32 color);

} // namespace medicon::gui
