#pragma once

#include "medicon/gui/GuiContext.hpp"
#include "medicon/gui/GuiMath.hpp"
#include "medicon/gui/SocialChat.hpp"
#include "medicon/gui/SocialFonts.hpp"
#include "medicon/gui/SocialWidgets.hpp"

#include <medicon/social/Store.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace medicon::gui {

class IShadowRenderer;
class SocialBackground;

enum class SocialView { Home, Global, Friends, Settings, Dm };

// The Social panel (spec 4.6): sidebar, Home, Global, DM, Friends, Settings and
// the login screen, opened over the blurred backdrop. Render thread only.
class SocialScreen {
public:
    struct Context {
        ImDrawList* list = nullptr;
        SocialFonts& fonts;
        SocialBackground& background;
        IShadowRenderer& shadows;
        social::SocialStore& store;
        social::Channel* channel = nullptr;
        FrameInfo frame{};
        PointerInput pointer{};
        // The Main Menu gradient behind the panel is drawn as at this menu time,
        // so it continues the menu's own gradient seamlessly.
        double backdrop_time = 0.0;
    };

    void open(SocialView view = SocialView::Home);
    // The DM with friend `user_id`.
    void open_dm(std::int64_t user_id);
    std::int64_t dm_with() const noexcept { return dm_with_; }
    // Home with the News or Change Log tab selected, scrolled to the newest.
    void open_news(social::NewsKind kind);
    void close() noexcept;
    bool opened() const noexcept { return open_; }
    // Open, or still fading out.
    bool visible() const noexcept { return open_ || animation_.p > 0.0F; }
    // Fully open: nothing under the backdrop needs drawing.
    bool opaque() const noexcept { return open_ && animation_.p >= 1.0F; }
    SocialView view() const noexcept { return view_; }
    social::NewsKind news_tab() const noexcept { return news_kind_; }
    // Where the last frame drew a Home tab (empty before it was drawn).
    SocialRect tab_rect(social::NewsKind kind) const noexcept { return tab_rects_[static_cast<std::size_t>(kind)]; }
    // Settings rows as last drawn: DM, friend requests, news, while playing, sound.
    SocialRect toggle_rect(std::size_t index) const noexcept {
        return index < toggle_rects_.size() ? toggle_rects_[index] : SocialRect{};
    }
    SocialRect sign_out_rect() const noexcept { return sign_out_rect_; }
    SocialRect sign_in_rect() const noexcept { return sign_in_rect_; }
    // Owners only: the Admin tab of Home (posting News and Change Log).
    bool admin_tab() const noexcept { return admin_tab_; }
    void open_admin() noexcept;
    SocialTextInput& admin_title() noexcept { return admin_title_; }
    SocialTextInput& admin_version() noexcept { return admin_version_; }
    SocialTextInput& admin_body() noexcept { return admin_body_; }
    void set_admin_kind(social::NewsKind kind) noexcept { admin_kind_ = kind; }
    SocialRect admin_post_rect() const noexcept { return admin_post_rect_; }
    SocialRect confirm_ok_rect() const noexcept { return confirm_ok_rect_; }
    bool confirming_post() const noexcept { return confirm_post_; }
    const std::string& admin_status() const noexcept { return admin_status_; }
    void set_input_animation(bool enabled) noexcept { input_animation_ = enabled; }
    // From Overlay::key_event while open. IME composition is handled by the host.
    void key_event(int virtual_key, bool down);
    // WM_CHAR, in the same order as the keys; surrogate pairs come as two calls.
    void char_event(wchar_t character);
    // The text input that has the keyboard, if any (IME and typed characters go there).
    SocialTextInput* focused_input() noexcept;
    // The chat view of a conversation (created on first use).
    SocialChatView& chat(social::ConversationId conversation);
    // Rows as last drawn, for Preview and tests: a sidebar friend, and the
    // friend screen's search field.
    SocialRect friend_rect(std::int64_t user_id) const noexcept;
    SocialRect search_rect() const noexcept { return search_rect_; }
    // A Friends-screen button ("Accept", "Add", ...) in the row of `user_id`.
    SocialRect user_button_rect(std::int64_t user_id, std::string_view label) const noexcept;
    void on_event(const social::Event& event);
    void render(Context& context);

    // Fixed strings at the sizes they are drawn in; requested once so the first
    // frame of the screen already has its glyphs.
    static void request_glyphs(SocialFonts& fonts);
    // Builds the panel shadows (sign-in and full size) ahead, off the render
    // thread, so opening the screen does not stall a frame.
    static void prepare(IShadowRenderer& shadows, ImVec2 display);
    // The content area right of the sidebar, which shows back.png lightly
    // blurred (SocialBackground::update_panel), and that blur in px.
    static ImVec2 content_size(ImVec2 display);
    static float picture_blur(ImVec2 display);
    // The login screen replaces the sidebar (spec 4.6).
    bool showing_login(const social::SocialStore& store) const noexcept;
    SocialTextInput& key_field() noexcept { return key_input_; }
    const std::string& login_error() const noexcept { return login_error_; }

private:
    struct Card {
        std::vector<SocialFonts::Line> title, body;
        std::string date;
        float height = 0.0F;
    };
    struct CardCache {
        float width = -1.0F;
        std::uint32_t revision = 0;
        std::unordered_map<std::int64_t, Card> cards;
    };

