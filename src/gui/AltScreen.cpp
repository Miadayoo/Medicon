#include <medicon/gui/AltScreen.hpp>

#include "SocialPanel.hpp"

#include <medicon/gui/Draw.hpp>
#include <medicon/gui/Glass.hpp>
#include <medicon/gui/MainMenu.hpp>
#include <medicon/gui/Shadow.hpp>
#include <medicon/gui/SocialBackground.hpp>
#include <medicon/gui/Theme.hpp>

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace medicon::gui {
namespace {

namespace style = theme::social;
namespace metrics = theme::social::metrics;
using namespace social_panel;

enum : std::uint32_t {
    id_close = 1,
    id_microsoft,
    id_cancel,
    id_scroll,
    id_confirm_ok,
    id_confirm_cancel,
    id_copy_code,
    id_open_sign_in,
    id_row_first = 1000, // + index * 2 + button
};

constexpr float row_height = 66.0F;
constexpr float row_gap = 8.0F;
constexpr float row_button_width = 98.0F;
constexpr float row_button_height = 34.0F;
constexpr float microsoft_height = 44.0F;

const char* stage_text(alt::SignInStage stage) {
    switch (stage) {
    case alt::SignInStage::Starting: return "Requesting sign-in code…";
    case alt::SignInStage::Waiting: return "Enter this code in the Microsoft window";
    case alt::SignInStage::Finishing: return "Adding account…";
    }
    return "";
}

const char* error_text(std::string_view error) {
    if (error == "not_configured") return "Microsoft sign-in isn't set up in this build";
    if (error == "network") return "Cannot reach Microsoft";
    if (error == "window") return "Could not open the sign-in window";
    if (error == "declined") return "Sign-in was declined";
    if (error == "expired") return "Sign-in timed out. Try again";
    if (error == "cancelled") return "Sign-in cancelled. Try again when ready";
    if (error == "no_xbox_profile") return "This account has no Xbox profile";
    if (error == "profile_unavailable") return "Signed in, but Xbox account details could not be loaded. Try again";
    if (error == "child_account") return "This child account needs a family group";
    if (error == "region_blocked") return "Xbox Live isn't available in this account's region";
    if (error == "save_failed") return "Could not save the account";
    if (error == "rejected") return "Microsoft refused the sign-in";
    if (error == "xbox_user_failed") return "Xbox user sign-in was rejected";
    if (error == "xsts_failed") return "Xbox token request was rejected";
    if (error == "profile_failed") return "Could not load the Xbox account details";
    if (error == "bad_response") return "Sign-in returned an incomplete response";
    if (error == "switch_unavailable") return "Account switching is unavailable for this game version";
    if (error == "session_failed") return "Minecraft sign-in failed. Try Use again";
    if (error == "session_busy") return "Wait for Minecraft sign-in to finish, then reconnect";
    if (error == "connection_failed") return "Server sign-in failed. Try Use again before reconnecting";
    if (error == "native_session_stale") return "Return to the title screen, then reconnect";
    if (error == "account_missing") return "This account is no longer saved";
    if (error == "account_in_use") return "Select another account before removing this one";
    return "Microsoft sign-in failed";
}

// The four-colour squares of the Microsoft logo.
void draw_microsoft_logo(ImDrawList* list, ImVec2 origin, float size, float alpha) {
    const float gap = std::max(1.0F, std::round(size * 0.08F));
    const float cell = std::floor((size - gap) * 0.5F);
    const auto square = [&](float x, float y, std::uint32_t color) {
        const ImVec2 min{origin.x + x, origin.y + y};
        list->AddRectFilled(min, {min.x + cell, min.y + cell}, to_u32(theme::rgb(color), alpha));
    };
    square(0.0F, 0.0F, 0xF25022);
    square(cell + gap, 0.0F, 0x7FBA00);
    square(0.0F, cell + gap, 0x00A4EF);
    square(cell + gap, cell + gap, 0xFFB900);
}

// The glass button with the logo in front of its label, centred together.
void draw_microsoft_button(ImDrawList* list, const SocialFonts& fonts, IShadowRenderer* glow, const SocialRect& rect,
                           const SocialMotion& motion, bool enabled, float s) {
    const char* label = "Microsoft";
    const float fade = enabled ? 1.0F : 0.7F;
    const float press = enabled ? motion.press : 0.0F;
    const ImVec2 center{(rect.min.x + rect.max.x) * 0.5F, (rect.min.y + rect.max.y) * 0.5F};
    const ScaleAround pressed(list, center, 1.0F - (1.0F - metrics::button_press_scale) * press);
    GlassPlate plate;
    plate.hover = enabled ? motion.hover : 0.0F;
    plate.press = press;
    plate.fill = style::plate;
    plate.alpha = fade;
    plate.glow = enabled ? glow : nullptr;
    plate.glow_sigma = social_button_glow_sigma(s);
    plate.accent_inset = rect.height() * metrics::accent_inset;
    draw_glass_plate(list, rect.min, rect.max, metrics::button_radius * s, plate);
    const float logo = std::round(18.0F * s);
    const float gap = std::round(10.0F * s);
    const float width = logo + gap + fonts.measure(SocialText::Title, label);
    const float x = std::round(center.x - width * 0.5F);
    draw_microsoft_logo(list, {x, std::round(center.y - logo * 0.5F)}, logo, fade);
    fonts.draw(list, SocialText::Title, x + logo + gap, center.y - fonts.line_height(SocialText::Title) * 0.5F,
               to_u32(style::heading, fade), label);
}

} // namespace

void AltScreen::request_glyphs(SocialFonts& fonts) {
    fonts.request(SocialText::Display, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-");
    fonts.request(SocialText::Heading, "Microsoft sign-in");
    for (const char* text : {"AltManager", "Remove this account?"}) fonts.request(SocialText::Heading, text);
    for (const char* text : {"Microsoft", "Remove", "Cancel", "Use", "In use", "Selected", "Copy code", "Copy again", "Open Microsoft"})
        fonts.request(SocialText::Title, text);
    for (const char* text : {"No accounts yet", "alts.bin could not be read", "Added "})
        fonts.request(SocialText::Body, text);
    for (const char* text : {"ADD ACCOUNT", "XUID 0123456789", "CODE ABCDEFGHIJKLMNOPQRSTUVWXYZ",
                             "Code copied. Paste with Ctrl+V.", "Copy the code or enter it manually.", "Copying…",
                             "Open Microsoft, then paste this code."})
        fonts.request(SocialText::Small, text);
    for (const auto stage : {alt::SignInStage::Starting, alt::SignInStage::Waiting, alt::SignInStage::Finishing})
        fonts.request(SocialText::Small, stage_text(stage));
    for (const char* error : {"not_configured", "network", "window", "declined", "expired", "no_xbox_profile",
                              "child_account", "region_blocked", "save_failed", "rejected", "cancelled", "profile_unavailable",
                              "xbox_user_failed", "xsts_failed", "profile_failed", "bad_response", "switch_unavailable",
                              "session_failed", "session_busy", "connection_failed", "native_session_stale", "account_missing", "account_in_use", ""})
        fonts.request(SocialText::Small, error_text(error));
}

void AltScreen::open() {
    if (!open_) input_.reset();
    open_ = true;
}

void AltScreen::close() noexcept {
    open_ = false;
    pending_use_id_ = 0;
    use_finished_.reset();
    confirm_remove_.clear();
}

void AltScreen::key_event(int virtual_key, bool down) {
    if (!down || !open_ || virtual_key != VK_ESCAPE) return;
    if (!confirm_remove_.empty()) confirm_remove_.clear();
    else close();
}

void AltScreen::on_event(const alt::Event& event, SocialFonts& fonts) {
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, alt::AccountsChanged>) {
            accounts_ = value.accounts;
            load_failed_ = value.load_failed;
            for (const auto& account : accounts_) {
                fonts.request(SocialText::Title, account.gamertag);
                fonts.request(SocialText::Heading, account.gamertag);
            }
        } else if constexpr (std::is_same_v<T, alt::SignInProgress>) {
            stage_ = value.stage;
            user_code_ = value.user_code;
            code_copied_ = value.code_copied;
            copy_pending_ = false;
            window_opened_ = value.window_opened;
            error_.clear();
            diagnostic_.clear();
            added_.clear();
            fonts.request(SocialText::Display, user_code_);
            fonts.request(SocialText::Small, user_code_);
        } else if constexpr (std::is_same_v<T, alt::SessionChanged>) {
            session_ = value;
            added_.clear();
            error_ = value.error;
            diagnostic_ = value.diagnostic;
            fonts.request(SocialText::Small, diagnostic_);
        } else if constexpr (std::is_same_v<T, alt::UseFinished>) {
            if (pending_use_id_ && value.request_id == pending_use_id_) {
                pending_use_id_ = 0;
                use_finished_ = value;
            }
        } else if constexpr (std::is_same_v<T, alt::SignInFinished>) {
            stage_.reset();
            user_code_.clear();
            code_copied_ = false;
            copy_pending_ = false;
            error_ = value.ok ? std::string{} : value.cancelled ? "cancelled" : value.error;
            diagnostic_ = value.ok || value.cancelled ? std::string{} : value.diagnostic;
            fonts.request(SocialText::Small, diagnostic_);
            added_ = value.ok ? value.gamertag : std::string{};
            if (value.ok) fonts.request(SocialText::Body, value.gamertag);
        }
    }, event);
}

