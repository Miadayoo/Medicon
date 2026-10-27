#pragma once

#include "medicon/gui/GuiContext.hpp"
#include "medicon/gui/Ime.hpp"
#include "medicon/gui/SocialBackground.hpp"
#include "medicon/gui/SocialFonts.hpp"
#include "medicon/gui/SocialScreen.hpp"
#include "medicon/gui/Toasts.hpp"

#include <medicon/social/Store.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

namespace medicon::gui {

// The render-thread half of Social (spec 4.3): drains the Channel into
// SocialStore at the start of a frame and draws notifications above all other UI.
class SocialUi {
public:
    // channel: owned by the network service and outliving this object.
    // play_sound: called on the render thread; must not block.
    void attach(social::Channel* channel, std::function<void()> play_sound);
    bool attached() const noexcept { return channel_ != nullptr; }
    void open_screen(SocialView view = SocialView::Home);
    // Home with the News or Change Log tab selected.
    void open_news(social::NewsKind kind);
    // The sign-in gate stays up even when the Main Menu goes away under it.
    void close_screen() noexcept { if (!gate_) screen_.close(); }
    // The injected client's startup sign-in: after the inject intro, an account
    // without a working key gets the sign-in panel, which cannot be closed until
    // a key is accepted, and every sign-in greets with "Welcome to {name}".
    // Off in Preview scenarios unless they ask for it.
    void set_startup_sign_in(bool enabled) noexcept { startup_sign_in_ = enabled; }
    // MediconLoader downloaded this Core version this launch: after the first
    // sign-in, a one-time "Updated to {version}" notification is shown.
    void set_update_notice(std::string version) noexcept { update_version_ = std::move(version); }
    // Once per frame after the intro (Overlay::render).
    void update_sign_in_gate();
    bool sign_in_gate_active() const noexcept { return gate_; }
    bool screen_open() const noexcept { return screen_.opened(); }
    bool screen_visible() const noexcept { return screen_.visible(); }
    // Fully open: the backdrop covers everything drawn before it.
    bool screen_opaque() const noexcept { return screen_.opaque(); }
    bool screen_resources_ready(ImVec2 size) const noexcept {
        return background_.ready(size) && background_.panel_ready(SocialScreen::content_size(size)) && screen_fonts_.ready();
    }
    bool screen_fonts_ready() const noexcept { return screen_fonts_.ready(); }
    bool screen_text_ready(SocialText role, std::string_view text) const { return screen_fonts_.has(role, text); }
    void key_event(int key, bool down) { if (screen_.opened()) screen_.key_event(key, down); }
    void char_event(wchar_t character) { if (screen_.opened()) screen_.char_event(character); }
    void set_texture_host(TextureHost* host) noexcept;
    // The host's IME bridge (may be null); Social inputs use it while focused.
    void set_ime(ImeBridge* ime) noexcept { ime_ = ime; }
    // The ClickGUI "Input Animation" setting, for every Social text input.
    void set_input_animation(bool enabled) noexcept { screen_.set_input_animation(enabled); }
    // The Main Menu's time, so the gradient behind the screen continues the menu's.
    void set_backdrop_time(double seconds) noexcept { backdrop_time_ = seconds; }

    // Before the font atlas is prepared, so new glyphs are built this frame.
    // With nothing queued this is one atomic load (spec 4.9).
    void pump(FontManager& fonts, bool in_menu);
    // Last thing in the frame. Returns at once when no notification is showing
    // and nothing is prefetched. `prefetch` prepares the screen's font and
    // backdrop on their worker threads before it is first opened (menus only).
    void render(ImDrawList* list, IShadowRenderer& shadows, const FrameInfo& frame, const PointerInput& pointer,
                bool in_menu, bool prefetch = false);

    // Social font sizes stay requested once used, so later notifications reuse the glyphs.
    bool wants_fonts() const noexcept { return fonts_requested_; }
    const social::SocialStore& store() const noexcept { return store_; }
    const SocialScreen& screen() const noexcept { return screen_; }
    // The Main Menu's Social badge: unread DMs or pending friend requests (spec 4.6).
    bool badge() const noexcept { return badge_preview_ || store_.attention(); }
    SocialScreen& screen() noexcept { return screen_; }
    void preview_badge(bool shown) noexcept { badge_preview_ = shown; }
    ToastManager& toasts() noexcept { return toasts_; }

    // social.json "debugTiming": microseconds spent in pump() + render() per frame.
    struct Timing {
        std::uint32_t frames = 0;
        double total_us = 0.0;
        double max_us = 0.0;
    };
    Timing take_timing() noexcept;

private:
    void draw(ImDrawList* list, IShadowRenderer& shadows, const FrameInfo& frame, const PointerInput& pointer,
              bool in_menu, bool prefetch);
    void request_screen_glyphs();
    void request_message_glyphs(const social::ChatMessage& message);
    void sync_ime();
    void open_from_toast(const Toast& toast);
    void on_event(const social::Event& event, FontManager& fonts, bool in_menu);
    void notify(Toast toast, FontManager& fonts, bool in_menu, bool enabled);
    void add_time(std::chrono::steady_clock::time_point start) noexcept;

    social::Channel* channel_ = nullptr;
    std::function<void()> play_sound_;
    social::SocialStore store_;
    ToastManager toasts_;
    SocialScreen screen_;
    SocialBackground background_;
    SocialFonts screen_fonts_;
    TextureHost* texture_host_ = nullptr;
    bool glyphs_requested_ = false;
    bool badge_preview_ = false;
    bool startup_sign_in_ = false;
    bool gate_ = false;
    // A sign-in was already greeted; reconnects keep it, losing the account resets it.
    bool welcomed_ = false;
    std::string update_version_;  // non-empty only when the loader just updated
    bool update_notified_ = false;
    double backdrop_time_ = 0.0;
    ImeBridge* ime_ = nullptr;
    std::vector<social::Event> scratch_;
    bool fonts_requested_ = false;
    std::chrono::steady_clock::time_point last_sound_{};
    double frame_us_ = 0.0;
    Timing timing_{};
};

} // namespace medicon::gui