    void draw_sidebar(Context& context, const SocialRect& area, float scale, float alpha);
    void draw_home(Context& context, const SocialRect& area, float scale, float alpha);
    void draw_settings(Context& context, const SocialRect& area, float scale, float alpha);
    void draw_placeholder(Context& context, const SocialRect& area, const char* title, float scale, float alpha);
    void draw_chat(Context& context, const SocialRect& area, social::ConversationId conversation, float scale);
    void draw_friends(Context& context, const SocialRect& area, float scale);
    void draw_friend_list(Context& context, const SocialRect& area, float scale, float alpha);
    void draw_confirm(Context& context, const SocialRect& panel, float scale);
    void draw_admin(Context& context, const SocialRect& area, float scale);
    // One line of a news body; Change Log lines starting "+ ", "- " or "* " get a coloured mark.
    void draw_body_line(Context& context, const social::NewsItem& item, const SocialFonts::Line& span, float x, float top,
                        float alpha) const;
    // A glass button in a list row; returns true when clicked.
    bool row_button(Context& context, std::uint32_t id, const SocialRect& rect, const char* label, SocialButton style,
                    bool enabled, float scale);
    std::string next_nonce();
    void draw_login(Context& context, const SocialRect& panel, float scale, float alpha);
    bool sidebar_item(Context& context, std::uint32_t id, const SocialRect& rect, const char* label, bool selected,
                      float scale, float alpha);
    // Eased hover and press of widget `id` (for glass plates, glows and the press).
    const SocialMotion& motion(std::uint32_t id, const SocialRect& rect, float dt);
    // A small glass icon button (plus, gear, close): returns true when clicked.
    bool icon_button(Context& context, std::uint32_t id, const SocialRect& rect, bool selected, float scale, float alpha);
    const Card& card(Context& context, const social::NewsItem& item, float width);
    void request_news(Context& context, social::NewsKind kind, std::optional<std::int64_t> before);
    void save_settings(Context& context);
    void submit_login(Context& context);

    bool open_ = false;
    Reversible animation_;
    SocialView view_ = SocialView::Home;
    SocialInput input_;
    SocialTextInput key_input_{64, true};
    bool ctrl_ = false;
    bool login_busy_ = false;
    std::string login_error_;
    bool settings_failed_ = false;
    bool slider_dragging_ = false;
    social::NewsKind news_kind_ = social::NewsKind::News;
    std::array<SocialScroll, social::news_kind_count> news_scroll_{};
    std::array<CardCache, social::news_kind_count> cards_{};
    std::array<bool, social::news_kind_count> news_failed_{};
    std::array<SocialRect, social::news_kind_count> tab_rects_{};
    std::array<SocialRect, 5> toggle_rects_{};
    SocialRect sign_out_rect_{};
    SocialRect sign_in_rect_{};
    SocialScroll friends_scroll_;
    std::unordered_map<std::uint32_t, SocialMotion> motion_;
    bool input_animation_ = true;
    wchar_t high_surrogate_ = 0;
    // The focused input's text whose glyphs were last requested.
    const SocialTextInput* glyphs_for_ = nullptr;
    std::uint32_t glyphs_revision_ = 0;
    // Chat (phases 4-6).
    std::int64_t dm_with_ = 0;
    std::map<social::ConversationId, std::unique_ptr<SocialChatView>> chats_;
    std::vector<std::pair<std::int64_t, SocialRect>> friend_rects_;
    // Friends screen: the search field, searched 300 ms after typing stops (spec 4.6).
    SocialTextInput search_input_{16};
    std::string searched_;
    double search_due_ = -1.0;
    SocialRect search_rect_{};
    struct UserButton {
        std::int64_t user_id;
        std::string label;
        SocialRect rect;
    };
    std::vector<UserButton> user_buttons_;
    SocialScroll friends_view_scroll_;
    std::string friend_error_;
    double friend_error_until_ = 0.0;
    std::uint64_t next_nonce_ = 1;
    // Right-click menu on a friend, then its confirmation (spec 4.6).
    std::int64_t menu_for_ = 0;
    ImVec2 menu_at_{};
    std::int64_t confirm_remove_ = 0;
    // Admin tab (owners): the form, then "Post … to everyone?" before it is sent.
    bool admin_tab_ = false;
    social::NewsKind admin_kind_ = social::NewsKind::Changelog;
    SocialTextInput admin_title_{120};
    SocialTextInput admin_version_{32};
    SocialTextInput admin_body_{4000};
    bool confirm_post_ = false;
    bool posting_ = false;
    std::string admin_status_;
    bool admin_status_ok_ = false;
    double admin_status_until_ = 0.0;
    SocialRect admin_post_rect_{};
    std::array<std::uint32_t, 3> admin_revisions_{~0u, ~0u, ~0u};
    SocialRect confirm_ok_rect_{};
    std::string search_typed_;
    bool refresh_friends_ = false;
    // The view last drawn, to give a chat field the keyboard when it is entered.
    SocialView shown_view_ = SocialView::Home;
    std::int64_t shown_dm_ = 0;
};

} // namespace medicon::gui