SocialRect AltScreen::remove_rect(std::string_view xuid) const noexcept {
    for (const auto& button : remove_rects_)
        if (button.xuid == xuid) return button.rect;
    return {};
}

SocialRect AltScreen::use_rect(std::string_view xuid) const noexcept {
    for (const auto& button : use_rects_)
        if (button.xuid == xuid) return button.rect;
    return {};
}

const SocialMotion& AltScreen::motion(std::uint32_t id, const SocialRect& rect, float dt) {
    auto& state = motion_[id];
    state.step(input_.hover(rect), input_.held(id), dt);
    return state;
}

void AltScreen::draw_list(Context& context, const SocialRect& area, float s) {
    auto* list = context.list;
    const auto& fonts = context.fonts;
    const float pad = metrics::content_padding * s;
    const float x0 = area.min.x + pad, x1 = area.max.x - pad;
    float y = area.min.y + pad;
    fonts.draw(list, SocialText::Heading, x0, y, to_u32(style::heading), "AltManager");
    y += fonts.line_height(SocialText::Heading) + 12.0F * s;

    const SocialRect view{{x0, y}, {x1, area.max.y - pad}};
    remove_rects_.clear();
    use_rects_.clear();
    code_rect_ = copy_code_rect_ = open_sign_in_rect_ = {};
    if (stage_) {
        draw_sign_in(context, view, s);
        return;
    }
    if (accounts_.empty()) {
        if (load_failed_) {
            draw_centered(list, fonts, SocialText::Body, view, to_u32(style::error), "alts.bin could not be read");
            return;
        }
        draw_centered(list, fonts, SocialText::Body, view, to_u32(style::text), "No accounts yet");
        return;
    }

    const float height = row_height * s, gap = row_gap * s;
    const float content = static_cast<float>(accounts_.size()) * (height + gap) - gap;
    float top = scroll_.begin(list, view, content, input_.pointer(), context.frame.dt, s);
    const float scrollbar = scroll_.max_offset() > 0.0F ? 10.0F * s : 0.0F;
    std::string scratch;
    for (std::size_t i = 0; i < accounts_.size(); ++i, top += height + gap) {
        if (top + height < view.min.y || top > view.max.y) continue;
        const auto& account = accounts_[i];
        const bool active = account.xuid == session_.active_xuid;
        const bool selected = account.xuid == session_.selected_xuid;
        const SocialRect row{{view.min.x, top}, {view.max.x - scrollbar, top + height}};
        const auto id = id_row_first + static_cast<std::uint32_t>(i) * 2;
        GlassPlate plate;
        plate.accent = false;
        plate.fill = active ? theme::rgb(0xD6F7E3) : style::plate;
        plate.hover = motion(id, row, context.frame.dt).hover;
        draw_glass_plate(list, row.min, row.max, metrics::card_radius * s, plate);
        if (active) list->AddRectFilled({row.min.x + 3.0F * s, row.min.y + 12.0F * s},
            {row.min.x + 6.0F * s, row.max.y - 12.0F * s}, to_u32(theme::rgb(0x2FB873)), 2.0F * s);

        const float button_w = row_button_width * s, button_h = row_button_height * s;
        const float button_y = std::round((row.min.y + row.max.y - button_h) * 0.5F);
        const float inset = metrics::card_padding * s;
        const SocialRect remove{{row.max.x - inset - button_w, button_y}, {row.max.x - inset, button_y + button_h}};
        const SocialRect use{{remove.min.x - 8.0F * s - button_w, button_y}, {remove.min.x - 8.0F * s, button_y + button_h}};
        remove_rects_.push_back({account.xuid, remove});
        use_rects_.push_back({account.xuid, use});
        const bool enabled = context.channel && session_.available && !session_.busy && !stage_;
        draw_social_button(list, fonts, &context.shadows, use, active ? "In use" : selected ? "Selected" : "Use",
                           SocialButton::Normal, motion(id, use, context.frame.dt), enabled, s, 1.0F);
        if (input_.click(id, use) && enabled) {
            session_.busy = true;
            error_.clear();
            diagnostic_.clear();
            pending_use_id_ = ++next_use_id_;
            context.channel->commands.push(alt::UseAccount{account.xuid, pending_use_id_});
        }
        draw_social_button(list, fonts, &context.shadows, remove, "Remove", SocialButton::Danger,
                           motion(id + 1, remove, context.frame.dt), !selected && !session_.busy, s, 1.0F);
        if (input_.click(id + 1, remove) && !selected && !session_.busy) confirm_remove_ = account.xuid;

        const float name = fonts.line_height(SocialText::Title), small = fonts.line_height(SocialText::Small);
        const float text_y = (row.min.y + row.max.y - name - small) * 0.5F;
        fonts.draw(list, SocialText::Title, row.min.x + inset, text_y, to_u32(active ? theme::rgb(0x23975D) : style::heading),
                   fonts.ellipsize(SocialText::Title, account.gamertag, use.min.x - row.min.x - inset * 2.0F, scratch));
        fonts.draw(list, SocialText::Small, row.min.x + inset, text_y + name, to_u32(style::text_secondary),
                   "XUID " + account.xuid);
    }
    scroll_.end(list, input_, id_scroll, s, 1.0F);
}

