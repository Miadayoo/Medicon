#include <medicon/gui/SocialScreen.hpp>

#include <medicon/gui/Draw.hpp>
#include <medicon/gui/Glass.hpp>
#include <medicon/gui/MainMenu.hpp>
#include <medicon/gui/Shadow.hpp>
#include <medicon/gui/SocialBackground.hpp>
#include <medicon/gui/Theme.hpp>

#include <Windows.h>

#include <cmath>
#include <cstdlib>
#include <cctype>
#include <cstdio>
#include <ctime>

namespace medicon::gui {
namespace {

namespace style = theme::social;
namespace metrics = theme::social::metrics;
using social::ConnectionState;
using social::NewsKind;

enum : std::uint32_t {
    id_close = 1,
    id_home,
    id_global,
    id_friends,
    id_settings,
    id_reconnect,
    id_tab_news,
    id_tab_changelog,
    id_news_retry,
    id_news_scroll,
    id_friends_scroll,
    id_key_field,
    id_login,
    id_logout,
    id_offset,
    id_toggle_first = 100,
    // Fixed ids below 3000; rows count up from their own bases.
    id_menu_remove = 2900,
    id_confirm_ok,
    id_confirm_cancel,
    id_search_field,
    id_tab_admin,
    id_admin_news,
    id_admin_changelog,
    id_admin_title,
    id_admin_version,
    id_admin_body,
    id_admin_post,
    id_admin_dll,
    id_admin_dll_path,
    id_post_ok,
    id_post_cancel,
    id_fixed_end,
    id_friend_first = 3000,  // + index: sidebar friends
    id_friends_view_first = 5000, // + index * 4 + button: the Friends screen
};

static_assert(id_fixed_end <= id_friend_first, "fixed widget ids must not reach the friend rows");

const char* friend_error_text(std::string_view error) {
    if (error == "user_not_found") return "No user with that name";
    if (error == "self") return "That's you";
    if (error == "already_friends") return "You're already friends";
    if (error == "request_exists") return "Request already sent";
    if (error == "rate_limited") return "Too many requests. Try again later";
    if (error == "offline") return "Not connected";
    if (error == "not_friends") return "You're not friends";
    return "Something went wrong";
}

const char* chat_error_text(std::string_view error, bool global) {
    if (error == "rate_limited") return global ? "One message every 0.3 seconds" : "Sending too fast";
    if (error == "too_long") return "Messages are up to 300 characters";
    if (error == "not_friends") return "You can only message friends";
    if (error == "offline") return "Not connected";
    if (error == "empty") return "";
    return "Could not send";
}

const char* login_message(std::string_view error) {
    if (error == "invalid_key") return "That license key is not valid";
    if (error == "revoked") return "This license key has been revoked";
    if (error == "banned") return "This account is suspended";
    if (error == "unauthorized") return "Could not sign in. Check your key";
    if (error == "logout_failed") return "Could not remove the saved license";
    if (error == "credential_save_failed") return "Could not save the license";
    if (error == "rate_limited") return "Too many attempts. Try again later";
    if (error == "bad_server_url") return "This client's server URL is invalid";
    return "Cannot reach the server";
}

constexpr const char* tab_labels[]{"News", "Change Log"};
constexpr const char* empty_labels[]{"No news yet", "No change log yet"};
constexpr float max_offset = 400.0F; // settings slider range for notify.inGameOffsetY

// The open/close animation scales finished geometry around the panel centre
// (spec 4.6: 0.96 -> 1) and fades it, instead of laying the panel out again at
// another size, which would also need fonts rasterised at that size.
void scale_from(ImDrawList* list, int first_vertex, int first_command, ImVec2 center, float scale, float alpha) {
    for (int i = first_vertex; i < list->VtxBuffer.Size; ++i) {
        auto& vertex = list->VtxBuffer[i];
        vertex.pos.x = center.x + (vertex.pos.x - center.x) * scale;
        vertex.pos.y = center.y + (vertex.pos.y - center.y) * scale;
        const auto a = static_cast<float>((vertex.col >> IM_COL32_A_SHIFT) & 0xFFu);
        vertex.col = (vertex.col & ~IM_COL32_A_MASK) |
                     (static_cast<ImU32>(std::lround(a * alpha)) << IM_COL32_A_SHIFT);
    }
    for (int i = first_command; i < list->CmdBuffer.Size; ++i) {
        auto& clip = list->CmdBuffer[i].ClipRect;
        clip.x = center.x + (clip.x - center.x) * scale;
        clip.y = center.y + (clip.y - center.y) * scale;
        clip.z = center.x + (clip.z - center.x) * scale;
        clip.w = center.y + (clip.w - center.y) * scale;
    }
}

std::string format_date(std::int64_t milliseconds) {
    const std::time_t seconds = static_cast<std::time_t>(milliseconds / 1000);
    std::tm local{};
    if (localtime_s(&local, &seconds) != 0) return {};
    char text[16];
    std::snprintf(text, sizeof text, "%04d/%02d/%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    return text;
}

std::size_t slot(NewsKind kind) { return static_cast<std::size_t>(kind); }

// Social sizes scale with the window like the rest of the client: height / 1080,
// or less in a window narrower than 16:9.
float layout_scale(ImVec2 display) { return std::min(display.y / metrics::reference_height, display.x / 1920.0F); }

ImVec2 panel_size(ImVec2 display, bool login) {
    const float s = layout_scale(display);
    return {std::round(std::min((login ? metrics::login_panel_width : metrics::panel_width) * s,
                                display.x * metrics::panel_max_ratio)),
            std::round(std::min((login ? metrics::login_panel_height : metrics::panel_height) * s,
                                display.y * metrics::panel_max_ratio))};
}

// A wide, soft blue shadow (never black) and a faint pale-blue glow around the panel.
ShadowParams panel_shadow(float s) {
    return {style::panel_shadow.opacity, style::panel_shadow.blur * 0.5F * s, 0.0F, style::panel_shadow.offset_y * s,
            to_u32(style::shadow_tint)};
}

ShadowParams panel_glow(float s) {
    return {style::panel_glow_opacity, style::panel_glow_blur * 0.5F * s, 0.0F, 0.0F, to_u32(style::panel_glow)};
}

float sidebar_width(float panel_width, float s) { return std::round(std::min(panel_width * 0.4F, metrics::sidebar_width * s)); }

// The sign-in form's width inside its card.
float login_form_width(ImVec2 display) {
    const float s = layout_scale(display);
    return std::min(metrics::login_width * s, panel_size(display, true).x - metrics::login_padding * s * 2.0F);
}

// "Medicon Social": the Main Menu's rounded face in pastel blue with a white rim,
// the way the Main Menu logo carries a rim of its own.
void draw_title(ImDrawList* list, const SocialFonts& fonts, const SocialRect& rect, float s, float alpha, std::string_view text) {
    const float x = std::round(rect.min.x + (rect.width() - fonts.measure(SocialText::Display, text)) * 0.5F);
    const float y = std::round(rect.min.y + (rect.height() - fonts.line_height(SocialText::Display)) * 0.5F);
    const float rim = std::max(1.0F, metrics::title_outline * s);
    for (int i = 0; i < 8; ++i) {
        const float angle = 6.28318530718F * static_cast<float>(i) / 8.0F;
        fonts.draw(list, SocialText::Display, x + std::cos(angle) * rim, y + std::sin(angle) * rim,
                   to_u32(style::title_outline, alpha), text);
    }
    fonts.draw(list, SocialText::Display, x, y, to_u32(style::title, alpha), text);
}

void draw_centered(ImDrawList* list, const SocialFonts& fonts, SocialText role, const SocialRect& rect, ImU32 color,
                   std::string_view text) {
    fonts.draw(list, role, rect.min.x + (rect.width() - fonts.measure(role, text)) * 0.5F,
               rect.min.y + (rect.height() - fonts.line_height(role)) * 0.5F, color, text);
}

} // namespace

void SocialScreen::request_glyphs(SocialFonts& fonts) {
    fonts.request(SocialText::Display, "Medicon Social");
    for (const char* text : {"Settings", "Global Chat", "Friends"})
        fonts.request(SocialText::Heading, text);
    for (const char* text : {"News", "Change Log", "Sign in", "Signing in…", "Sign out", "Medicon", "Global"})
        fonts.request(SocialText::Title, text);
    for (const char* text : {"Enter your license key to sign in", "Direct messages", "Friend requests", "News",
                             "Show while playing", "Sound", "Position while playing", "Signed in as ", "Coming soon",
                             "No news yet", "No change log yet", "Loading…", "Could not load",
                             "Settings could not be saved (social.json is invalid)", "MDC-XXXX-XXXX-XXXX-XXXX*"})
        fonts.request(SocialText::Body, text);
    for (const auto* error : {"invalid_key", "revoked", "banned", "unauthorized", "logout_failed", "credential_save_failed", "rate_limited", "bad_server_url", ""})
        fonts.request(SocialText::Body, login_message(error));
    for (const char* text : {"Search by name", "Find friends by their name", "No users found", "Remove friend", "Remove",
                             "Cancel", "You're no longer friends", "Coming soon"})
        fonts.request(SocialText::Body, text);
    for (const char* text : {"Add", "Requested", "Message", "Accept", "Decline", "Cancel", "Remove", "Friends"})
        fonts.request(SocialText::Title, text);
    for (const auto* error : {"user_not_found", "self", "already_friends", "request_exists", "rate_limited", "offline", ""})
        fonts.request(SocialText::Small, friend_error_text(error));
    SocialChatView::request_glyphs(fonts);
    for (const char* text : {"Admin", "Post", "Post this News?", "Post this Change Log?"}) fonts.request(SocialText::Title, text);
    for (const char* text : {"Post this News?", "Post this Change Log?"}) fonts.request(SocialText::Heading, text);
    for (const char* text : {"Title", "Version (optional)", "Write the post here", "+ Added Killaura", "- Removed something",
                             "* Fixed something", "Everyone online sees it right away.", "It can't be edited or deleted later.",
                             "Nothing to preview yet"})
        fonts.request(SocialText::Body, text);
    for (const char* text : {"TITLE", "VERSION", "TEXT", "PREVIEW", "OWNER", "+ added   - removed   * fixed", "Posted",
                             "Posting\xE2\x80\xA6", "Only owners can post", "A title and text are required", "Not connected",
                             "Could not post"})
        fonts.request(SocialText::Small, text);
    for (const char* text : {"DM", "NOTIFICATIONS", "ACCOUNT", "No friends yet", "SEARCH", "REQUESTS", "SENT", "99+", "Online", "Connecting…", "Reconnecting…",
                             "Signed in elsewhere", "Offline", "Reconnect", "Retry", "+0123456789 px",
                             "Keys start with MDC- and are issued by an admin"})
        fonts.request(SocialText::Small, text);
}

void SocialScreen::prepare(IShadowRenderer& shadows, ImVec2 display) {
    const float s = layout_scale(display);
    const auto panel_radii = CornerRadii::all(metrics::panel_radius * s);
    for (const bool login : {false, true}) {
        shadows.prepare(panel_size(display, login), panel_radii, panel_shadow(s));
        shadows.prepare(panel_size(display, login), panel_radii, panel_glow(s));
    }
    // The sign-in field's focus glow and the button's hover glow.
    const ShadowParams glow{1.0F, social_button_glow_sigma(s), 0.0F, 0.0F, to_u32(style::glow)};
    shadows.prepare({login_form_width(display), metrics::input_height * s}, CornerRadii::all(metrics::input_radius * s), glow);
    shadows.prepare({login_form_width(display), metrics::button_height * s}, CornerRadii::all(metrics::button_radius * s), glow);
}

ImVec2 SocialScreen::content_size(ImVec2 display) {
    const ImVec2 panel = panel_size(display, false);
    return {panel.x - sidebar_width(panel.x, layout_scale(display)), panel.y};
}

float SocialScreen::picture_blur(ImVec2 display) { return metrics::panel_picture_blur * layout_scale(display); }

void SocialScreen::open(SocialView view) {
    if (!open_) {
        input_.reset();
        ctrl_ = false;
    }
    open_ = true;
    view_ = view;
}

void SocialScreen::open_dm(std::int64_t user_id) {
    open(SocialView::Dm);
    dm_with_ = user_id;
}

void SocialScreen::open_admin() noexcept {
    open(SocialView::Home);
    admin_tab_ = true;
}

SocialChatView& SocialScreen::chat(social::ConversationId conversation) {
    auto& view = chats_[conversation];
    if (!view) view = std::make_unique<SocialChatView>(conversation);
    return *view;
}

SocialTextInput* SocialScreen::focused_input() noexcept {
    if (key_input_.focused()) return &key_input_;
    if (view_ == SocialView::Friends && search_input_.focused()) return &search_input_;
    if (view_ == SocialView::Home && admin_tab_) {
        for (auto* input : {&admin_title_, &admin_version_, &admin_body_})
            if (input->focused()) return input;
    }
    if (view_ == SocialView::Global || view_ == SocialView::Dm) {
        const auto found = chats_.find(view_ == SocialView::Global ? social::global_conversation : dm_with_);
        if (found != chats_.end() && found->second->field().focused()) return &found->second->field();
    }
    return nullptr;
}

SocialRect SocialScreen::user_button_rect(std::int64_t user_id, std::string_view label) const noexcept {
    for (const auto& button : user_buttons_)
        if (button.user_id == user_id && button.label == label) return button.rect;
    return {};
}

SocialRect SocialScreen::friend_rect(std::int64_t user_id) const noexcept {
    for (const auto& [id, rect] : friend_rects_)
        if (id == user_id) return rect;
    return {};
}

std::string SocialScreen::next_nonce() { return "f" + std::to_string(next_nonce_++); }

void SocialScreen::open_news(NewsKind kind) {
    open(SocialView::Home);
    news_kind_ = kind;
    news_scroll_[slot(kind)].reset();
}

void SocialScreen::close() noexcept {
    open_ = false;
    if (auto* input = focused_input()) input->set_focused(false);
    slider_dragging_ = false;
    menu_for_ = 0;
    confirm_remove_ = 0;
}

bool SocialScreen::showing_login(const social::SocialStore& store) const noexcept {
    return login_busy_ || store.state() == ConnectionState::NoLicense || store.state() == ConnectionState::AuthFailed;
}

void SocialScreen::key_event(int virtual_key, bool down) {
    if (virtual_key == VK_CONTROL || virtual_key == VK_LCONTROL || virtual_key == VK_RCONTROL) {
        ctrl_ = down;
        return;
    }
    if (!down || !open_) return;
    if (virtual_key == VK_ESCAPE) {
        // AutoOverlay consumes Esc on the window thread while IME composition is active.
        // An open menu or confirmation closes first.
        if (menu_for_ != 0 || confirm_remove_ != 0 || confirm_post_) {
            menu_for_ = confirm_remove_ = 0;
            confirm_post_ = false;
            return;
        }
        if (!locked_) close();
        return;
    }
    if (auto* input = focused_input()) input->key(virtual_key, ctrl_);
}

void SocialScreen::char_event(wchar_t character) {
    if (!open_) return;
    // A character outside the BMP arrives as two WM_CHARs; join them first.
    if (character >= 0xD800 && character <= 0xDBFF) {
        high_surrogate_ = character;
        return;
    }
    wchar_t text[2]{character, 0};
    std::wstring_view typed(text, 1);
    if (character >= 0xDC00 && character <= 0xDFFF) {
        if (!high_surrogate_) return;
        text[0] = std::exchange(high_surrogate_, wchar_t{0});
        text[1] = character;
        typed = std::wstring_view(text, 2);
    }
    high_surrogate_ = 0;
    if (auto* input = focused_input()) input->type(typed);
}

void SocialScreen::on_event(const social::Event& event) {
    if (const auto* login = std::get_if<social::LoginFinished>(&event)) {
        login_busy_ = false;
        if (login->ok) {
            login_error_.clear();
            key_input_.clear();
            key_input_.set_focused(false);
            view_ = SocialView::Home;
        } else {
            login_error_ = login->error;
        }
    } else if (std::holds_alternative<social::SettingsSaveFailed>(event)) {
        settings_failed_ = true;
    } else if (const auto* page = std::get_if<social::NewsPage>(&event)) {
        news_failed_[slot(page->kind)] = page->failed;
    } else if (const auto* logout = std::get_if<social::LogoutFinished>(&event)) {
        login_busy_ = false;
        key_input_.clear();
        key_input_.set_focused(false);
        login_error_ = logout->credential_removed ? "" : "logout_failed";
    } else if (const auto* state = std::get_if<social::StateChanged>(&event)) {
        // A key revoked or banned while connected is explained on the login screen.
        if (!login_busy_ && state->state == ConnectionState::AuthFailed) login_error_ = state->reason;
        if (!login_busy_ && state->state == ConnectionState::NoLicense && login_error_ != "logout_failed") login_error_.clear();
        if (state->state == ConnectionState::NoLicense || state->state == ConnectionState::AuthFailed) {
            // Another account may sign in next: nothing of this one stays.
            chats_.clear();
            search_input_.clear();
            searched_.clear();
            dm_with_ = 0;
            menu_for_ = confirm_remove_ = 0;
            if (view_ == SocialView::Dm || view_ == SocialView::Friends) view_ = SocialView::Home;
        }
    } else if (const auto* done = std::get_if<social::RequestFinished>(&event)) {
        const double now = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
        const auto& nonce = done->nonce;
        if (!nonce.empty() && nonce.front() == 'a') {
            // The Admin tab's post.
            posting_ = false;
            admin_status_ok_ = done->ok;
            admin_status_until_ = now + 4.0;
            if (done->ok) {
                admin_status_ = admin_attach_dll_ ? "Published" : "Posted";
                admin_title_.clear();
                admin_version_.clear();
                admin_body_.clear();
                admin_dll_path_.clear();
            } else {
                admin_status_ = done->error == "forbidden"      ? "Only owners can post"
                              : done->error == "bad_request"    ? "A title and text are required"
                              : done->error == "offline"        ? "Not connected"
                              : done->error == "dll_unreadable" ? "Cannot read that DLL file"
                              : done->error == "version_conflict" ? "That version is already published"
                              : done->error == "bad_release"    ? "That file is not a valid x64 DLL"
                              : done->error == "releases_disabled" ? "The server has releases turned off"
                                                                   : "Could not post";
            }
        } else if (!nonce.empty() && nonce.front() == 'f') {
            // Friend requests: lists come back from the server either way.
            refresh_friends_ = true;
            if (done->ok) searched_.clear(); // search rows show the new relation
            else {
                friend_error_ = friend_error_text(done->error);
                friend_error_until_ = now + 3.0;
            }
        } else if (!done->ok && !nonce.empty() && (nonce.front() == 'g' || nonce.front() == 'd')) {
            // "g0-3" / "d42-3": the conversation is between the letter and the dash.
            const auto conversation = static_cast<social::ConversationId>(std::strtoll(nonce.c_str() + 1, nullptr, 10));
            const auto found = chats_.find(conversation);
            const char* text = chat_error_text(done->error, conversation == social::global_conversation);
            if (found != chats_.end() && *text) found->second->show_error(text, now);
        }
    } else if (const auto* update = std::get_if<social::FriendUpdated>(&event)) {
        if (update->status == social::FriendStatus::Removed && view_ == SocialView::Dm && dm_with_ == update->user.id)
            view_ = SocialView::Friends;
    }
}

void SocialScreen::request_news(Context& context, NewsKind kind, std::optional<std::int64_t> before) {
    if (!context.channel) return;
    if (context.store.loading(kind)) return;
    news_failed_[slot(kind)] = false;
    social::FetchNews request;
    request.kind = kind;
    request.before = before;
    context.channel->api.push(request);
    context.store.set_loading(kind, true);
}

void SocialScreen::save_settings(Context& context) {
    settings_failed_ = false;
    if (context.channel) context.channel->commands.push(social::SaveSettings{context.store.notify()});
}

void SocialScreen::submit_login(Context& context) {
    if (login_busy_ || key_input_.length() == 0 || !context.channel) return;
    const std::string key = key_input_.utf8();
    bool valid = key.size() == 23 && key.substr(0, 4) == "MDC-";
    for (std::size_t i = 4; valid && i < key.size(); ++i) {
        const bool separator = i == 8 || i == 13 || i == 18;
        valid = separator ? key[i] == '-' : std::isalnum(static_cast<unsigned char>(key[i])) != 0;
    }
    if (!valid) {
        login_error_ = "invalid_key";
        return;
    }
    login_busy_ = true;
    login_error_.clear();
    context.channel->commands.push(social::Login{key});
}

const SocialScreen::Card& SocialScreen::card(Context& context, const social::NewsItem& item, float width) {
    const float s = layout_scale({context.frame.client_w, context.frame.client_h});
    auto& cache = cards_[slot(item.kind)];
    // Line breaks depend on the width and on the glyph advances of the atlas in use.
    if (cache.width != width || cache.revision != context.fonts.revision()) {
        cache.cards.clear();
        cache.width = width;
        cache.revision = context.fonts.revision();
    }
    auto [found, inserted] = cache.cards.try_emplace(item.id);
    if (inserted) {
        Card& layout = found->second;
        const float inner = width - metrics::card_padding * s * 2.0F;
        context.fonts.wrap(SocialText::Title, item.title, inner, layout.title);
        context.fonts.wrap(SocialText::Body, item.body, inner, layout.body);
        layout.date = format_date(item.created_at);
        const auto& fonts = context.fonts;
        layout.height = metrics::card_padding * s * 2.0F +
                        static_cast<float>(layout.title.size()) * fonts.line_height(SocialText::Title) +
                        metrics::text_gap * s + fonts.line_height(SocialText::Small);
        if (!item.body.empty())
            layout.height += metrics::text_gap * s * 1.5F + static_cast<float>(layout.body.size()) * fonts.line_height(SocialText::Body);
    }
    return found->second;
}

const SocialMotion& SocialScreen::motion(std::uint32_t id, const SocialRect& rect, float dt) {
    auto& state = motion_[id];
    state.step(input_.hover(rect), input_.held(id), dt);
    return state;
}

bool SocialScreen::icon_button(Context& context, std::uint32_t id, const SocialRect& rect, bool selected, float s, float alpha) {
    const auto& m = motion(id, rect, context.frame.dt);
    const float shown = std::max(selected ? 1.0F : 0.0F, m.hover);
    if (shown > 0.0F) {
        GlassPlate plate;
        plate.hover = m.hover;
        plate.press = m.press;
        plate.accent = false;
        plate.drop = false;
        plate.fill = style::plate;
        plate.alpha = alpha * shown;
        draw_glass_plate(context.list, rect.min, rect.max, metrics::item_radius * s * 0.8F, plate);
    }
    return input_.click(id, rect);
}

bool SocialScreen::sidebar_item(Context& context, std::uint32_t id, const SocialRect& rect, const char* label,
                                bool selected, float s, float alpha) {
    auto* list = context.list;
    const auto& m = motion(id, rect, context.frame.dt);
    // Main Menu plates: the selected one with its accent line, the others only while hovered.
    const float shown = selected ? 1.0F : m.hover;
    float inset = rect.height() * metrics::accent_inset;
    if (shown > 0.0F) {
        GlassPlate plate;
        plate.hover = m.hover;
        plate.press = m.press;
        plate.accent = selected;
        plate.accent_inset = rect.height() * metrics::accent_inset;
        plate.drop = selected;
        plate.fill = style::plate;
        plate.alpha = alpha * shown;
        inset = draw_glass_plate(list, rect.min, rect.max, metrics::item_radius * s, plate);
    }
    context.fonts.draw(list, SocialText::Title, rect.min.x + inset * 1.55F,
                       rect.min.y + (rect.height() - context.fonts.line_height(SocialText::Title)) * 0.5F,
                       to_u32(selected ? style::heading : style::text, alpha), label);
    return input_.click(id, rect);
}

void SocialScreen::draw_sidebar(Context& context, const SocialRect& area, float s, float alpha) {
    auto* list = context.list;
    const auto& fonts = context.fonts;
    const float radius = metrics::panel_radius * s;
    const float line = std::max(1.0F, s);
    list->AddRectFilled(area.min, area.max, to_u32(style::sidebar_fill, alpha), radius, ImDrawFlags_RoundCornersLeft);
    list->AddLine({area.max.x, area.min.y + radius}, {area.max.x, area.max.y - radius}, to_u32(style::divider, alpha), line);
    const float pad = metrics::sidebar_padding * s;
    const float item = metrics::sidebar_item_height * s;
    const float x0 = area.min.x + pad, x1 = area.max.x - pad;
    float y = area.min.y + pad;
    if (sidebar_item(context, id_home, {{x0, y}, {x1, y + item}}, "Medicon", view_ == SocialView::Home, s, alpha))
        view_ = SocialView::Home;
    y += item + 6.0F * s;
    if (sidebar_item(context, id_global, {{x0, y}, {x1, y + item}}, "Global", view_ == SocialView::Global, s, alpha))
        view_ = SocialView::Global;
    y += item + 8.0F * s;
    list->AddLine({x0, y}, {x1, y}, to_u32(style::divider, alpha), line);
    y += 6.0F * s;

    // "DM" with the add-friend button on the right (spec 4.6). Its badge for
    // pending requests arrives with friends (phase 5).
    const float header = 24.0F * s;
    fonts.draw(list, SocialText::Small, x0 + 6.0F * s, y + (header - fonts.line_height(SocialText::Small)) * 0.5F,
               to_u32(style::heading, alpha), "DM");
    const SocialRect plus{{x1 - header, y}, {x1, y + header}};
    const bool plus_clicked = icon_button(context, id_friends, plus, view_ == SocialView::Friends, s, alpha);
    const ImVec2 c{(plus.min.x + plus.max.x) * 0.5F, (plus.min.y + plus.max.y) * 0.5F};
    const float arm = header * 0.22F, half = std::max(0.75F, 1.1F * s);
    list->AddRectFilled({c.x - arm, c.y - half}, {c.x + arm, c.y + half}, to_u32(style::icon, alpha), half);
    list->AddRectFilled({c.x - half, c.y - arm}, {c.x + half, c.y + arm}, to_u32(style::icon, alpha), half);
    if (plus_clicked) view_ = SocialView::Friends;
    if (!context.store.incoming().empty()) {
        // Spec 4.6: a badge on + while requests wait.
        const float dot = 3.5F * s;
        list->AddCircleFilled({plus.max.x - dot, plus.min.y + dot}, dot + std::max(1.0F, s), to_u32(theme::white, alpha), 16);
        list->AddCircleFilled({plus.max.x - dot, plus.min.y + dot}, dot, to_u32(style::unread_badge, alpha), 16);
    }
    y += header + 2.0F * s;

    // The friend list (phase 5) takes the remaining height and scrolls.
    const float footer = metrics::sidebar_footer_height * s;
    const SocialRect friends{{area.min.x, y}, {area.max.x, area.max.y - footer}};
    draw_friend_list(context, friends, s, alpha);

    // Me, the connection state (spec 4.4) and the settings gear.
    const float footer_top = area.max.y - footer;
    list->AddLine({x0, footer_top}, {x1, footer_top}, to_u32(style::divider, alpha), line);
    const auto& me = context.store.me();
    const float name_top = footer_top + (footer - fonts.line_height(SocialText::Title) - fonts.line_height(SocialText::Small) - 2.0F * s) * 0.5F;
    std::string name_scratch;
    const float gear_size = 30.0F * s;
    const auto name = fonts.ellipsize(SocialText::Title, me ? std::string_view(me->name) : std::string_view("-"),
                                      x1 - x0 - gear_size - 12.0F * s, name_scratch);
    fonts.draw(list, SocialText::Title, x0 + 6.0F * s, name_top, to_u32(style::heading, alpha), name);
    const char* status = "Offline";
    const Color* dot = &style::offline;
    switch (context.store.state()) {
    case ConnectionState::Online: status = "Online"; dot = &style::online; break;
    case ConnectionState::Authenticating:
    case ConnectionState::Connecting: status = "Connecting…"; dot = &style::connecting; break;
    case ConnectionState::Reconnecting: status = "Reconnecting…"; dot = &style::connecting; break;
    case ConnectionState::Replaced: status = "Signed in elsewhere"; break;
    default: break;
    }
    const float status_top = name_top + fonts.line_height(SocialText::Title) + 2.0F * s;
    const float small = fonts.line_height(SocialText::Small);
    const float dot_radius = metrics::status_dot * s * 0.5F;
    list->AddCircleFilled({x0 + 6.0F * s + dot_radius, status_top + small * 0.5F}, dot_radius, to_u32(*dot, alpha), 16);
    const float status_x = x0 + 6.0F * s + dot_radius * 2.0F + 6.0F * s;
    fonts.draw(list, SocialText::Small, status_x, status_top, to_u32(style::text_secondary, alpha), status);
    if (me && me->rank == social::Rank::Owner) {
        // The rank, as a small pink tag after the connection state.
        const float tag_x = status_x + fonts.measure(SocialText::Small, status) + 8.0F * s;
        const float tag_w = fonts.measure(SocialText::Small, "OWNER") + 10.0F * s;
        if (tag_x + tag_w < x1 - gear_size) {
            list->AddRectFilled({tag_x, status_top - 1.0F * s}, {tag_x + tag_w, status_top + small + 1.0F * s},
                                to_u32(style::owner_tag, alpha), (small + 2.0F * s) * 0.5F);
            fonts.draw(list, SocialText::Small, tag_x + 5.0F * s, status_top, to_u32(style::owner_text, alpha), "OWNER");
        }
    }
    if (context.store.state() == ConnectionState::Replaced) {
        // 4009 does not reconnect by itself (spec 2.6); this is how to come back.
        const float x = status_x + fonts.measure(SocialText::Small, status) + 8.0F * s;
        const SocialRect link{{x, status_top}, {x + fonts.measure(SocialText::Small, "Reconnect"), status_top + small}};
        fonts.draw(list, SocialText::Small, link.min.x, link.min.y, to_u32(style::blue, alpha), "Reconnect");
        if (input_.hover(link))
            list->AddLine({link.min.x, link.max.y}, {link.max.x, link.max.y}, to_u32(style::blue, alpha), line);
        if (input_.click(id_reconnect, link) && context.channel) context.channel->commands.push(social::Reconnect{});
    }
    const SocialRect gear{{x1 - gear_size, footer_top + (footer - gear_size) * 0.5F},
                          {x1, footer_top + (footer + gear_size) * 0.5F}};
    const bool gear_clicked = icon_button(context, id_settings, gear, view_ == SocialView::Settings, s, alpha);
    draw_social_gear(list, {(gear.min.x + gear.max.x) * 0.5F, (gear.min.y + gear.max.y) * 0.5F}, gear_size * 0.32F,
                     to_u32(style::icon, alpha));
    if (gear_clicked) view_ = SocialView::Settings;
}

void SocialScreen::draw_home(Context& context, const SocialRect& area, float s, float alpha) {
    auto* list = context.list;
    const auto& fonts = context.fonts;
    auto& store = context.store;
    const float pad = metrics::content_padding * s;
    const float tab_height = metrics::tab_height * s;
    float x = area.min.x + pad;
    const float y = area.min.y + pad;
    for (std::size_t i = 0; i < social::news_kind_count; ++i) {
        const auto kind = static_cast<NewsKind>(i);
        const float width = fonts.measure(SocialText::Title, tab_labels[i]) + metrics::tab_padding * s * 2.0F;
        const SocialRect tab{{x, y}, {x + width, y + tab_height}};
        tab_rects_[i] = tab;
        const bool selected = news_kind_ == kind && !admin_tab_;
        const std::uint32_t id = i == 0 ? id_tab_news : id_tab_changelog;
        const auto& m = motion(id, tab, context.frame.dt);
        // A glass pill for the selected tab; the other shows one while hovered.
        const float shown = selected ? 1.0F : m.hover;
        if (shown > 0.0F) {
            GlassPlate plate;
            plate.hover = m.hover;
            plate.press = m.press;
            plate.accent = false;
            plate.drop = selected;
            plate.fill = style::plate;
            plate.alpha = alpha * shown;
            draw_glass_plate(list, tab.min, tab.max, tab_height * 0.5F, plate);
        }
        draw_centered(list, fonts, SocialText::Title, tab, to_u32(selected ? style::heading : style::text_secondary, alpha),
                      tab_labels[i]);
        if (input_.click(id, tab)) {
            news_kind_ = kind;
            admin_tab_ = false;
        }
        x += width + 8.0F * s;
    }
    // Owners get a third tab to post News and Change Log.
    const bool owner = store.me() && store.me()->rank == social::Rank::Owner;
    if (!owner) admin_tab_ = false;
    if (owner) {
        const float width = fonts.measure(SocialText::Title, "Admin") + metrics::tab_padding * s * 2.0F;
        const SocialRect tab{{x, y}, {x + width, y + tab_height}};
        const auto& m = motion(id_tab_admin, tab, context.frame.dt);
        const float shown = admin_tab_ ? 1.0F : m.hover;
        if (shown > 0.0F) {
            GlassPlate plate;
            plate.hover = m.hover;
            plate.press = m.press;
            plate.accent = false;
            plate.drop = admin_tab_;
            plate.fill = style::plate;
            plate.alpha = alpha * shown;
            draw_glass_plate(list, tab.min, tab.max, tab_height * 0.5F, plate);
        }
        draw_centered(list, fonts, SocialText::Title, tab, to_u32(admin_tab_ ? style::heading : style::text_secondary, alpha),
                      "Admin");
        if (input_.click(id_tab_admin, tab)) admin_tab_ = true;
    }
    if (admin_tab_) {
        draw_admin(context, {{area.min.x, y + tab_height}, area.max}, s);
        return;
    }

    const auto kind = news_kind_;
    const auto& items = store.news(kind);
    const bool online = store.state() == ConnectionState::Online;
    if (online && items.empty() && store.has_more(kind) && !store.loading(kind) && !news_failed_[slot(kind)])
        request_news(context, kind, std::nullopt);

    const SocialRect view{{area.min.x + pad, y + tab_height + 12.0F * s}, {area.max.x - pad * 0.5F, area.max.y - pad * 0.5F}};
    const float width = view.width() - pad * 0.5F;
    const float gap = metrics::card_gap * s;
    float content = 0.0F;
    for (const auto& item : items) content += card(context, item, width).height + gap;
    const bool loading = store.loading(kind);
    const bool failed = news_failed_[slot(kind)];
    const float footer = loading || items.empty() || failed ? 36.0F * s : 0.0F;
    content += footer;

    auto& scroll = news_scroll_[slot(kind)];
    float top = scroll.begin(list, view, content, input_.pointer(), context.frame.dt, s);
    const float card_padding = metrics::card_padding * s;
    const float radius = metrics::card_radius * s;
    // Virtualised: only cards that intersect the view are drawn (spec 4.9).
    for (const auto& item : items) {
        const auto& layout = card(context, item, width);
        const float bottom = top + layout.height;
        if (bottom >= view.min.y && top <= view.max.y) {
            const ImVec2 min{view.min.x, top}, max{view.min.x + width, bottom};
            // The Main Menu's glass plate, without the accent line.
            GlassPlate plate;
            plate.accent = false;
            plate.fill = style::card_fill;
            plate.alpha = alpha;
            draw_glass_plate(list, min, max, radius, plate);
            const float text_x = min.x + card_padding;
            float line_top = min.y + card_padding;
            for (const auto& span : layout.title) {
                fonts.draw(list, SocialText::Title, text_x, line_top, to_u32(style::heading, alpha),
                           std::string_view(item.title).substr(span.begin, span.end - span.begin));
                line_top += fonts.line_height(SocialText::Title);
            }
            line_top += metrics::text_gap * s;
            fonts.draw(list, SocialText::Small, text_x, line_top, to_u32(style::text_secondary, alpha), layout.date);
            if (item.version) {
                const float tag_x = text_x + fonts.measure(SocialText::Small, layout.date) + 10.0F * s;
                const float tag_width = fonts.measure(SocialText::Small, *item.version) + 12.0F * s;
                const float small = fonts.line_height(SocialText::Small);
                list->AddRectFilled({tag_x, line_top - 1.0F * s}, {tag_x + tag_width, line_top + small + 1.0F * s},
                                    to_u32(style::tag_fill, alpha), 6.0F * s);
                fonts.draw(list, SocialText::Small, tag_x + 6.0F * s, line_top, to_u32(style::heading, alpha), *item.version);
            }
            line_top += fonts.line_height(SocialText::Small);
            if (!layout.body.empty()) {
                line_top += metrics::text_gap * s * 1.5F;
                for (const auto& span : layout.body) {
                    // Spec 4.6: plain text with its line breaks kept.
                    if (line_top + fonts.line_height(SocialText::Body) >= view.min.y && line_top <= view.max.y)
                        draw_body_line(context, item, span, text_x, line_top, alpha);
                    line_top += fonts.line_height(SocialText::Body);
                }
            }
        }
        top = bottom + gap;
    }
    if (footer > 0.0F) {
        const SocialRect row{{view.min.x, top}, {view.min.x + width, top + footer}};
        if (failed && !loading) {
            draw_social_button(list, fonts, &context.shadows, row, "Retry", SocialButton::Normal,
                               motion(id_news_retry, row, context.frame.dt), true, s, alpha);
            if (input_.click(id_news_retry, row)) request_news(context, kind, items.empty() ? std::nullopt : std::optional{items.back().id});
        } else {
            draw_centered(list, fonts, SocialText::Body, row, to_u32(style::text_secondary, alpha),
                          loading ? "Loading…" : empty_labels[slot(kind)]);
        }
    }
    scroll.end(list, input_, id_news_scroll, s, alpha);

    // Spec 4.6: reaching the bottom loads the next page (before = the last id).
    if (online && !items.empty() && store.has_more(kind) && !store.loading(kind) && !failed &&
        scroll.max_offset() - scroll.offset() < metrics::load_more_distance * s)
        request_news(context, kind, items.back().id);
}

void SocialScreen::draw_settings(Context& context, const SocialRect& area, float s, float alpha) {
    auto* list = context.list;
    const auto& fonts = context.fonts;
    auto& store = context.store;
    const float pad = metrics::content_padding * s;
    const float x0 = area.min.x + pad, x1 = area.max.x - pad;
    float y = area.min.y + pad;
    fonts.draw(list, SocialText::Heading, x0, y, to_u32(style::heading, alpha), "Settings");
    y += fonts.line_height(SocialText::Heading) + 12.0F * s;
    fonts.draw(list, SocialText::Small, x0 + 4.0F * s, y, to_u32(style::text_secondary, alpha), "NOTIFICATIONS");
    y += fonts.line_height(SocialText::Small) + 4.0F * s;

    auto notify = store.notify();
    const float row = metrics::row_height * s;
    const float radius = metrics::item_radius * s;
    struct Toggle {
        const char* label;
        bool* value;
    };
    const Toggle toggles[]{{"Direct messages", &notify.dm}, {"Friend requests", &notify.friend_requests},
                           {"News", &notify.news}, {"Show while playing", &notify.in_game}, {"Sound", &notify.sound}};
    bool changed = false;
    std::uint32_t id = id_toggle_first;
    for (const auto& toggle : toggles) {
        const SocialRect rect{{x0, y}, {x1, y + row}};
        toggle_rects_[id - id_toggle_first] = rect;
        const auto& m = motion(id, rect, context.frame.dt);
        if (m.hover > 0.0F) {
            GlassPlate plate;
            plate.hover = m.hover;
            plate.accent = false;
            plate.drop = false;
            plate.fill = style::plate;
            plate.alpha = alpha * m.hover * 0.7F;
            draw_glass_plate(list, rect.min, rect.max, radius, plate);
        }
        fonts.draw(list, SocialText::Body, x0 + 10.0F * s, y + (row - fonts.line_height(SocialText::Body)) * 0.5F,
                   to_u32(style::text, alpha), toggle.label);
        const float tw = metrics::toggle_width * s, th = metrics::toggle_height * s;
        draw_social_toggle(list, {{x1 - 10.0F * s - tw, y + (row - th) * 0.5F}, {x1 - 10.0F * s, y + (row + th) * 0.5F}},
                           *toggle.value, m.hover, alpha);
        if (input_.click(id++, rect)) {
            *toggle.value = !*toggle.value;
            changed = true;
        }
        y += row;
    }

    // In-game vertical offset of notifications, previewed while dragging and saved on release.
    const SocialRect offset_row{{x0, y}, {x1, y + row}};
    fonts.draw(list, SocialText::Body, x0 + 10.0F * s, y + (row - fonts.line_height(SocialText::Body)) * 0.5F,
               to_u32(style::text, alpha), "Position while playing");
    char value_text[24];
    std::snprintf(value_text, sizeof value_text, "+%d px", static_cast<int>(std::lround(notify.in_game_offset_y)));
    const float value_width = fonts.measure(SocialText::Small, "+400 px");
    const float slider_width = metrics::slider_width * s;
    const float slider_right = x1 - 10.0F * s - value_width - 10.0F * s;
    const SocialRect slider{{slider_right - slider_width, y + row * 0.25F}, {slider_right, y + row * 0.75F}};
    const bool dragging = input_.drag(id_offset, slider);
    if (dragging) {
        const float t = std::clamp((input_.pointer().position.x - slider.min.x) / slider.width(), 0.0F, 1.0F);
        notify.in_game_offset_y = std::round(t * max_offset);
        slider_dragging_ = true;
        store.set_notify(notify);
    } else if (slider_dragging_) {
        slider_dragging_ = false;
        save_settings(context);
    }
    draw_social_slider(list, slider, std::clamp(notify.in_game_offset_y / max_offset, 0.0F, 1.0F),
                       dragging || input_.hover(slider), s, alpha);
    fonts.draw(list, SocialText::Small, x1 - 10.0F * s - value_width,
               y + (row - fonts.line_height(SocialText::Small)) * 0.5F, to_u32(style::text_secondary, alpha), value_text);
    (void)offset_row;
    y += row + 12.0F * s;

    if (changed) {
        store.set_notify(notify);
        save_settings(context);
    }

    fonts.draw(list, SocialText::Small, x0 + 4.0F * s, y, to_u32(style::text_secondary, alpha), "ACCOUNT");
    y += fonts.line_height(SocialText::Small) + 4.0F * s;
    const auto& me = store.me();
    const std::string account = std::string("Signed in as ") + (me ? me->name : std::string("-"));
    fonts.draw(list, SocialText::Body, x0 + 10.0F * s, y + (row - fonts.line_height(SocialText::Body)) * 0.5F,
               to_u32(style::text, alpha), account);
    const float logout_width = fonts.measure(SocialText::Title, "Sign out") + 44.0F * s;
    const SocialRect logout{{x1 - logout_width, y + (row - metrics::button_height * s * 0.85F) * 0.5F},
                            {x1, y + (row + metrics::button_height * s * 0.85F) * 0.5F}};
    sign_out_rect_ = logout;
    draw_social_button(list, fonts, &context.shadows, logout, "Sign out", SocialButton::Danger,
                       motion(id_logout, logout, context.frame.dt), true, s, alpha);
    // Spec 4.6: delete license.bin, disconnect, and go back to the login screen.
    if (input_.click(id_logout, logout) && context.channel) {
        context.channel->commands.push(social::Logout{});
        view_ = SocialView::Home;
    }
    y += row + 10.0F * s;
    if (settings_failed_) {
        fonts.draw(list, SocialText::Body, x0 + 10.0F * s, y, to_u32(style::error, alpha),
                   "Settings could not be saved (social.json is invalid)");
    }
}

void SocialScreen::draw_friend_list(Context& context, const SocialRect& area, float s, float alpha) {
    auto* list = context.list;
    const auto& fonts = context.fonts;
    const auto& friends = context.store.friends();
    const float pad = metrics::sidebar_padding * s;
    const float row = metrics::friend_row_height * s;
    friend_rects_.clear();
    const float top = friends_scroll_.begin(list, area, static_cast<float>(friends.size()) * row + 4.0F * s,
                                            input_.pointer(), context.frame.dt, s);
    if (friends.empty())
        fonts.draw(list, SocialText::Small, area.min.x + pad + 6.0F * s, top + 6.0F * s, to_u32(style::text_secondary, alpha),
                   "No friends yet");
    float y = top;
    std::string scratch;
    for (std::size_t i = 0; i < friends.size(); ++i, y += row) {
        const auto& entry = friends[i];
        const SocialRect rect{{area.min.x + pad, y}, {area.max.x - pad, y + row - 2.0F * s}};
        friend_rects_.emplace_back(entry.user.id, rect);
        if (rect.max.y < area.min.y || rect.min.y > area.max.y) continue;
        const auto id = id_friend_first + static_cast<std::uint32_t>(i);
        const bool selected = view_ == SocialView::Dm && dm_with_ == entry.user.id;
        const auto& m = motion(id, rect, context.frame.dt);
        const float shown = selected ? 1.0F : m.hover;
        if (shown > 0.0F) {
            GlassPlate plate;
            plate.hover = m.hover;
            plate.press = m.press;
            plate.accent = selected;
            plate.accent_inset = rect.height() * metrics::accent_inset;
            plate.drop = false;
            plate.fill = style::plate;
            plate.alpha = alpha * shown;
            draw_glass_plate(list, rect.min, rect.max, metrics::item_radius * s, plate);
        }
        // Spec 4.6: the online dot, the name and the unread badge.
        const float dot = metrics::status_dot * s * 0.5F;
        const float cy = (rect.min.y + rect.max.y) * 0.5F;
        const float dot_x = rect.min.x + 14.0F * s;
        list->AddCircleFilled({dot_x, cy}, dot, to_u32(entry.online ? style::online : style::offline, alpha), 16);
        float right = rect.max.x - 6.0F * s;
        if (entry.unread > 0) {
            const std::string count = entry.unread > 99 ? "99+" : std::to_string(entry.unread);
            const float h = metrics::badge_height * s;
            const float w = std::max(h, fonts.measure(SocialText::Small, count) + 10.0F * s);
            list->AddRectFilled({right - w, cy - h * 0.5F}, {right, cy + h * 0.5F}, to_u32(style::unread_badge, alpha), h * 0.5F);
            fonts.draw(list, SocialText::Small, right - w + (w - fonts.measure(SocialText::Small, count)) * 0.5F,
                       cy - fonts.line_height(SocialText::Small) * 0.5F, to_u32(theme::white, alpha), count);
            right -= w + 6.0F * s;
        }
        const float name_x = dot_x + dot + 8.0F * s;
        const auto name = fonts.ellipsize(SocialText::Body, entry.user.name, right - name_x, scratch);
        fonts.draw(list, SocialText::Body, name_x, cy - fonts.line_height(SocialText::Body) * 0.5F,
                   to_u32(selected ? style::heading : style::text, alpha), name);
        if (input_.click(id, rect) && area.contains(input_.pointer().position)) open_dm(entry.user.id);
        if (input_.right_click(rect) && area.contains(input_.pointer().position)) {
            menu_for_ = entry.user.id;
            menu_at_ = input_.pointer().position;
        }
    }
    friends_scroll_.end(list, input_, id_friends_scroll, s, alpha);
}

bool SocialScreen::row_button(Context& context, std::uint32_t id, const SocialRect& rect, const char* label,
                              SocialButton kind, bool enabled, float s) {
    draw_social_button(context.list, context.fonts, &context.shadows, rect, label, kind, motion(id, rect, context.frame.dt),
                       enabled, s, 1.0F);
    return input_.click(id, rect) && enabled;
}

void SocialScreen::draw_chat(Context& context, const SocialRect& area, social::ConversationId conversation, float s) {
    const float pad = metrics::content_padding * s;
    const SocialRect inner{{area.min.x + pad, area.min.y + pad * 0.5F}, {area.max.x - pad, area.max.y - pad * 0.5F}};
    if (conversation != social::global_conversation && !context.store.find_friend(conversation) &&
        context.store.friends_loaded()) {
        draw_placeholder(context, area, "Friends", s, 1.0F);
        return;
    }
    ChatContext chat_context{context.list, context.fonts, context.shadows, context.store, context.channel, input_,
                             context.frame.dt, s, ImGui::GetTime()};
    chat(conversation).render(chat_context, inner);
}

void SocialScreen::draw_friends(Context& context, const SocialRect& area, float s) {
    auto* list = context.list;
    auto& fonts = context.fonts;
    auto& store = context.store;
    const float pad = metrics::content_padding * s;
    const float x0 = area.min.x + pad, x1 = area.max.x - pad;
    float y = area.min.y + pad;
    fonts.draw(list, SocialText::Heading, x0, y, to_u32(style::heading), "Friends");
    y += fonts.line_height(SocialText::Heading) + 12.0F * s;

    // Spec 4.6: search 300 ms after typing stops.
    const float field_h = metrics::input_height * s;
    search_rect_ = {{x0, y}, {std::min(x1, x0 + 380.0F * s), y + field_h}};
    if (input_.click(id_search_field, search_rect_)) search_input_.set_focused(true);
    search_input_.draw(list, fonts, search_rect_, s, 1.0F, "Search by name", true, ImGui::GetTime(), &context.shadows);
    const double now = ImGui::GetTime();
    std::string query = search_input_.utf8();
    std::erase(query, ' ');
    if (query != search_typed_) {
        search_typed_ = query;
        search_due_ = now + 0.3;
    }
    if (!query.empty() && query != searched_ && search_due_ >= 0.0 && now >= search_due_ && context.channel &&
        store.state() == ConnectionState::Online) {
        context.channel->api.push(social::SearchUsers{query});
        searched_ = query;
        search_due_ = -1.0;
    }
    y += field_h + 4.0F * s;
    const float small = fonts.line_height(SocialText::Small);
    if (!friend_error_.empty() && now < friend_error_until_)
        fonts.draw(list, SocialText::Small, x0 + 4.0F * s, y, to_u32(style::error), friend_error_);
    y += small + 6.0F * s;

    const float row = 38.0F * s;
    const float base_button_w = 92.0F * s, button_h = 30.0F * s;
    const SocialRect view{{area.min.x + pad * 0.5F, y}, {area.max.x - pad * 0.5F, area.max.y - pad * 0.5F}};
    const bool searching = !query.empty() && store.search_query() == query;
    const auto& results = store.search_results();
    const float sections = (searching ? small + 6.0F * s + static_cast<float>(std::max<std::size_t>(1, results.size())) * row : 0.0F) +
                           (store.incoming().empty() ? 0.0F : small + 6.0F * s + static_cast<float>(store.incoming().size()) * row) +
                           (store.outgoing().empty() ? 0.0F : small + 6.0F * s + static_cast<float>(store.outgoing().size()) * row) +
                           row;
    float top = friends_view_scroll_.begin(list, view, sections, input_.pointer(), context.frame.dt, s);
    std::uint32_t id = id_friends_view_first;
    std::string scratch;
    std::int64_t row_user = 0;
    user_buttons_.clear();
    const auto section = [&](const char* title) {
        fonts.draw(list, SocialText::Small, x0 + 4.0F * s, top, to_u32(style::text_secondary), title);
        top += small + 6.0F * s;
    };
    // One row: a glass plate, the name, and buttons from the right.
    const auto user_row = [&](const social::UserRef& user, auto&& buttons) {
        row_user = user.id;
        const SocialRect rect{{x0, top}, {x1, top + row - 4.0F * s}};
        GlassPlate plate;
        plate.accent = false;
        plate.fill = style::card_fill;
        draw_glass_plate(list, rect.min, rect.max, metrics::item_radius * s, plate);
        float right = rect.max.x - 8.0F * s;
        buttons(rect, right);
        const auto name = fonts.ellipsize(SocialText::Body, user.name, right - rect.min.x - 24.0F * s, scratch);
        fonts.draw(list, SocialText::Body, rect.min.x + 14.0F * s, (rect.min.y + rect.max.y - fonts.line_height(SocialText::Body)) * 0.5F,
                   to_u32(style::text), name);
        top += row;
    };
    const auto button = [&](const SocialRect& rect, float& right, const char* label, SocialButton kind, bool enabled) {
        // Wide enough for the label next to the accent line.
        const float button_w = std::max(base_button_w, fonts.measure(SocialText::Title, label) + 44.0F * s);
        const SocialRect place{{right - button_w, (rect.min.y + rect.max.y - button_h) * 0.5F},
                               {right, (rect.min.y + rect.max.y + button_h) * 0.5F}};
        right -= button_w + 8.0F * s;
        user_buttons_.push_back({row_user, label, place});
        return row_button(context, id++, place, label, kind, enabled, s);
    };
    if (searching) {
        section("SEARCH");
        if (results.empty()) {
            fonts.draw(list, SocialText::Body, x0 + 14.0F * s, top + (row - fonts.line_height(SocialText::Body)) * 0.5F,
                       to_u32(style::text_secondary), "No users found");
            top += row;
        }
        for (const auto& result : results) {
            user_row(result.user, [&](const SocialRect& rect, float& right) {
                switch (result.relation) {
                case social::Relation::None:
                    if (button(rect, right, "Add", SocialButton::Primary, true) && context.channel)
                        context.channel->commands.push(social::RequestFriend{next_nonce(), result.user.name});
                    break;
                case social::Relation::Outgoing: button(rect, right, "Requested", SocialButton::Normal, false); break;
                case social::Relation::Friend:
                    if (button(rect, right, "Message", SocialButton::Primary, true)) open_dm(result.user.id);
                    break;
                case social::Relation::Incoming:
                    if (button(rect, right, "Accept", SocialButton::Primary, true) && context.channel)
                        context.channel->commands.push(social::RespondFriend{next_nonce(), result.user.id, true});
                    break;
                }
            });
        }
    }
    if (!store.incoming().empty()) {
        section("REQUESTS");
        for (const auto& user : store.incoming()) {
            user_row(user, [&](const SocialRect& rect, float& right) {
                if (button(rect, right, "Decline", SocialButton::Danger, true) && context.channel)
                    context.channel->commands.push(social::RespondFriend{next_nonce(), user.id, false});
                if (button(rect, right, "Accept", SocialButton::Primary, true) && context.channel)
                    context.channel->commands.push(social::RespondFriend{next_nonce(), user.id, true});
            });
        }
    }
    if (!store.outgoing().empty()) {
        section("SENT");
        for (const auto& user : store.outgoing()) {
            user_row(user, [&](const SocialRect& rect, float& right) {
                if (button(rect, right, "Cancel", SocialButton::Normal, true) && context.channel)
                    context.channel->commands.push(social::RemoveFriend{next_nonce(), user.id});
            });
        }
    }
    if (!searching && store.incoming().empty() && store.outgoing().empty())
        fonts.draw(list, SocialText::Body, x0 + 4.0F * s, top + 8.0F * s, to_u32(style::text_secondary),
                   "Find friends by their name");
    friends_view_scroll_.end(list, input_, id_friends_scroll + 1, s, 1.0F);
}

void SocialScreen::draw_confirm(Context& context, const SocialRect& panel, float s) {
    auto* list = context.list;
    auto& fonts = context.fonts;
    if (confirm_post_) {
        // The warning before a post reaches everyone; nothing is sent until "Post".
        list->AddRectFilled(panel.min, panel.max, to_u32(style::dialog_veil), metrics::panel_radius * s);
        const float w = 400.0F * s, h = 180.0F * s;
        const ImVec2 c{(panel.min.x + panel.max.x) * 0.5F, (panel.min.y + panel.max.y) * 0.5F};
        const SocialRect card{{c.x - w * 0.5F, c.y - h * 0.5F}, {c.x + w * 0.5F, c.y + h * 0.5F}};
        context.shadows.draw(list, card.min, card.max, CornerRadii::all(metrics::panel_radius * s), panel_glow(s), 1.0F);
        list->AddRectFilled(card.min, card.max, to_u32(style::panel_fill), metrics::panel_radius * s);
        list->AddRect(card.min, card.max, to_u32(style::panel_rim), metrics::panel_radius * s);
        const bool attaching = admin_kind_ == NewsKind::Changelog && admin_attach_dll_;
        const char* question = attaching ? "Post this Change Log and publish the DLL?"
                               : admin_kind_ == NewsKind::Changelog ? "Post this Change Log?" : "Post this News?";
        float y = card.min.y + 22.0F * s;
        fonts.draw(list, SocialText::Heading, card.min.x + 24.0F * s, y, to_u32(style::heading), question);
        y += fonts.line_height(SocialText::Heading) + 6.0F * s;
        fonts.draw(list, SocialText::Body, card.min.x + 24.0F * s, y, to_u32(style::description), "Everyone online sees it right away.");
        y += fonts.line_height(SocialText::Body);
        fonts.draw(list, SocialText::Body, card.min.x + 24.0F * s, y, to_u32(style::description), "It can't be edited or deleted later.");
        const float bw = 110.0F * s, bh = metrics::button_height * s;
        const SocialRect cancel{{card.max.x - 24.0F * s - bw, card.max.y - 20.0F * s - bh}, {card.max.x - 24.0F * s, card.max.y - 20.0F * s}};
        const SocialRect post{{cancel.min.x - 10.0F * s - bw, cancel.min.y}, {cancel.min.x - 10.0F * s, cancel.max.y}};
        confirm_ok_rect_ = post;
        if (row_button(context, id_post_ok, post, "Post", SocialButton::Primary, true, s)) {
            confirm_post_ = false;
            if (context.channel) {
                posting_ = true;
                admin_status_.clear();
                const std::string nonce = "a" + std::to_string(next_nonce_++);
                if (attaching) {
                    // One request posts the Change Log and publishes the DLL on
                    // the server, so neither happens without the other.
                    context.channel->api.push(social::PublishRelease{nonce, admin_version_.utf8(), admin_title_.utf8(),
                                                                     admin_body_.utf8(), admin_dll_path_.text()});
                } else {
                    context.channel->commands.push(social::PostNews{nonce, admin_kind_, admin_title_.utf8(),
                                                                    admin_body_.utf8(),
                                                                    admin_kind_ == NewsKind::Changelog ? admin_version_.utf8() : ""});
                }
            }
        } else if (row_button(context, id_post_cancel, cancel, "Cancel", SocialButton::Normal, true, s)) {
            confirm_post_ = false;
        }
        return;
    }
    // The right-click menu on a friend, then "Remove … ?" (spec 4.6).
    if (menu_for_ != 0) {
        const char* label = "Remove friend";
        const float w = fonts.measure(SocialText::Body, label) + 32.0F * s, h = 34.0F * s;
        const SocialRect menu{menu_at_, {menu_at_.x + w, menu_at_.y + h}};
        const auto& m = motion(id_menu_remove, menu, context.frame.dt);
        GlassPlate plate;
        plate.hover = m.hover;
        plate.accent = false;
        plate.fill = style::plate;
        plate.glow = &context.shadows;
        plate.glow_sigma = social_button_glow_sigma(s);
        draw_glass_plate(list, menu.min, menu.max, metrics::item_radius * s, plate);
        fonts.draw(list, SocialText::Body, menu.min.x + 16.0F * s, menu.min.y + (h - fonts.line_height(SocialText::Body)) * 0.5F,
                   to_u32(style::error), label);
        if (input_.click(id_menu_remove, menu)) {
            confirm_remove_ = menu_for_;
            menu_for_ = 0;
        } else if (input_.pressed_any() && !menu.contains(input_.pointer().position)) {
            menu_for_ = 0;
        }
        return;
    }
    const auto* target = context.store.find_friend(confirm_remove_);
    if (!target) {
        confirm_remove_ = 0;
        return;
    }
    list->AddRectFilled(panel.min, panel.max, to_u32(style::dialog_veil), metrics::panel_radius * s);
    const float w = 340.0F * s, h = 150.0F * s;
    const ImVec2 c{(panel.min.x + panel.max.x) * 0.5F, (panel.min.y + panel.max.y) * 0.5F};
    const SocialRect card{{c.x - w * 0.5F, c.y - h * 0.5F}, {c.x + w * 0.5F, c.y + h * 0.5F}};
    context.shadows.draw(list, card.min, card.max, CornerRadii::all(metrics::panel_radius * s), panel_glow(s), 1.0F);
    list->AddRectFilled(card.min, card.max, to_u32(style::panel_fill), metrics::panel_radius * s);
    list->AddRect(card.min, card.max, to_u32(style::panel_rim), metrics::panel_radius * s);
    const std::string question = "Remove " + target->user.name + "?";
    context.fonts.request(SocialText::Heading, question);
    fonts.draw(list, SocialText::Heading, card.min.x + 24.0F * s, card.min.y + 22.0F * s, to_u32(style::heading), question);
    const float bw = 110.0F * s, bh = metrics::button_height * s;
    const SocialRect cancel{{card.max.x - 24.0F * s - bw, card.max.y - 22.0F * s - bh}, {card.max.x - 24.0F * s, card.max.y - 22.0F * s}};
    const SocialRect remove{{cancel.min.x - 10.0F * s - bw, cancel.min.y}, {cancel.min.x - 10.0F * s, cancel.max.y}};
    if (row_button(context, id_confirm_ok, remove, "Remove", SocialButton::Danger, true, s) && context.channel) {
        context.channel->commands.push(social::RemoveFriend{next_nonce(), confirm_remove_});
        confirm_remove_ = 0;
    } else if (row_button(context, id_confirm_cancel, cancel, "Cancel", SocialButton::Normal, true, s)) {
        confirm_remove_ = 0;
    }
}

void SocialScreen::draw_body_line(Context& context, const social::NewsItem& item, const SocialFonts::Line& span, float x,
                                  float top, float alpha) const {
    const auto& fonts = context.fonts;
    const std::string_view text = std::string_view(item.body).substr(span.begin, span.end - span.begin);
    // Only a line's own start counts, not a wrapped continuation.
    const bool line_start = span.begin == 0 || item.body[span.begin - 1] == '\n';
    const Color* mark = nullptr;
    if (item.kind == NewsKind::Changelog && line_start && !text.empty()) {
        if (text.front() == '+') mark = &style::change_added;
        else if (text.front() == '-') mark = &style::change_removed;
        else if (text.front() == '*') mark = &style::change_fixed;
    }
    if (!mark) {
        fonts.draw(context.list, SocialText::Body, x, top, to_u32(style::text, alpha), text);
        return;
    }
    // The mark in its colour, drawn twice a pixel apart so it reads as bold.
    const std::string_view symbol = text.substr(0, 1);
    fonts.draw(context.list, SocialText::Body, x, top, to_u32(*mark, alpha), symbol);
    fonts.draw(context.list, SocialText::Body, x + 1.0F, top, to_u32(*mark, alpha), symbol);
    fonts.draw(context.list, SocialText::Body, x + fonts.measure(SocialText::Body, symbol), top, to_u32(style::text, alpha),
               text.substr(1));
}

void SocialScreen::open_dll_dialog() {
    // The dialog is modal to the host game window, found through ImGui's viewport.
    void* owner = nullptr;
    if (const ImGuiViewport* viewport = ImGui::GetMainViewport()) owner = viewport->PlatformHandleRaw;
    // A double-null filter list: the Core DLL first, then every file.
    std::wstring filter;
    for (const wchar_t* part : {L"Core DLL (*.dll)", L"*.dll", L"All files (*.*)", L"*.*"}) {
        filter.append(part);
        filter.push_back(L'\0');
    }
    admin_dll_dialog_.open(owner, L"Choose the Core DLL to publish", std::move(filter));
}

void SocialScreen::draw_admin(Context& context, const SocialRect& area, float s) {
    auto* list = context.list;
    auto& fonts = context.fonts;
    const float pad = metrics::content_padding * s;
    const float x0 = area.min.x + pad, x1 = area.max.x - pad;
    float y = area.min.y + 12.0F * s;
    const float small = fonts.line_height(SocialText::Small);
    const float field_h = metrics::input_height * s;
    const double now = ImGui::GetTime();
    admin_body_.set_multiline(true);
    // Every field's glyphs, not only the focused one's (text may be set without typing).
    for (auto* input : {&admin_title_, &admin_version_, &admin_body_}) {
        auto& seen = admin_revisions_[input == &admin_title_ ? 0 : input == &admin_version_ ? 1 : 2];
        if (seen == input->revision()) continue;
        seen = input->revision();
        fonts.request(SocialText::Body, input->utf8());
        fonts.request(SocialText::Title, input->utf8());
        fonts.request(SocialText::Small, input->utf8());
    }

    // News or Change Log, as two small pills.
    float x = x0;
    for (const auto kind : {NewsKind::News, NewsKind::Changelog}) {
        const char* label = kind == NewsKind::News ? "News" : "Change Log";
        const float w = fonts.measure(SocialText::Title, label) + 28.0F * s, h = 28.0F * s;
        const SocialRect pill{{x, y}, {x + w, y + h}};
        const auto id = kind == NewsKind::News ? id_admin_news : id_admin_changelog;
        const auto& m = motion(id, pill, context.frame.dt);
        const bool selected = admin_kind_ == kind;
        GlassPlate plate;
        plate.hover = m.hover;
        plate.accent = false;
        plate.drop = selected;
        plate.fill = selected ? style::bubble_mine : style::plate;
        plate.alpha = selected ? 1.0F : 0.5F + 0.5F * m.hover;
        draw_glass_plate(list, pill.min, pill.max, h * 0.5F, plate);
        draw_centered(list, fonts, SocialText::Title, pill, to_u32(selected ? style::heading : style::text_secondary), label);
        if (input_.click(id, pill)) admin_kind_ = kind;
        x = pill.max.x + 8.0F * s;
    }
    // Change Log can carry a Core DLL: posting then also publishes the update.
    if (admin_kind_ == NewsKind::Changelog) {
        // A file chosen in the native dialog (run off the render thread) arrives
        // here; an empty path means it was cancelled, so the choice is unchanged.
        if (std::wstring picked; admin_dll_dialog_.poll(picked) && !picked.empty())
            admin_dll_path_.set_text(picked);
        const bool busy = admin_dll_dialog_.busy();
        const char* label = busy ? "Choosing\xE2\x80\xA6" : "Attach DLL";
        const float w = fonts.measure(SocialText::Title, label) + 28.0F * s, h = 28.0F * s;
        const SocialRect pill{{x, y}, {x + w, y + h}};
        admin_dll_rect_ = pill;
        const auto& m = motion(id_admin_dll, pill, context.frame.dt);
        GlassPlate plate;
        plate.hover = m.hover;
        plate.accent = false;
        plate.drop = admin_attach_dll_;
        plate.fill = admin_attach_dll_ ? style::bubble_mine : style::plate;
        plate.alpha = admin_attach_dll_ ? 1.0F : 0.5F + 0.5F * m.hover;
        draw_glass_plate(list, pill.min, pill.max, h * 0.5F, plate);
        draw_centered(list, fonts, SocialText::Title, pill,
                      to_u32(admin_attach_dll_ ? style::heading : style::text_secondary), label);
        if (input_.click(id_admin_dll, pill) && !busy && !posting_) {
            if (admin_attach_dll_) {
                // Turn attaching off and forget the chosen file.
                admin_attach_dll_ = false;
                admin_dll_path_.clear();
            } else {
                // Turn attaching on and open the native picker to choose the DLL.
                admin_attach_dll_ = true;
                if (dll_picker_enabled_) open_dll_dialog();
            }
        }
    } else {
        admin_attach_dll_ = false;
        admin_dll_rect_ = {};
    }
    y += 28.0F * s + 12.0F * s;

    // Title, and the version for a Change Log.
    const bool changelog = admin_kind_ == NewsKind::Changelog;
    const float version_w = changelog ? 170.0F * s : 0.0F;
    const float gap = changelog ? 12.0F * s : 0.0F;
    fonts.draw(list, SocialText::Small, x0 + 4.0F * s, y, to_u32(style::text_secondary), "TITLE");
    if (changelog) fonts.draw(list, SocialText::Small, x1 - version_w + 4.0F * s, y, to_u32(style::text_secondary), "VERSION");
    y += small + 4.0F * s;
    // One field owns the keyboard. focused_input() takes the first focused field,
    // so focusing only the clicked one left every key in Title once it was used.
    const auto focus = [this](SocialTextInput& field) {
        for (auto* input : {&admin_title_, &admin_version_, &admin_body_}) input->set_focused(input == &field);
    };
    const SocialRect title{{x0, y}, {x1 - version_w - gap, y + field_h}};
    admin_field_rects_[0] = title;
    if (input_.click(id_admin_title, title)) focus(admin_title_);
    admin_title_.draw(list, fonts, title, s, 1.0F, "Title", !posting_, now, &context.shadows);
    if (changelog) {
        const SocialRect version{{x1 - version_w, y}, {x1, y + field_h}};
        admin_field_rects_[1] = version;
        if (input_.click(id_admin_version, version)) focus(admin_version_);
        admin_version_.draw(list, fonts, version, s, 1.0F, "Version (optional)", !posting_, now, &context.shadows);
    } else {
        // News hides the version; it must not keep typing into a field nobody sees.
        admin_field_rects_[1] = {};
        admin_version_.set_focused(false);
    }
    y += field_h + 12.0F * s;
    const bool attach = changelog && admin_attach_dll_;
    if (attach) {
        fonts.draw(list, SocialText::Small, x0 + 4.0F * s, y, to_u32(style::text_secondary), "DLL FILE");
        y += small + 4.0F * s;
        const SocialRect field{{x0, y}, {x1, y + field_h}};
        // Read-only: the path is chosen in the native dialog, not typed. Clicking
        // the field (re)opens the picker.
        if (dll_picker_enabled_ && !admin_dll_dialog_.busy() && !posting_ && input_.click(id_admin_dll_path, field))
            open_dll_dialog();
        admin_dll_path_.draw(list, fonts, field, s, 1.0F, "Click to choose a DLL file", !posting_, now, &context.shadows);
        y += field_h + 12.0F * s;
    }

    // The text on the left, a live preview on the right.
    const float button_h = metrics::button_height * s;
    const float bottom = area.max.y - pad * 0.75F - button_h - 12.0F * s;
    const float split = x0 + (x1 - x0) * 0.55F;
    fonts.draw(list, SocialText::Small, x0 + 4.0F * s, y, to_u32(style::text_secondary), "TEXT");
    fonts.draw(list, SocialText::Small, split + 16.0F * s, y, to_u32(style::text_secondary), "PREVIEW");
    if (changelog) {
        // The marks and their colours.
        const char* legend[]{"+ added", "- removed", "* fixed"};
        const Color* colors[]{&style::change_added, &style::change_removed, &style::change_fixed};
        float lx = split - 4.0F * s;
        for (int i = 2; i >= 0; --i) {
            lx -= fonts.measure(SocialText::Small, legend[i]);
            fonts.draw(list, SocialText::Small, lx, y, to_u32(*colors[i]), std::string_view(legend[i], 1));
            fonts.draw(list, SocialText::Small, lx + fonts.measure(SocialText::Small, std::string_view(legend[i], 1)), y,
                       to_u32(style::text_secondary), std::string_view(legend[i]).substr(1));
            lx -= 10.0F * s;
        }
    }
    y += small + 4.0F * s;
    const SocialRect body{{x0, y}, {split, bottom}};
    admin_field_rects_[2] = body;
    if (input_.click(id_admin_body, body)) focus(admin_body_);
    admin_body_.draw(list, fonts, body, s, 1.0F, changelog ? "+ Added Killaura" : "Write the post here", !posting_, now,
                     &context.shadows);
    // Preview: the card as it will appear, colours included.
    const SocialRect preview{{split + 12.0F * s, y}, {x1, bottom}};
    GlassPlate plate;
    plate.accent = false;
    plate.fill = style::card_fill;
    draw_glass_plate(list, preview.min, preview.max, metrics::card_radius * s, plate);
    social::NewsItem item;
    item.kind = admin_kind_;
    item.title = admin_title_.utf8();
    item.body = admin_body_.utf8();
    list->PushClipRect(preview.min, preview.max, true);
    const float inner = preview.width() - metrics::card_padding * s * 2.0F;
    float py = preview.min.y + metrics::card_padding * s;
    const float px = preview.min.x + metrics::card_padding * s;
    if (item.title.empty() && item.body.empty()) {
        fonts.draw(list, SocialText::Body, px, py, to_u32(style::text_secondary), "Nothing to preview yet");
    } else {
        std::vector<SocialFonts::Line> lines;
        fonts.wrap(SocialText::Title, item.title, inner, lines);
        for (const auto& span : lines) {
            fonts.draw(list, SocialText::Title, px, py, to_u32(style::heading),
                       std::string_view(item.title).substr(span.begin, span.end - span.begin));
            py += fonts.line_height(SocialText::Title);
        }
        if (changelog && admin_version_.length() > 0) {
            const std::string version = admin_version_.utf8();
            const float tag_w = fonts.measure(SocialText::Small, version) + 12.0F * s;
            list->AddRectFilled({px, py + 2.0F * s}, {px + tag_w, py + small + 4.0F * s}, to_u32(style::tag_fill), 6.0F * s);
            fonts.draw(list, SocialText::Small, px + 6.0F * s, py + 3.0F * s, to_u32(style::heading), version);
            py += small + 6.0F * s;
        }
        py += metrics::text_gap * s * 1.5F;
        lines.clear();
        fonts.wrap(SocialText::Body, item.body, inner, lines);
        for (const auto& span : lines) {
            draw_body_line(context, item, span, px, py, 1.0F);
            py += fonts.line_height(SocialText::Body);
        }
    }
    list->PopClipRect();

    // Post: always through the confirmation.
    admin_post_rect_ = {{x1 - 120.0F * s, area.max.y - pad * 0.75F - button_h}, {x1, area.max.y - pad * 0.75F}};
    const auto has_text = [](const SocialTextInput& input) {
        for (const wchar_t c : input.text())
            if (c != L' ' && c != L'\n' && c != L'\t' && c != 0x3000) return true;
        return false;
    };
    const bool ready = !posting_ && has_text(admin_title_) && has_text(admin_body_) &&
                       (!attach || (has_text(admin_version_) && admin_dll_path_.length() > 0)) &&
                       context.store.state() == ConnectionState::Online;
    if (row_button(context, id_admin_post, admin_post_rect_, posting_ ? "Posting\xE2\x80\xA6" : "Post", SocialButton::Primary, ready, s)) {
        if (auto* input = focused_input()) input->set_focused(false);
        confirm_post_ = true;
    }
    if (!admin_status_.empty() && now < admin_status_until_) {
        fonts.draw(list, SocialText::Small, x0 + 4.0F * s,
                   admin_post_rect_.min.y + (button_h - small) * 0.5F, to_u32(admin_status_ok_ ? style::online : style::error),
                   admin_status_);
    }
}

void SocialScreen::draw_placeholder(Context& context, const SocialRect& area, const char* title, float s, float alpha) {
    const float pad = metrics::content_padding * s;
    context.fonts.draw(context.list, SocialText::Heading, area.min.x + pad, area.min.y + pad, to_u32(style::heading, alpha), title);
    draw_centered(context.list, context.fonts, SocialText::Body, area, to_u32(style::text_secondary, alpha), "Coming soon");
}

void SocialScreen::draw_login(Context& context, const SocialRect& panel, float s, float alpha) {
    auto* list = context.list;
    const auto& fonts = context.fonts;
    const float pad = metrics::login_padding * s;
    const float width = login_form_width({context.frame.client_w, context.frame.client_h});
    const float x = panel.min.x + (panel.width() - width) * 0.5F;
    const float input_height = metrics::input_height * s;
    const float button_height = metrics::button_height * s;
    const float display = fonts.line_height(SocialText::Display);
    const float body = fonts.line_height(SocialText::Body);
    const float small = fonts.line_height(SocialText::Small);
    const float block = display + 8.0F * s + body + 26.0F * s + input_height + 16.0F * s + button_height + 14.0F * s +
                        body + 6.0F * s + small;
    float y = panel.min.y + std::max(pad, (panel.height() - block) * 0.5F);
    const auto row = [&](float height) { return SocialRect{{x, y}, {x + width, y + height}}; };
    draw_title(list, fonts, row(display), s, alpha, "Medicon Social");
    y += display + 8.0F * s;
    draw_centered(list, fonts, SocialText::Body, row(body), to_u32(style::description, alpha),
                  "Enter your license key to sign in");
    y += body + 26.0F * s;

    const SocialRect field = row(input_height);
    // The field takes the keyboard as soon as the login screen appears.
    if (!key_input_.focused() && !login_busy_ && key_input_.length() == 0 && login_error_.empty()) key_input_.set_focused(true);
    if (input_.click(id_key_field, field)) key_input_.set_focused(true);
    key_input_.draw(list, fonts, field, s, alpha, "MDC-XXXX-XXXX-XXXX-XXXX", !login_busy_, ImGui::GetTime(), &context.shadows);
    y += input_height + 16.0F * s;

    // Like the Main Menu's Play button: glass, accent line, glow, a light press.
    const SocialRect button = row(button_height);
    sign_in_rect_ = button;
    const bool enabled = !login_busy_ && key_input_.length() > 0;
    draw_social_button(list, fonts, &context.shadows, button, login_busy_ ? "Signing in…" : "Sign in", SocialButton::Primary,
                       motion(id_login, button, context.frame.dt), enabled, s, alpha);
    if ((input_.click(id_login, button) && enabled) || key_input_.consume_submit()) submit_login(context);
    y += button_height + 14.0F * s;

    if (!login_error_.empty() && !login_busy_)
        draw_centered(list, fonts, SocialText::Body, row(body), to_u32(style::error, alpha), login_message(login_error_));
    y += body + 6.0F * s;
    draw_centered(list, fonts, SocialText::Small, row(small), to_u32(style::text_secondary, alpha),
                  "Keys start with MDC- and are issued by an admin");
}

void SocialScreen::render(Context& context) {
    animation_.step(open_, context.frame.dt, style::timing::panel_open, style::timing::panel_close);
    if (!visible()) return;
    auto* list = context.list;
    const float p = animation_.value();
    const ImVec2 display{context.frame.client_w, context.frame.client_h};
    const float s = layout_scale(display);
    // Behind the panel: the Main Menu's own pastel gradient, then the softly
    // blurred back.png and a milky veil. Never a dark or grey scrim.
    draw_main_menu_backdrop(list, display, context.backdrop_time, p);
    context.background.draw(list, display, p);

    // A closing panel no longer reacts to the pointer or the keyboard.
    PointerInput pointer = open_ ? context.pointer : PointerInput{};
    const float panel_scale = metrics::panel_open_scale + (1.0F - metrics::panel_open_scale) * p;
    if (pointer.valid && panel_scale > 0.0F) {
        pointer.position.x = display.x * 0.5F + (pointer.position.x - display.x * 0.5F) / panel_scale;
        pointer.position.y = display.y * 0.5F + (pointer.position.y - display.y * 0.5F) / panel_scale;
    }
    // While a menu or confirmation is up, nothing under it takes the pointer.
    const bool popup = menu_for_ != 0 || confirm_remove_ != 0 || confirm_post_;
    input_.begin(pointer);
    input_.set_blocked(popup);
    for (auto* input : {&key_input_, &search_input_, &admin_title_, &admin_version_, &admin_body_, &admin_dll_path_}) {
        input->update();
        input->set_animation(input_animation_);
    }
    for (auto& [id, view] : chats_) {
        view->field().update();
        view->field().set_animation(input_animation_);
    }
    // Entering a chat gives its field the keyboard.
    if (view_ != shown_view_ || (view_ == SocialView::Dm && dm_with_ != shown_dm_)) {
        if (auto* input = focused_input()) input->set_focused(false);
        if (view_ == SocialView::Global || view_ == SocialView::Dm)
            chat(view_ == SocialView::Global ? social::global_conversation : dm_with_).field().set_focused(true);
        shown_view_ = view_;
        shown_dm_ = dm_with_;
    }
    // Characters arrive through char_event, in order with the keys (ImGui's own
    // queue can hold them back a frame behind an Enter).
    if (auto* input = open_ ? focused_input() : nullptr; input && (input != glyphs_for_ || input->revision() != glyphs_revision_)) {
        // Typed text needs its glyphs in the Social atlas (Japanese included); asked once per edit.
        glyphs_for_ = input;
        glyphs_revision_ = input->revision();
        context.fonts.request(SocialText::Body, input->utf8());
    }
    context.store.set_open_dm(open_ && view_ == SocialView::Dm ? dm_with_ : 0);
    if (refresh_friends_ && context.channel && context.store.state() == ConnectionState::Online) {
        refresh_friends_ = false;
        context.channel->api.push(social::FetchFriends{});
    }

    const bool login = showing_login(context.store);
    const ImVec2 size = panel_size(display, login);
    const float width = size.x, height = size.y;
    const ImVec2 corner{std::round((display.x - width) * 0.5F), std::round((display.y - height) * 0.5F)};
    const SocialRect panel{corner, {corner.x + width, corner.y + height}};
    const int first_vertex = list->VtxBuffer.Size;
    list->AddDrawCmd();
    const int first_command = list->CmdBuffer.Size - 1;
    {
        const SocialFonts::Scope scope(context.fonts, list);
        const float radius = metrics::panel_radius * s;
        // Shadow and glow fade and scale with the panel (scale_from below).
        context.shadows.draw(list, panel.min, panel.max, CornerRadii::all(radius), panel_shadow(s), 1.0F);
        context.shadows.draw(list, panel.min, panel.max, CornerRadii::all(radius), panel_glow(s), 1.0F);
        list->AddRectFilled(panel.min, panel.max, to_u32(style::panel_fill), radius);
        const float sidebar = sidebar_width(width, s);
        // Right of the sidebar: back.png, lightly blurred, under the content.
        if (!login)
            context.background.draw_panel(list, {panel.min.x + sidebar, panel.min.y}, panel.max, radius,
                                          ImDrawFlags_RoundCornersRight, 1.0F);
        list->AddRect(panel.min, panel.max, to_u32(style::panel_rim), radius, 0, 1.0F);
        if (login) {
            draw_login(context, panel, s, 1.0F);
        } else {
            key_input_.set_focused(false);
            draw_sidebar(context, {panel.min, {panel.min.x + sidebar, panel.max.y}}, s, 1.0F);
            const SocialRect content{{panel.min.x + sidebar, panel.min.y}, panel.max};
            switch (view_) {
            case SocialView::Home: draw_home(context, content, s, 1.0F); break;
            case SocialView::Global: draw_chat(context, content, social::global_conversation, s); break;
            case SocialView::Dm: draw_chat(context, content, dm_with_, s); break;
            case SocialView::Friends: draw_friends(context, content, s); break;
            case SocialView::Settings: draw_settings(context, content, s, 1.0F); break;
            }
        }
        // Spec 4.6 closes with Esc only; outside clicks do nothing. A visible
        // close button is added for the mouse. The sign-in gate has neither.
        if (!locked_) {
            const float size = metrics::close_size * s;
            const float inset = 12.0F * s;
            const SocialRect close_button{{panel.max.x - size - inset, panel.min.y + inset},
                                          {panel.max.x - inset, panel.min.y + inset + size}};
            const bool close_clicked = icon_button(context, id_close, close_button, false, s, 1.0F);
            const ImVec2 c{(close_button.min.x + close_button.max.x) * 0.5F, (close_button.min.y + close_button.max.y) * 0.5F};
            const float arm = size * 0.18F;
            const float thickness = std::max(1.0F, 1.8F * s);
            list->AddLine({c.x - arm, c.y - arm}, {c.x + arm, c.y + arm}, to_u32(style::icon), thickness);
            list->AddLine({c.x - arm, c.y + arm}, {c.x + arm, c.y - arm}, to_u32(style::icon), thickness);
            if (close_clicked) close();
        }
        if (popup && !login) {
            input_.set_blocked(false);
            draw_confirm(context, panel, s);
        }
    }
    input_.end();
    if (input_.pressed_elsewhere())
        if (auto* input = focused_input()) input->set_focused(false);
    if (p < 1.0F) {
        scale_from(list, first_vertex, first_command, {corner.x + width * 0.5F, corner.y + height * 0.5F}, panel_scale, p);
    }
    // Whatever comes next (notifications) must not land in a command whose clip rectangle was scaled.
    list->AddDrawCmd();
}

} // namespace medicon::gui