void AltScreen::draw_sign_in(Context& context, const SocialRect& area, float s) {
    auto* list = context.list;
    const auto& fonts = context.fonts;
    if (user_code_.empty()) {
        draw_centered(list, fonts, SocialText::Heading, area, to_u32(style::heading), stage_text(*stage_));
        return;
    }
    const float pad = 24.0F * s, gap = 10.0F * s;
    const float button_h = metrics::button_height * s;
    const float width = std::min(area.width(), 600.0F * s);
    const float height = pad * 2.0F + fonts.line_height(SocialText::Heading) + fonts.line_height(SocialText::Display) +
                         fonts.line_height(SocialText::Small) * 3.0F + button_h * 2.0F + gap * 5.0F;
    const ImVec2 center{(area.min.x + area.max.x) * 0.5F, (area.min.y + area.max.y) * 0.5F};
    const SocialRect card{{center.x - width * 0.5F, center.y - height * 0.5F},
                          {center.x + width * 0.5F, center.y + height * 0.5F}};
    GlassPlate plate;
    plate.fill = style::plate;
    plate.accent = false;
    draw_glass_plate(list, card.min, card.max, metrics::card_radius * s, plate);
    const float x0 = card.min.x + pad, x1 = card.max.x - pad;
    float y = card.min.y + pad;
    const auto line = [&](SocialText role, std::string_view text, const Color& color) {
        const float h = fonts.line_height(role);
        draw_centered(list, fonts, role, {{x0, y}, {x1, y + h}}, to_u32(color), text);
        y += h + gap;
    };
    line(SocialText::Heading, "Microsoft sign-in", style::heading);
    code_rect_ = {{x0, y}, {x1, y + fonts.line_height(SocialText::Display)}};
    line(SocialText::Display, user_code_, style::heading);
    const char* message = copy_pending_ ? "Copying…" : code_copied_ ? "Code copied. Paste with Ctrl+V."
                                                                            : "Copy the code or enter it manually.";
    fonts.wrap(SocialText::Small, message, x1 - x0, wrapped_);
    for (const auto& part : wrapped_) line(SocialText::Small, std::string_view(message).substr(part.begin, part.end - part.begin),
                                          style::description);
    // Leave room for two wrapped status lines at small window sizes.
    y = std::max(y, code_rect_.max.y + gap + fonts.line_height(SocialText::Small) * 2.0F + gap);
    copy_code_rect_ = {{x0, y}, {x1, y + button_h}};
    draw_social_button(list, fonts, &context.shadows, copy_code_rect_, code_copied_ ? "Copy again" : "Copy code",
                       SocialButton::Normal, motion(id_copy_code, copy_code_rect_, context.frame.dt), !copy_pending_, s, 1.0F);
    if (input_.click(id_copy_code, copy_code_rect_) && !copy_pending_ && context.channel) {
        copy_pending_ = true;
        code_copied_ = false;
        context.channel->commands.push(alt::CopySignInCode{});
    }
    y += button_h + gap;
    open_sign_in_rect_ = {{x0, y}, {x1, y + button_h}};
    draw_social_button(list, fonts, &context.shadows, open_sign_in_rect_, "Open Microsoft", SocialButton::Normal,
                       motion(id_open_sign_in, open_sign_in_rect_, context.frame.dt), true, s, 1.0F);
    if (input_.click(id_open_sign_in, open_sign_in_rect_) && context.channel)
        context.channel->commands.push(alt::OpenSignInPage{});
}

void AltScreen::draw_sidebar(Context& context, const SocialRect& area, float s) {
    auto* list = context.list;
    const auto& fonts = context.fonts;
    const float radius = metrics::panel_radius * s;
    list->AddRectFilled(area.min, area.max, to_u32(style::sidebar_fill), radius, ImDrawFlags_RoundCornersRight);
    list->AddLine({area.min.x, area.min.y + radius}, {area.min.x, area.max.y - radius}, to_u32(style::divider),
                  std::max(1.0F, s));
    const float pad = metrics::content_padding * s * 0.7F;
    const float x0 = area.min.x + pad, x1 = area.max.x - pad;
    float y = area.min.y + metrics::content_padding * s + fonts.line_height(SocialText::Heading) + 12.0F * s;
    fonts.draw(list, SocialText::Small, x0, y, to_u32(style::text_secondary), "ADD ACCOUNT");
    y += fonts.line_height(SocialText::Small) + 8.0F * s;

    bool busy = stage_.has_value();
    microsoft_rect_ = {{x0, y}, {x1, y + microsoft_height * s}};
    draw_microsoft_button(list, fonts, &context.shadows, microsoft_rect_, motion(id_microsoft, microsoft_rect_, context.frame.dt),
                          !busy, s);
    if (input_.click(id_microsoft, microsoft_rect_) && !busy && context.channel) {
        stage_ = alt::SignInStage::Starting;
        error_.clear();
        diagnostic_.clear();
        added_.clear();
        user_code_.clear();
        code_copied_ = false;
        busy = true;
        context.channel->commands.push(alt::StartSignIn{});
    }
    y += microsoft_height * s + 12.0F * s;

    const auto paragraph = [&](SocialText role, std::string_view text, const Color& color) {
        fonts.wrap(role, text, x1 - x0, wrapped_);
        for (const auto& line : wrapped_) {
            fonts.draw(list, role, x0, y, to_u32(color), text.substr(line.begin, line.end - line.begin));
            y += fonts.line_height(role);
        }
        y += 8.0F * s;
    };
    if (busy) {
        paragraph(SocialText::Small, stage_text(*stage_), style::description);
        if (!user_code_.empty()) paragraph(SocialText::Small, "CODE " + user_code_, style::heading);
        if (!user_code_.empty() && !window_opened_)
            paragraph(SocialText::Small, "Open Microsoft, then paste this code.", style::description);
        cancel_rect_ = {{x0, y}, {x1, y + metrics::button_height * s}};
        draw_social_button(list, fonts, &context.shadows, cancel_rect_, "Cancel", SocialButton::Normal,
                           motion(id_cancel, cancel_rect_, context.frame.dt), true, s, 1.0F);
        if (input_.click(id_cancel, cancel_rect_) && context.channel) context.channel->commands.push(alt::CancelSignIn{});
    } else {
        cancel_rect_ = {};
        if (!error_.empty()) {
            paragraph(SocialText::Small, error_text(error_), style::error);
            if (!diagnostic_.empty()) paragraph(SocialText::Small, diagnostic_, style::text_secondary);
        }
        else if (!added_.empty()) paragraph(SocialText::Body, "Added " + added_, style::heading);
    }
}

void AltScreen::draw_confirm(Context& context, const SocialRect& panel, float s) {
    auto* list = context.list;
    const auto& fonts = context.fonts;
    const float radius = metrics::panel_radius * s;
    list->AddRectFilled(panel.min, panel.max, to_u32(style::dialog_veil), radius);
    const float w = 400.0F * s, h = 150.0F * s;
    const ImVec2 c{(panel.min.x + panel.max.x) * 0.5F, (panel.min.y + panel.max.y) * 0.5F};
    const SocialRect card{{c.x - w * 0.5F, c.y - h * 0.5F}, {c.x + w * 0.5F, c.y + h * 0.5F}};
    context.shadows.draw(list, card.min, card.max, CornerRadii::all(radius), panel_glow(s), 1.0F);
    list->AddRectFilled(card.min, card.max, to_u32(style::panel_fill), radius);
    list->AddRect(card.min, card.max, to_u32(style::panel_rim), radius);
    float y = card.min.y + 22.0F * s;
    fonts.draw(list, SocialText::Heading, card.min.x + 24.0F * s, y, to_u32(style::heading), "Remove this account?");
    y += fonts.line_height(SocialText::Heading) + 6.0F * s;
    const auto found = std::find_if(accounts_.begin(), accounts_.end(),
                                    [&](const alt::Account& account) { return account.xuid == confirm_remove_; });
    if (found != accounts_.end())
        fonts.draw(list, SocialText::Title, card.min.x + 24.0F * s, y, to_u32(style::description), found->gamertag);
    const float bw = 110.0F * s, bh = metrics::button_height * s;
    const SocialRect cancel{{card.max.x - 24.0F * s - bw, card.max.y - 20.0F * s - bh},
                            {card.max.x - 24.0F * s, card.max.y - 20.0F * s}};
    const SocialRect remove{{cancel.min.x - 10.0F * s - bw, cancel.min.y}, {cancel.min.x - 10.0F * s, cancel.max.y}};
    confirm_ok_rect_ = remove;
    draw_social_button(list, fonts, &context.shadows, remove, "Remove", SocialButton::Danger,
                       motion(id_confirm_ok, remove, context.frame.dt), true, s, 1.0F);
    draw_social_button(list, fonts, &context.shadows, cancel, "Cancel", SocialButton::Normal,
                       motion(id_confirm_cancel, cancel, context.frame.dt), true, s, 1.0F);
    if (input_.click(id_confirm_ok, remove)) {
        if (context.channel) context.channel->commands.push(alt::RemoveAccount{confirm_remove_});
        confirm_remove_.clear();
    } else if (input_.click(id_confirm_cancel, cancel)) {
        confirm_remove_.clear();
    }
}

void AltScreen::render(Context& context) {
    animation_.step(open_, context.frame.dt, style::timing::panel_open, style::timing::panel_close);
    if (!visible()) return;
    auto* list = context.list;
    const float p = animation_.value();
    const ImVec2 display{context.frame.client_w, context.frame.client_h};
    const float s = layout_scale(display);
    // The same backdrop as Social: the Main Menu's gradient, then the blurred back.png.
    draw_main_menu_backdrop(list, display, context.backdrop_time, p);
    context.background.draw(list, display, p);

    // A closing panel no longer reacts to the pointer.
    PointerInput pointer = open_ ? context.pointer : PointerInput{};
    const float panel_scale = metrics::panel_open_scale + (1.0F - metrics::panel_open_scale) * p;
    if (pointer.valid && panel_scale > 0.0F) {
        pointer.position.x = display.x * 0.5F + (pointer.position.x - display.x * 0.5F) / panel_scale;
        pointer.position.y = display.y * 0.5F + (pointer.position.y - display.y * 0.5F) / panel_scale;
    }
    const bool popup = !confirm_remove_.empty();
    input_.begin(pointer);
    input_.set_blocked(popup);

    const ImVec2 size = panel_size(display, false);
    const ImVec2 corner{std::round((display.x - size.x) * 0.5F), std::round((display.y - size.y) * 0.5F)};
    const SocialRect panel{corner, {corner.x + size.x, corner.y + size.y}};
    const int first_vertex = list->VtxBuffer.Size;
    list->AddDrawCmd();
    const int first_command = list->CmdBuffer.Size - 1;
    {
        const SocialFonts::Scope scope(context.fonts, list);
        const float radius = metrics::panel_radius * s;
        context.shadows.draw(list, panel.min, panel.max, CornerRadii::all(radius), panel_shadow(s), 1.0F);
        context.shadows.draw(list, panel.min, panel.max, CornerRadii::all(radius), panel_glow(s), 1.0F);
        list->AddRectFilled(panel.min, panel.max, to_u32(style::panel_fill), radius);
        const float sidebar = sidebar_width(size.x, s);
        // Mirrors Social: the picture on the left, the sidebar on the right.
        const SocialRect content{panel.min, {panel.max.x - sidebar, panel.max.y}};
        context.background.draw_panel(list, content.min, content.max, radius, ImDrawFlags_RoundCornersLeft, 1.0F);
        list->AddRect(panel.min, panel.max, to_u32(style::panel_rim), radius, 0, 1.0F);
        draw_list(context, content, s);
        draw_sidebar(context, {{content.max.x, panel.min.y}, panel.max}, s);

        const float close_size = metrics::close_size * s;
        const float inset = 12.0F * s;
        const SocialRect close_button{{panel.max.x - close_size - inset, panel.min.y + inset},
                                      {panel.max.x - inset, panel.min.y + inset + close_size}};
        const auto& close_motion = motion(id_close, close_button, context.frame.dt);
        if (close_motion.hover > 0.0F) {
            GlassPlate plate;
            plate.hover = close_motion.hover;
            plate.press = close_motion.press;
            plate.accent = false;
            plate.drop = false;
            plate.fill = style::plate;
            plate.alpha = close_motion.hover;
            draw_glass_plate(list, close_button.min, close_button.max, metrics::item_radius * s * 0.8F, plate);
        }
        const bool close_clicked = input_.click(id_close, close_button);
        const ImVec2 c{(close_button.min.x + close_button.max.x) * 0.5F, (close_button.min.y + close_button.max.y) * 0.5F};
        const float arm = close_size * 0.18F;
        const float thickness = std::max(1.0F, 1.8F * s);
        list->AddLine({c.x - arm, c.y - arm}, {c.x + arm, c.y + arm}, to_u32(style::icon), thickness);
        list->AddLine({c.x - arm, c.y + arm}, {c.x + arm, c.y - arm}, to_u32(style::icon), thickness);
        if (close_clicked) close();
        if (popup) {
            input_.set_blocked(false);
            draw_confirm(context, panel, s);
        }
    }
    input_.end();
    if (p < 1.0F) {
        scale_from(list, first_vertex, first_command, {corner.x + size.x * 0.5F, corner.y + size.y * 0.5F}, panel_scale, p);
    }
    // Whatever comes next (notifications) must not land in a command whose clip rectangle was scaled.
    list->AddDrawCmd();
}

} // namespace medicon::gui
