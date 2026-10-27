#include "Scenarios.hpp"

#include <medicon/core/ModuleManager.hpp>
#include <medicon/gui/GuiMath.hpp>
#include <medicon/gui/MainMenu.hpp>
#include <medicon/gui/Overlay.hpp>
#include <medicon/gui/SocialWidgets.hpp>
#include <medicon/gui/Theme.hpp>
#include <medicon/modules/ClientModules.hpp>
#include <medicon/social/Config.hpp>
#include <medicon/social/Events.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <stdexcept>
#include <thread>
#include <vector>

namespace medicon::preview {
namespace {

namespace mt = theme::metrics;
namespace gm = medicon::gui;

// Five ArrayList entries with varied widths, including a Japanese name.
const char* const arraylist_five[] = {"demo.fullbright", "demo.zoom", "demo.toggle_sprint", "demo.japanese", "demo.freelook"};

void set_enabled(ScenarioHost& host, const char* id, bool enabled) {
    if (Module* module = host.modules().find(id)) module->set_enabled(enabled);
}

ClickGuiModule* clickgui(ScenarioHost& host) {
    return dynamic_cast<ClickGuiModule*>(host.modules().find(ClickGuiModule::module_id));
}

void settle(ScenarioHost& host) {
    // Includes the 220 ms pulse trip and 1200 ms illumination decay.
    const int frames = static_cast<int>(std::ceil(1.6F / host.dt()));
    for (int i = 0; i < frames; ++i) host.frame();
    host.overlay().gui().finish_animations();
    host.frame();
    host.overlay().gui().finish_animations();
    host.frame();
}

void base(ScenarioHost& host, bool arraylist = true) {
    auto& gui = host.overlay().gui();
    gui.set_open(true);
    for (std::size_t i = 0; i < category_count; ++i) gui.set_panel_collapsed(static_cast<Category>(i), false);
    if (arraylist) for (const char* id : arraylist_five) set_enabled(host, id, true);
}

void log_panels(ScenarioHost& host) {
    for (const auto& rect : host.overlay().gui().panel_rects()) {
        char line[160];
        std::snprintf(line, sizeof line, "panel %s x=%.0f y=%.0f w=%.0f h=%.0f scale=%.3f", category_id(rect.category),
                      rect.x, rect.y, rect.w, rect.h, host.overlay().gui().ui_scale());
        host.log(line);
    }
}

// Captures `suffix` frames at the given times (s) after a trigger.
void timed_captures(ScenarioHost& host, bool capture, const std::function<void()>& trigger) {
    trigger();
    const float dt = host.dt();
    const std::pair<float, const char*> points[] = {{0.060F, "t060"}, {0.120F, "t120"}, {0.600F, "done"}};
    int frame = 0;
    for (const auto& [time, suffix] : points) {
        const int target = static_cast<int>(std::lround(time / dt));
        while (frame < target) {
            host.frame();
            ++frame;
        }
        char line[96];
        std::snprintf(line, sizeof line, "capture %s after %d frames (%.1f ms)", suffix, frame, frame * dt * 1000.0F);
        host.log(line);
        if (capture) host.capture(suffix);
    }
}

// Social notifications (spec 4.7) fed through a Channel exactly as the DLL's
// network threads do: three visible toasts and one queued behind them.
void toasts(ScenarioHost& host, bool capture, bool menu) {
    host.overlay().gui().set_open(false);
    host.overlay().set_main_menu_active(menu);
    for (const char* id : arraylist_five) set_enabled(host, id, true);
    static social::Channel channel;
    host.overlay().set_social(&channel, {});
    channel.events.push(social::SettingsLoaded{});
    const auto news = [](std::int64_t id, social::NewsKind kind, const char* title, std::optional<std::string> version) {
        return social::NewsPosted{social::NewsItem{id, kind, title, "Body", std::move(version), 0}};
    };
    channel.events.push(news(1, social::NewsKind::News, "Server maintenance on 10/28, 22:00 to 23:00", std::nullopt));
    channel.events.push(news(2, social::NewsKind::Changelog, "Added Medicon Social", "v0.2.0"));
    channel.events.push(social::StateChanged{social::ConnectionState::Replaced, {}});
    channel.events.push(news(3, social::NewsKind::News, "queued", std::nullopt));
    // The Social face loads on a worker thread; toasts wait for it without advancing.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!host.overlay().social().screen_fonts_ready()) {
        if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("Social font was not ready within 10 s");
        host.frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // Past the 200 ms entrance, with the stack settled.
    for (int i = 0; i < static_cast<int>(std::ceil(0.4F / host.dt())); ++i) host.frame();
    const auto& toasts = host.overlay().social().toasts();
    char line[96];
    std::snprintf(line, sizeof line, "toasts visible=%zu queued=%zu", toasts.visible().size(), toasts.queued());
    host.log(line);
    if (toasts.visible().size() != 3 || toasts.queued() != 1) throw std::runtime_error("unexpected toast stack");
    if (capture) host.capture("");
}

// Runs frames until `done` holds, in real time: the network answers on its own threads.
void wait_for(ScenarioHost& host, std::chrono::seconds limit, const char* what, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error(std::string("timed out waiting for ") + what);
        host.frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    host.log(std::string("milestone ") + what);
}

// End-to-end check against a running social-server (MediconPreview --social-dir ...
// --scenario social_live). An outside script posts news, restarts the server and
// revokes the key while this waits for each consequence.
void require_local_social_server() {
    if (social::server_url != "http://127.0.0.1:18787")
        throw std::runtime_error("Social E2E requires a separate build with MEDICON_SOCIAL_SERVER_URL=http://127.0.0.1:18787");
}

void social_live(ScenarioHost& host, bool capture) {
    require_local_social_server();
    auto& social = host.overlay().social();
    if (!social.attached()) throw std::runtime_error("social_live needs --social or --social-dir");
    host.overlay().gui().set_open(false);
    const auto& store = social.store();
    const auto online = [&] { return store.state() == social::ConnectionState::Online; };
    const auto showing = [&] { return !social.toasts().visible().empty() && social.screen_fonts_ready(); };
    const auto settle_toasts = [&] {
        for (int i = 0; i < static_cast<int>(std::ceil(0.4F / host.dt())); ++i) host.frame();
    };
    wait_for(host, std::chrono::seconds(30), "online", online);
    wait_for(host, std::chrono::seconds(90), "toast", showing);
    settle_toasts();
    if (capture) host.capture("toast");
    // Let the notification leave so the next one is the system toast.
    wait_for(host, std::chrono::seconds(30), "toast_gone", [&] { return social.toasts().empty(); });
    wait_for(host, std::chrono::seconds(90), "offline", [&] { return !online(); });
    wait_for(host, std::chrono::seconds(90), "reconnected", online);
    wait_for(host, std::chrono::seconds(90), "revoked", [&] {
        return store.state() == social::ConnectionState::AuthFailed && store.state_reason() == "revoked";
    });
    wait_for(host, std::chrono::seconds(10), "revoked_toast", showing);
    settle_toasts();
    if (capture) host.capture("revoked");
}

// These scenarios use the same Overlay and event queue as the live client, but
// never start a network service or use real credentials.
social::Channel& social_mock(ScenarioHost& host, bool online, bool open = true) {
    static social::Channel channel;
    host.overlay().gui().set_open(false);
    host.overlay().set_main_menu_active(true);
    host.overlay().set_social(&channel, {});
    channel.events.push(social::SettingsLoaded{});
    if (online) {
        channel.events.push(social::SessionStarted{social::UserRef{1, "Preview User"}, 0});
        channel.events.push(social::StateChanged{social::ConnectionState::Online, {}});
    }
    if (open) host.overlay().social().open_screen();
    return channel;
}

void social_settle(ScenarioHost& host) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::optional<std::chrono::steady_clock::time_point> font_ready_at;
    do {
        host.frame();
        const auto now = std::chrono::steady_clock::now();
        if (host.overlay().social().screen_fonts_ready() && !font_ready_at) font_ready_at = now;
        if (host.overlay().social().screen_resources_ready({host.width(), host.height()}) ||
            (font_ready_at && now - *font_ready_at >= std::chrono::seconds(2))) {
            if (!host.overlay().social().screen_resources_ready({host.width(), host.height()}))
                host.log("Social background not ready; rendering fallback");
            for (int i = 0; i < static_cast<int>(std::ceil(0.3F / host.dt())); ++i) host.frame();
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("Social fonts were not ready within 10 s");
}

void social_click(ScenarioHost& host, float x, float y) {
    const LPARAM point = MAKELPARAM(static_cast<WORD>(std::lround(x)), static_cast<WORD>(std::lround(y)));
    host.send(WM_MOUSEMOVE, 0, point); host.frame();
    host.send(WM_LBUTTONDOWN, MK_LBUTTON, point); host.frame();
    host.send(WM_LBUTTONUP, 0, point); host.frame();
}

void social_page(social::Channel& channel, social::NewsKind kind, int count) {
    social::NewsPage page;
    page.kind = kind;
    page.limit = 100;
    for (int i = 0; i < count; ++i) {
        page.items.push_back(social::NewsItem{1000 - i, kind,
            kind == social::NewsKind::News ? "Server maintenance tonight" : "Social screen update",
            "The servers restart at 22:00 for about an hour. Long lines wrap inside the card.\nLine breaks are kept.",
            kind == social::NewsKind::Changelog ? std::optional<std::string>{"v0.3.0"} : std::nullopt,
            1793059200000LL});
    }
    channel.events.push(std::move(page));
}

void social_key(ScenarioHost& host, UINT message, WPARAM key) {
    host.send(message, key, message == WM_KEYUP ? static_cast<LPARAM>(0xC0000001) : 1);
}

// Replaces the key field's text with `text` and presses Enter, as a user would.
void social_sign_in(ScenarioHost& host, const std::string& text) {
    social_key(host, WM_KEYDOWN, VK_CONTROL);
    social_key(host, WM_KEYDOWN, 'A');
    social_key(host, WM_KEYUP, 'A');
    social_key(host, WM_KEYUP, VK_CONTROL);
    host.frame();
    social_key(host, WM_KEYDOWN, VK_BACK);
    social_key(host, WM_KEYUP, VK_BACK);
    host.frame();
    for (const char c : text) host.send(WM_CHAR, static_cast<WPARAM>(static_cast<unsigned char>(c)), 1);
    host.frame();
    social_key(host, WM_KEYDOWN, VK_RETURN);
    social_key(host, WM_KEYUP, VK_RETURN);
    host.frame();
}

void check_newest_first(const std::vector<social::NewsItem>& items, const char* what) {
    for (std::size_t i = 1; i < items.size(); ++i) {
        if (items[i].id >= items[i - 1].id || items[i].created_at > items[i - 1].created_at)
            throw std::runtime_error(std::string(what) + " is not newest first");
    }
}

// Spec 5, phase 3 against a running social-server (tools/social-e2e.mjs drives
// the server side): sign in through the screen, read Home, change a setting,
// sign out and back in. The key comes from MEDICON_SOCIAL_TEST_KEY and is never
// written to the log.
void social_live_login(ScenarioHost& host, bool capture) {
    require_local_social_server();
    auto& social = host.overlay().social();
    if (!social.attached()) throw std::runtime_error("social_live_login needs --social-dir");
    const char* key_variable = std::getenv("MEDICON_SOCIAL_TEST_KEY");
    if (!key_variable || !*key_variable) throw std::runtime_error("set MEDICON_SOCIAL_TEST_KEY");
    const std::string key = key_variable;
    const auto& store = social.store();
    const auto& screen = social.screen();
    using social::ConnectionState;
    host.overlay().gui().set_open(false);
    host.overlay().set_main_menu_active(true);
    wait_for(host, std::chrono::seconds(30), "no_license", [&] { return store.state() == ConnectionState::NoLicense; });
    wait_for(host, std::chrono::seconds(10), "menu_prefetched", [&] {
        return social.screen_resources_ready({host.width(), host.height()});
    });
    // Through the Main Menu's Social button.
    const auto layout = gui::evaluate_main_menu({host.width(), host.height()}, 0.0);
    social_click(host, (layout.social.min.x + layout.social.max.x) * 0.5F, (layout.social.min.y + layout.social.max.y) * 0.5F);
    if (!screen.showing_login(store)) throw std::runtime_error("the Social button did not show the sign-in screen");
    social_settle(host);
    if (capture) host.capture("login");

    // The server starts only after the first attempt, so it cannot be reached.
    social_sign_in(host, key);
    wait_for(host, std::chrono::seconds(30), "login_unreachable", [&] { return screen.login_error() == "unreachable"; });
    if (capture) host.capture("unreachable");
    // A well-formed key the server does not know, retried until the server answers.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (screen.login_error() != "invalid_key") {
        if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("the server never answered");
        social_sign_in(host, "MDC-AAAA-BBBB-CCCC-DDDD");
        wait_for(host, std::chrono::seconds(30), "login_answered", [&] {
            return !screen.login_error().empty() || store.state() == ConnectionState::Online;
        });
    }
    host.log("milestone login_invalid_key");
    for (int i = 0; i < 10; ++i) host.frame();
    if (capture) host.capture("invalid_key");

    social_sign_in(host, key);
    wait_for(host, std::chrono::seconds(30), "online", [&] { return store.state() == ConnectionState::Online; });
    if (screen.showing_login(store) || screen.view() != gui::SocialView::Home) throw std::runtime_error("Home did not open");

    // Home: the first page (20), newest first, then the rest on scrolling.
    wait_for(host, std::chrono::seconds(30), "home_news", [&] {
        return store.news(social::NewsKind::News).size() >= 20 && !store.loading(social::NewsKind::News) &&
               social.screen_text_ready(gui::SocialText::Title, store.news(social::NewsKind::News).front().title);
    });
    check_newest_first(store.news(social::NewsKind::News), "News");
    for (int i = 0; i < 10; ++i) host.frame();
    if (capture) host.capture("home_news");
    const auto tab = screen.tab_rect(social::NewsKind::News);
    const LPARAM over_cards = MAKELPARAM(static_cast<WORD>(std::lround(tab.max.x + 200.0F)),
                                         static_cast<WORD>(std::lround(tab.max.y + 150.0F)));
    host.send(WM_MOUSEMOVE, 0, over_cards);
    wait_for(host, std::chrono::seconds(30), "news_all", [&] {
        host.send(WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA * 3), 0);
        return !store.has_more(social::NewsKind::News) && !store.loading(social::NewsKind::News);
    });
    char line[96];
    std::snprintf(line, sizeof line, "news loaded: %zu", store.news(social::NewsKind::News).size());
    host.log(line);
    check_newest_first(store.news(social::NewsKind::News), "News after paging");
    const auto changelog_tab = screen.tab_rect(social::NewsKind::Changelog);
    social_click(host, (changelog_tab.min.x + changelog_tab.max.x) * 0.5F, (changelog_tab.min.y + changelog_tab.max.y) * 0.5F);
    wait_for(host, std::chrono::seconds(30), "home_changelog", [&] {
        return !store.news(social::NewsKind::Changelog).empty() && !store.loading(social::NewsKind::Changelog) &&
               screen.news_tab() == social::NewsKind::Changelog;
    });
    check_newest_first(store.news(social::NewsKind::Changelog), "Change Log");
    for (int i = 0; i < 10; ++i) host.frame();
    if (capture) host.capture("home_changelog");

    // Settings: turning Sound off is written to social.json by the network thread.
    social.open_screen(gui::SocialView::Settings);
    for (int i = 0; i < 5; ++i) host.frame();
    const auto sound = screen.toggle_rect(4);
    social_click(host, (sound.min.x + sound.max.x) * 0.5F, (sound.min.y + sound.max.y) * 0.5F);
    if (store.notify().sound) throw std::runtime_error("the Sound toggle did not change");
    for (int i = 0; i < 30; ++i) host.frame();
    host.log("milestone sound_off");
    if (capture) host.capture("settings");

    // Sign out deletes license.bin and returns to the sign-in screen.
    const auto sign_out = screen.sign_out_rect();
    social_click(host, (sign_out.min.x + sign_out.max.x) * 0.5F, (sign_out.min.y + sign_out.max.y) * 0.5F);
    wait_for(host, std::chrono::seconds(30), "logged_out", [&] {
        return store.state() == ConnectionState::NoLicense && screen.showing_login(store);
    });
    // Let the driver look at the folder before signing in again.
    const auto pause = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < pause) host.frame();
    social_sign_in(host, key);
    wait_for(host, std::chrono::seconds(30), "online_again", [&] { return store.state() == ConnectionState::Online; });
}

void social_scenario(ScenarioHost& host, bool capture, const std::string& variant) {
    const bool online = variant == "home" || variant == "changelog" || variant == "empty" ||
                        variant == "many" || variant == "settings" || variant == "device";
    auto& channel = social_mock(host, online);
    if (variant == "error") channel.events.push(social::LoginFinished{false, "invalid_key"});
    if (variant == "network") channel.events.push(social::LoginFinished{false, "unreachable"});
    if (online) {
        social_page(channel, social::NewsKind::News, variant == "empty" ? 0 : (variant == "many" ? 60 : 3));
        social_page(channel, social::NewsKind::Changelog, variant == "empty" ? 0 : 2);
    }
    social_settle(host);
    if (variant == "success") {
        // Deliberately fictitious key: simulate typing and have the mock service
        // answer only after a real Login command is emitted by the view.
        for (const wchar_t ch : std::wstring(L"MDC-TEST-TEST-TEST-TEST")) host.send(WM_CHAR, ch, 1);
        host.frame();
        host.send(WM_KEYDOWN, VK_RETURN, 1); host.frame();
        host.send(WM_KEYUP, VK_RETURN, static_cast<LPARAM>(0xC0000001)); host.frame();
        auto command = channel.commands.pop(std::chrono::steady_clock::now());
        if (!command || !std::holds_alternative<social::Login>(*command))
            throw std::runtime_error("Social mock login did not emit a Login command");
        channel.events.push(social::LoginFinished{true, {}});
        channel.events.push(social::SessionStarted{social::UserRef{1, "Preview User"}, 0});
        channel.events.push(social::StateChanged{social::ConnectionState::Online, {}});
        social_page(channel, social::NewsKind::News, 3);
        social_page(channel, social::NewsKind::Changelog, 2);
        wait_for(host, std::chrono::seconds(10), "social_login_glyphs", [&] {
            return host.overlay().social().screen_text_ready(gui::SocialText::Title, "Server maintenance tonight");
        });
    } else if (variant == "changelog") {
        // Click the tab where the screen drew it.
        const auto tab = host.overlay().social().screen().tab_rect(social::NewsKind::Changelog);
        social_click(host, (tab.min.x + tab.max.x) * 0.5F, (tab.min.y + tab.max.y) * 0.5F);
        if (host.overlay().social().screen().news_tab() != social::NewsKind::Changelog)
            throw std::runtime_error("the Change Log tab did not take the click");
    } else if (variant == "settings") {
        host.overlay().social().open_screen(gui::SocialView::Settings);
    } else if (variant == "device") {
        host.recreate_graphics();
        social_settle(host);
    }
    for (int i = 0; i < static_cast<int>(std::ceil(0.3F / host.dt())); ++i) host.frame();
    if (capture) host.capture("");
}

// Frames up to `seconds` after a trigger, captured at each listed time.
void capture_at(ScenarioHost& host, bool capture, const char* prefix, std::initializer_list<int> milliseconds) {
    int frame = 0;
    for (const int ms : milliseconds) {
        const int target = static_cast<int>(std::lround(ms / 1000.0F / host.dt()));
        while (frame < target) {
            host.frame();
            ++frame;
        }
        char suffix[32], line[96];
        std::snprintf(suffix, sizeof suffix, "%s%03d", prefix, ms);
        std::snprintf(line, sizeof line, "capture %s at %.1f ms", suffix, frame * host.dt() * 1000.0F);
        host.log(line);
        if (capture) host.capture(suffix);
    }
}

// 220 ms to open (0.96 -> 1, fading in, shadow and glow with it), 140 ms to close.
// Run with --fixed-dt 0.01 so every capture lands on its time exactly.
void social_animation(ScenarioHost& host, bool capture) {
    auto& channel = social_mock(host, true, false);
    social_page(channel, social::NewsKind::News, 3);
    social_page(channel, social::NewsKind::Changelog, 2);
    // Prefetched in the menu, as in the client.
    wait_for(host, std::chrono::seconds(10), "social_prefetched", [&] {
        return host.overlay().social().screen_resources_ready({host.width(), host.height()}) &&
               host.overlay().social().screen_text_ready(gui::SocialText::Title, "Server maintenance tonight");
    });
    host.overlay().social().open_screen();
    capture_at(host, capture, "open", {0, 70, 140, 220});
    host.overlay().social().key_event(VK_ESCAPE, true);
    capture_at(host, capture, "close", {0, 50, 90, 140});
}

// The Social button in the Main Menu, optionally with its badge.
void main_menu_social(ScenarioHost& host, bool capture, bool badge) {
    social_mock(host, true, false);
    host.overlay().social().preview_badge(badge);
    for (int i = 0; i < static_cast<int>(std::ceil(0.5F / host.dt())); ++i) host.frame();
    if (capture) host.capture("");
}

// A click on the Social button opens the screen, which then hides the menu.
void main_menu_social_click(ScenarioHost& host, bool capture) {
    social_mock(host, true, false);
    for (int i = 0; i < static_cast<int>(std::ceil(0.5F / host.dt())); ++i) host.frame();
    const auto layout = gui::evaluate_main_menu({host.width(), host.height()}, 0.0);
    social_click(host, (layout.social.min.x + layout.social.max.x) * 0.5F, (layout.social.min.y + layout.social.max.y) * 0.5F);
    if (!host.overlay().social().screen_open()) throw std::runtime_error("the Social button did not open the screen");
    if (host.overlay().main_menu().consume_action()) throw std::runtime_error("the Social click reached the game");
    social_settle(host);
    host.log(host.overlay().social().screen_opaque() ? "screen opaque; menu hidden" : "screen not opaque");
    if (capture) host.capture("");
}

// The sign-in card's live states: the focused field's glow, then the Sign in
// button hovered (glow, pink accent) and held (0.98).
void social_login_states(ScenarioHost& host, bool capture) {
    social_mock(host, false);
    social_settle(host);
    for (const wchar_t ch : std::wstring(L"MDC-ABCD")) host.send(WM_CHAR, ch, 1);
    for (int i = 0; i < 20; ++i) host.frame();
    if (capture) host.capture("focus");
    const auto button = host.overlay().social().screen().sign_in_rect();
    const LPARAM point = MAKELPARAM(static_cast<WORD>(std::lround((button.min.x + button.max.x) * 0.5F)),
                                    static_cast<WORD>(std::lround((button.min.y + button.max.y) * 0.5F)));
    host.send(WM_MOUSEMOVE, 0, point);
    for (int i = 0; i < 20; ++i) host.frame();
    if (capture) host.capture("hover");
    host.send(WM_LBUTTONDOWN, MK_LBUTTON, point);
    for (int i = 0; i < 10; ++i) host.frame();
    if (capture) host.capture("pressed");
}

// --- Chat, friends and DM (phases 4-6), all through the mock channel -----------

social::ChatMessage chat_message(std::int64_t id, std::int64_t author, const char* name, const char* body,
                                 std::int64_t minutes, std::int64_t to = 0) {
    // 2026-10-27 21:00 local-ish, a minute per step.
    // mochi (4) is an owner: Global shows the OWNER tag after the name.
    return social::ChatMessage{id, social::UserRef{author, name, author == 4 ? social::Rank::Owner : social::Rank::User}, body,
                               1793124000000LL + minutes * 60000, to};
}

void push_friends(social::Channel& channel) {
    social::FriendList list;
    list.friends = {social::Friend{social::UserRef{2, "alice"}, true, 2, 0},
                    social::Friend{social::UserRef{3, "bob"}, false, 0, 0},
                    social::Friend{social::UserRef{4, "mochi"}, true, 0, 0}};
    list.incoming = {social::UserRef{5, "cat"}};
    list.outgoing = {social::UserRef{6, "dave"}};
    channel.events.push(social::FriendsLoaded{list, false});
}

void type_text(ScenarioHost& host, const std::wstring& text) {
    for (const wchar_t c : text) host.send(WM_CHAR, c, 1);
    host.frame();
}

void press_enter(ScenarioHost& host) {
    host.send(WM_KEYDOWN, VK_RETURN, 1);
    host.frame();
    host.send(WM_KEYUP, VK_RETURN, static_cast<LPARAM>(0xC0000001));
    host.frame();
}

void wait_frames(ScenarioHost& host, float seconds) {
    for (int i = 0; i < static_cast<int>(std::ceil(seconds / host.dt())); ++i) host.frame();
}

// Global: grouped names and times, Japanese, my pending and failed messages,
// and the error above the input (spec 4.6).
void social_global(ScenarioHost& host, bool capture) {
    auto& channel = social_mock(host, true, false);
    push_friends(channel);
    social::MessagePage page;
    page.conversation = social::global_conversation;
    page.limit = 50;
    page.items = {chat_message(9, 1, "Preview User", "same here, see you in the lobby", 9),
                  chat_message(8, 2, "alice", "anyone up for bedwars tonight?", 8),
                  chat_message(7, 2, "alice", "the new menu looks so cute", 7),
                  chat_message(6, 3, "bob", "こんばんは！今日のアップデート来てる", 4),
                  chat_message(5, 4, "mochi", "おつかれさまです", 2),
                  chat_message(4, 4, "mochi", "Social の通知、ちゃんと届いてます", 1)};
    channel.events.push(std::move(page));
    host.overlay().social().open_screen(gui::SocialView::Global);
    social_settle(host);
    // Typed and sent: pending at 50 % until an answer arrives.
    type_text(host, L"今から入るね");
    press_enter(host);
    auto command = channel.commands.pop(std::chrono::steady_clock::now());
    const auto* send = command ? std::get_if<social::SendChat>(&*command) : nullptr;
    if (!send || send->conversation != social::global_conversation) throw std::runtime_error("Enter did not send to Global");
    // A second one too soon is refused: it fails with "click to retry" and the
    // 2 s note above the input.
    type_text(host, L"wait for me");
    press_enter(host);
    command = channel.commands.pop(std::chrono::steady_clock::now());
    const auto* second = command ? std::get_if<social::SendChat>(&*command) : nullptr;
    if (!second) throw std::runtime_error("the second message was not sent");
    channel.events.push(social::RequestFinished{second->nonce, false, "rate_limited", std::nullopt});
    type_text(host, L"もう一回");
    wait_frames(host, 0.3F);
    if (capture) host.capture("");
}

// A DM: bubbles (mine right, blue), "Read" under my last read message, and the
// sidebar with online dots, unread badges and the request badge on +.
void social_dm(ScenarioHost& host, bool capture) {
    auto& channel = social_mock(host, true, false);
    push_friends(channel);
    social::MessagePage page;
    page.conversation = 3;
    page.limit = 50;
    page.items = {chat_message(26, 3, "bob", "今日いける？", 12, 1),
                  chat_message(25, 1, "Preview User", "ok! 21時からなら", 11, 3),
                  chat_message(24, 1, "Preview User", "loaded the new client, the Social screen is nice", 10, 3),
                  chat_message(23, 3, "bob", "did you see the patch notes?", 6, 1),
                  chat_message(22, 3, "bob", "hey", 5, 1)};
    channel.events.push(std::move(page));
    channel.events.push(social::ReadBy{3, 24});
    host.overlay().social().open_screen();
    host.overlay().social().screen().open_dm(3);
    social_settle(host);
    wait_frames(host, 0.3F);
    if (capture) host.capture("");
}

// Friends (+): search 300 ms after typing, requests and sent requests (spec 4.6).
void social_friends(ScenarioHost& host, bool capture) {
    auto& channel = social_mock(host, true, false);
    push_friends(channel);
    host.overlay().social().open_screen(gui::SocialView::Friends);
    social_settle(host);
    const auto field = host.overlay().social().screen().search_rect();
    social_click(host, (field.min.x + field.max.x) * 0.5F, (field.min.y + field.max.y) * 0.5F);
    type_text(host, L"al");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::optional<social::ApiRequest> request;
    while (!request || !std::holds_alternative<social::SearchUsers>(*request)) {
        if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("no search was sent");
        host.frame();
        request = channel.api.pop(std::chrono::steady_clock::now());
    }
    if (std::get<social::SearchUsers>(*request).query != "al") throw std::runtime_error("searched for the wrong text");
    channel.events.push(social::SearchFinished{"al", {{social::UserRef{2, "alice"}, social::Relation::Friend},
                                                      {social::UserRef{7, "alfred"}, social::Relation::None},
                                                      {social::UserRef{8, "alma"}, social::Relation::Outgoing}}, false});
    wait_frames(host, 0.3F);
    if (capture) host.capture("");
}

// Right-click on a friend, "Remove friend", then the confirmation.
void social_friend_remove(ScenarioHost& host, bool capture) {
    auto& channel = social_mock(host, true, false);
    push_friends(channel);
    host.overlay().social().open_screen();
    social_settle(host);
    const auto row = host.overlay().social().screen().friend_rect(3);
    const LPARAM point = MAKELPARAM(static_cast<WORD>(std::lround((row.min.x + row.max.x) * 0.5F)),
                                    static_cast<WORD>(std::lround((row.min.y + row.max.y) * 0.5F)));
    host.send(WM_MOUSEMOVE, 0, point); host.frame();
    host.send(WM_RBUTTONDOWN, MK_RBUTTON, point); host.frame();
    host.send(WM_RBUTTONUP, 0, point); host.frame();
    wait_frames(host, 0.2F);
    if (capture) host.capture("menu");
    // The menu opens at the pointer; its item is just below and to the right.
    social_click(host, (row.min.x + row.max.x) * 0.5F + 30.0F, (row.min.y + row.max.y) * 0.5F + 12.0F);
    wait_frames(host, 0.3F);
    if (capture) host.capture("confirm");
}

// Phases 4-6 against a running social-server: tools/social-chat-e2e.mjs plays
// "bob", "cat" and "eve" over the WebSocket and answers each milestone below.
// The key comes from MEDICON_SOCIAL_TEST_KEY and is never written to the log.
void social_live_chat(ScenarioHost& host, bool capture) {
    auto& social = host.overlay().social();
    if (!social.attached()) throw std::runtime_error("social_live_chat needs --social-dir");
    const char* key_variable = std::getenv("MEDICON_SOCIAL_TEST_KEY");
    if (!key_variable || !*key_variable) throw std::runtime_error("set MEDICON_SOCIAL_TEST_KEY");
    const auto& store = social.store();
    auto& screen = social.screen();
    const auto limit = std::chrono::seconds(60);
    using social::ConnectionState;
    const auto click_rect = [&](const gui::SocialRect& rect) {
        if (rect.width() <= 0.0F) throw std::runtime_error("a widget to click was not drawn");
        social_click(host, (rect.min.x + rect.max.x) * 0.5F, (rect.min.y + rect.max.y) * 0.5F);
    };
    const auto friend_id = [&](const char* name) -> std::int64_t {
        for (const auto& entry : store.friends())
            if (entry.user.name == name) return entry.user.id;
        return 0;
    };
    const auto last_mine = [&](social::ConversationId id) -> const social::ChatEntry* {
        const auto& entries = store.conversation(id).entries;
        const auto me = store.me() ? store.me()->id : 0;
        for (auto it = entries.rbegin(); it != entries.rend(); ++it)
            if (it->message.author.id == me) return &*it;
        return nullptr;
    };
    const auto has_body = [&](social::ConversationId id, std::string_view body) {
        for (const auto& entry : store.conversation(id).entries)
            if (entry.message.body == body && entry.message.id != 0) return true;
        return false;
    };

    host.overlay().gui().set_open(false);
    host.overlay().set_main_menu_active(true);
    wait_for(host, limit, "no_license", [&] { return store.state() == ConnectionState::NoLicense; });
    social.open_screen();
    social_settle(host);
    social_sign_in(host, key_variable);
    wait_for(host, limit, "online", [&] { return store.state() == ConnectionState::Online; });

    // --- Phase 4: Global ------------------------------------------------------
    social.open_screen(gui::SocialView::Global);
    wait_for(host, limit, "global_loaded", [&] {
        return store.conversation(social::global_conversation).loaded && !store.conversation(social::global_conversation).loading;
    });
    // Scrolling to the top loads older messages until there are none.
    host.send(WM_MOUSEMOVE, 0, MAKELPARAM(static_cast<WORD>(host.width() * 0.6F), static_cast<WORD>(host.height() * 0.5F)));
    wait_for(host, limit, "global_history", [&] {
        host.send(WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA * 5), 0);
        const auto& global = store.conversation(social::global_conversation);
        return !global.has_more && !global.loading;
    });
    char line[96];
    std::snprintf(line, sizeof line, "global messages loaded: %zu", store.conversation(social::global_conversation).entries.size());
    host.log(line);
    type_text(host, L"こんにちは、DLL から");
    press_enter(host);
    try {
        wait_for(host, std::chrono::seconds(15), "global_sent", [&] {
            const auto* mine = last_mine(social::global_conversation);
            return mine && mine->status == social::ChatEntry::Status::Sent && mine->message.id != 0;
        });
    } catch (...) {
        // Status and error code only, never the text.
        const auto* mine = last_mine(social::global_conversation);
        const auto* focused = screen.focused_input();
        std::snprintf(line, sizeof line, "global send: entry=%s status=%d error=%s focused=%s length=%zu", mine ? "yes" : "no",
                      mine ? static_cast<int>(mine->status) : -1, mine ? mine->error.c_str() : "-", focused ? "yes" : "no",
                      focused ? focused->length() : 0);
        host.log(line);
        throw;
    }
    wait_for(host, limit, "global_received", [&] { return has_body(social::global_conversation, "テストクライアントから送信しました"); });
    // Two at once: the second is refused (0.3 s limit) and says so.
    type_text(host, L"one");
    press_enter(host);
    type_text(host, L"two");
    press_enter(host);
    wait_for(host, limit, "global_rate_limited", [&] {
        const auto* mine = last_mine(social::global_conversation);
        return mine && mine->status == social::ChatEntry::Status::Failed && mine->error == "rate_limited";
    });
    wait_frames(host, 0.3F);
    if (capture) host.capture("global");

    // --- Phase 5: friends -----------------------------------------------------
    wait_for(host, limit, "friend_request_seen", [&] {
        return !store.incoming().empty() && store.incoming().front().name == "bob" && !social.toasts().visible().empty();
    });
    social.open_screen(gui::SocialView::Friends);
    wait_frames(host, 0.3F);
    click_rect(screen.user_button_rect(store.incoming().front().id, "Accept"));
    wait_for(host, limit, "friend_accepted", [&] { return friend_id("bob") != 0; });
    click_rect(screen.search_rect());
    type_text(host, L"cat");
    wait_for(host, limit, "cat_found", [&] { return !store.search_results().empty() && store.search_query() == "cat"; });
    wait_frames(host, 0.2F);
    click_rect(screen.user_button_rect(store.search_results().front().user.id, "Add"));
    wait_for(host, limit, "cat_requested", [&] { return !store.outgoing().empty(); });
    wait_for(host, limit, "mutual_friends", [&] { return friend_id("cat") != 0 && store.outgoing().empty(); });
    wait_frames(host, 0.3F);
    if (capture) host.capture("friends");

    // --- Phase 6: DM -----------------------------------------------------------
    const auto bob = friend_id("bob");
    social.open_screen(gui::SocialView::Global);
    wait_for(host, limit, "dm_unread", [&] {
        const auto* entry = store.find_friend(bob);
        bool toast = false;
        for (const auto& visible : social.toasts().visible()) toast = toast || visible.toast.kind == gui::ToastKind::Dm;
        return entry && entry->unread >= 2 && toast && social.badge();
    });
    wait_frames(host, 0.2F);
    if (capture) host.capture("unread");
    click_rect(screen.friend_rect(bob));
    wait_for(host, limit, "dm_opened", [&] {
        const auto* entry = store.find_friend(bob);
        return entry && entry->unread == 0 && store.conversation(bob).loaded;
    });
    type_text(host, L"了解！21時ね");
    press_enter(host);
    wait_for(host, limit, "dm_sent", [&] {
        const auto* mine = last_mine(bob);
        return mine && mine->status == social::ChatEntry::Status::Sent && mine->message.id != 0;
    });
    wait_for(host, limit, "dm_read_shown", [&] {
        const auto* mine = last_mine(bob);
        const auto* entry = store.find_friend(bob);
        return mine && entry && entry->their_read_up_to >= mine->message.id;
    });
    // While bob's DM is open his new message notifies nothing (spec 4.7).
    const auto before = store.conversation(bob).entries.size();
    // Notifications already on screen (from before the DM was opened) do not count.
    std::uint64_t newest_toast = 0;
    for (const auto& visible : social.toasts().visible()) newest_toast = std::max(newest_toast, visible.id);
    wait_for(host, limit, "dm_live", [&] { return store.conversation(bob).entries.size() > before; });
    wait_frames(host, 1.0F);
    for (const auto& visible : social.toasts().visible())
        if (visible.id > newest_toast && visible.toast.kind == gui::ToastKind::Dm) throw std::runtime_error("the open DM notified");
    if (social.toasts().queued() > 0) throw std::runtime_error("the open DM queued a notification");
    host.log("milestone dm_no_toast");
    if (capture) host.capture("dm");
}

// Owners: the Admin tab with a Change Log and its live preview, the warning on
// Post, and the post itself only after the warning's Post.
void social_admin(ScenarioHost& host, bool capture) {
    auto& channel = social_mock(host, true, false);
    channel.events.push(social::SessionStarted{social::UserRef{1, "Preview User", social::Rank::Owner}, 0});
    push_friends(channel);
    auto& screen = host.overlay().social().screen();
    host.overlay().social().open_screen();
    screen.open_admin();
    screen.set_admin_kind(social::NewsKind::Changelog);
    screen.admin_title().set_text(L"Medicon 1.2");
    screen.admin_version().set_text(L"v1.2.0");
    screen.admin_body().set_text(L"+ Added Killaura\n- Removed antistaff\n* Fixed auth\n+ Added the Admin tab");
    social_settle(host);
    wait_frames(host, 0.5F);
    if (!screen.admin_tab()) throw std::runtime_error("the Admin tab is not shown to an owner");
    if (capture) host.capture("form");
    const auto post = screen.admin_post_rect();
    social_click(host, (post.min.x + post.max.x) * 0.5F, (post.min.y + post.max.y) * 0.5F);
    wait_frames(host, 0.3F);
    if (!screen.confirming_post()) throw std::runtime_error("Post did not ask first");
    if (const auto early = channel.commands.pop(std::chrono::steady_clock::now());
        early && std::holds_alternative<social::PostNews>(*early))
        throw std::runtime_error("Post sent before the warning was accepted");
    if (capture) host.capture("confirm");
    const auto ok = screen.confirm_ok_rect();
    social_click(host, (ok.min.x + ok.max.x) * 0.5F, (ok.min.y + ok.max.y) * 0.5F);
    auto command = channel.commands.pop(std::chrono::steady_clock::now());
    const auto* posted = command ? std::get_if<social::PostNews>(&*command) : nullptr;
    if (!posted || posted->kind != social::NewsKind::Changelog || posted->version != "v1.2.0" ||
        posted->body.find("- Removed antistaff") == std::string::npos)
        throw std::runtime_error("the warning's Post did not send the Change Log");
    channel.events.push(social::RequestFinished{posted->nonce, true, {}, std::nullopt});
    wait_frames(host, 0.2F);
    if (screen.admin_status() != "Posted") throw std::runtime_error("no confirmation after posting");
    host.log("milestone admin_posted");
}

// Typing follows the clicked Admin field. Title used to keep every key once used.
void social_admin_focus(ScenarioHost& host, bool capture) {
    auto& channel = social_mock(host, true, false);
    channel.events.push(social::SessionStarted{social::UserRef{1, "Preview User", social::Rank::Owner}, 0});
    auto& screen = host.overlay().social().screen();
    host.overlay().social().open_screen();
    screen.open_admin();
    screen.set_admin_kind(social::NewsKind::Changelog);
    social_settle(host);
    wait_frames(host, 0.5F);
    if (!screen.admin_tab()) throw std::runtime_error("the Admin tab is not shown to an owner");
    const auto type_into = [&](std::size_t field, const std::wstring& text) {
        const auto rect = screen.admin_field_rect(field);
        social_click(host, (rect.min.x + rect.max.x) * 0.5F, (rect.min.y + rect.max.y) * 0.5F);
        type_text(host, text);
    };
    type_into(0, L"T");
    type_into(1, L"V");
    type_into(2, L"B");
    type_into(0, L"t");
    if (screen.admin_title().text() != L"Tt" || screen.admin_version().text() != L"V" ||
        screen.admin_body().text() != L"B")
        throw std::runtime_error("typing did not follow the clicked Admin field");
    // News hides the version field; its keys go nowhere unseen.
    type_into(1, L"2");
    screen.set_admin_kind(social::NewsKind::News);
    wait_frames(host, 0.1F);
    type_text(host, L"x");
    if (screen.admin_version().text() != L"V2" || screen.focused_input() == &screen.admin_version())
        throw std::runtime_error("the hidden version field kept the keyboard");
    if (capture) host.capture("");
    host.log("milestone admin_focus");
}

// The injected client's startup sign-in: after the intro, no key means the
// sign-in panel, which Esc cannot close; an accepted key goes to the Main Menu
// and greets with "Welcome to {name}".
void social_sign_in_gate(ScenarioHost& host, bool capture) {
    auto& channel = social_mock(host, false, false);
    auto& ui = host.overlay().social();
    ui.set_startup_sign_in(true);
    channel.events.push(social::StateChanged{social::ConnectionState::NoLicense, {}});
    social_settle(host);
    if (!ui.screen_open() || !ui.sign_in_gate_active() || !ui.screen().showing_login(ui.store()))
        throw std::runtime_error("no sign-in panel without a key");
    host.send(WM_KEYDOWN, VK_ESCAPE, 1); host.frame();
    host.send(WM_KEYUP, VK_ESCAPE, static_cast<LPARAM>(0xC0000001)); host.frame();
    host.overlay().set_main_menu_active(false);
    host.overlay().set_main_menu_active(true);
    host.frame();
    if (!ui.screen_open()) throw std::runtime_error("the sign-in gate was closed without a key");
    if (capture) host.capture("gate");
    type_text(host, L"MDC-TEST-TEST-TEST-TEST");
    press_enter(host);
    auto command = channel.commands.pop(std::chrono::steady_clock::now());
    if (!command || !std::holds_alternative<social::Login>(*command)) throw std::runtime_error("the gate did not sign in");
    channel.events.push(social::LoginFinished{true, {}});
    channel.events.push(social::StateChanged{social::ConnectionState::Connecting, {}});
    channel.events.push(social::SessionStarted{social::UserRef{1, "Preview User"}, 0});
    channel.events.push(social::StateChanged{social::ConnectionState::Online, {}});
    wait_frames(host, 0.5F);
    if (ui.screen_open() || ui.sign_in_gate_active() || !host.overlay().main_menu().active())
        throw std::runtime_error("an accepted key did not go to the Main Menu");
    const auto welcomed = [&] {
        for (const auto& entry : ui.toasts().visible())
            if (entry.toast.body == "Welcome to Preview User") return true;
        return false;
    };
    if (!welcomed()) throw std::runtime_error("no Welcome notification after signing in");
    if (ui.toasts().visible().size() != 1)
        throw std::runtime_error("only the Welcome notification shows without a loader update");
    // A reconnect is not a new sign-in.
    wait_frames(host, 5.0F);
    channel.events.push(social::StateChanged{social::ConnectionState::Reconnecting, {}});
    channel.events.push(social::SessionStarted{social::UserRef{1, "Preview User"}, 0});
    channel.events.push(social::StateChanged{social::ConnectionState::Online, {}});
    wait_frames(host, 0.5F);
    if (welcomed() || ui.screen_open()) throw std::runtime_error("a reconnect greeted again or gated");
    if (capture) host.capture("menu");
    host.log("milestone sign_in_gate");
}

// With a loader update, signing in also says "Updated to {version}".
void social_update_notice(ScenarioHost& host, bool capture) {
    auto& channel = social_mock(host, false, false);
    auto& ui = host.overlay().social();
    ui.set_startup_sign_in(true);
    ui.set_update_notice("1.0.5");
    channel.events.push(social::StateChanged{social::ConnectionState::NoLicense, {}});
    social_settle(host);
    type_text(host, L"MDC-TEST-TEST-TEST-TEST");
    press_enter(host);
    channel.commands.pop(std::chrono::steady_clock::now());
    channel.events.push(social::LoginFinished{true, {}});
    channel.events.push(social::SessionStarted{social::UserRef{1, "Preview User"}, 0});
    channel.events.push(social::StateChanged{social::ConnectionState::Online, {}});
    wait_frames(host, 0.5F);
    const auto has = [&](const std::string& body) {
        for (const auto& entry : ui.toasts().visible()) if (entry.toast.body == body) return true;
        return false;
    };
    if (!has("Welcome to Preview User") || !has("Updated to 1.0.5"))
        throw std::runtime_error("sign-in after a loader update did not notify");
    // A reconnect does not repeat the update notice.
    wait_frames(host, 5.0F);
    channel.events.push(social::StateChanged{social::ConnectionState::Reconnecting, {}});
    channel.events.push(social::SessionStarted{social::UserRef{1, "Preview User"}, 0});
    channel.events.push(social::StateChanged{social::ConnectionState::Online, {}});
    wait_frames(host, 0.5F);
    if (has("Updated to 1.0.5")) throw std::runtime_error("the update notice repeated on reconnect");
    if (capture) host.capture("");
    host.log("milestone update_notice");
}

// A Change Log whose lines start with +, - and *, shown with coloured marks.
void social_changelog_marks(ScenarioHost& host, bool capture) {
    auto& channel = social_mock(host, true, false);
    social::NewsPage page;
    page.kind = social::NewsKind::Changelog;
    page.limit = 100;
    page.items.push_back(social::NewsItem{3, social::NewsKind::Changelog, "Medicon 1.2",
        "+ Added Killaura\n- Removed antistaff\n* Fixed auth\n+ Added owner ranks and the Admin tab", "v1.2.0", 1793124000000LL});
    page.items.push_back(social::NewsItem{2, social::NewsKind::Changelog, "Medicon 1.1",
        "+ Added Global chat, friends and DM\n* Fixed typing Japanese while the IME is on", "v1.1.0", 1793037600000LL});
    channel.events.push(std::move(page));
    host.overlay().social().open_news(social::NewsKind::Changelog);
    social_settle(host);
    wait_frames(host, 0.3F);
    if (capture) host.capture("");
}

// Spec 4.7: a click on a notification in a menu opens what it is about.
void toasts_click(ScenarioHost& host, bool capture) {
    auto& channel = social_mock(host, true, false);
    social::NewsItem item{7, social::NewsKind::Changelog, "Added Medicon Social", "Body", "v0.2.0", 0};
    channel.events.push(social::NewsPosted{item});
    wait_for(host, std::chrono::seconds(10), "toast_shown", [&] {
        return !host.overlay().social().toasts().visible().empty() && host.overlay().social().screen_fonts_ready();
    });
    for (int i = 0; i < static_cast<int>(std::ceil(0.3F / host.dt())); ++i) host.frame();
    const float s = host.height() / 1080.0F;
    const float top = theme::social::metrics::toast_top * s;
    social_click(host, host.width() * 0.5F, top + theme::social::metrics::toast_height * s * 0.5F);
    const auto& screen = host.overlay().social().screen();
    if (!screen.opened() || screen.view() != gui::SocialView::Home || screen.news_tab() != social::NewsKind::Changelog)
        throw std::runtime_error("the Change Log notification did not open the Change Log tab");
    host.log("toast click opened Change Log");
    social_settle(host);
    if (capture) host.capture("");
}

// The typing animation on the license key field, on a clock that follows the
// Preview's fixed frame time: one character, then 15 characters a second.
double preview_clock = 1000.0;
void social_typing(ScenarioHost& host, bool capture) {
    social_mock(host, false);
    social_settle(host);
    preview_clock = 1000.0;
    gui::SocialTextInput::set_clock([] { return preview_clock; });
    host.set_frame_hook([&host] { preview_clock += host.dt(); });
    host.send(WM_CHAR, L'M', 1);
    capture_at(host, capture, "char", {0, 30, 60, 120, 180, 300});
    // 15 a second: a new character every 67 ms while the earlier ones still move.
    int frame = 0;
    for (const wchar_t ch : std::wstring(L"DC-TEST")) {
        host.send(WM_CHAR, ch, 1);
        const int until = frame + static_cast<int>(std::lround(0.0667F / host.dt()));
        while (frame < until) {
            host.frame();
            ++frame;
        }
    }
    if (capture) host.capture("burst");
    host.set_frame_hook({});
    gui::SocialTextInput::set_clock(nullptr);
}

// The first frames after opening, once the menu had time to prefetch.
void social_first_open(ScenarioHost& host, bool capture) {
    auto& channel = social_mock(host, true, false);
    social_page(channel, social::NewsKind::News, 3);
    social_page(channel, social::NewsKind::Changelog, 2);
    const auto start = std::chrono::steady_clock::now();
    wait_for(host, std::chrono::seconds(10), "social_prefetched", [&] {
        return host.overlay().social().screen_resources_ready({host.width(), host.height()}) &&
               host.overlay().social().screen_text_ready(gui::SocialText::Title, "Server maintenance tonight");
    });
    char line[160];
    std::snprintf(line, sizeof line, "prefetch done after %.0f ms in the menu",
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    host.log(line);
    host.overlay().social().open_screen();
    std::vector<double> times;
    for (int i = 0; i < 12; ++i) {
        const auto begin = std::chrono::steady_clock::now();
        host.frame();
        times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
        if (i == 0 && capture) host.capture("first");
    }
    std::snprintf(line, sizeof line, "first-open frame cpu ms: %.3f %.3f %.3f %.3f ... last %.3f; ready on frame 1: %s",
                  times[0], times[1], times[2], times[3], times.back(),
                  host.overlay().social().screen_resources_ready({host.width(), host.height()}) ? "yes" : "no");
    host.log(line);
}

// Timing workloads (--gpu-timing): input keeps going for every timed frame.
void social_scroll(ScenarioHost& host, bool capture) {
    auto& channel = social_mock(host, true);
    social_page(channel, social::NewsKind::News, 60);
    social_page(channel, social::NewsKind::Changelog, 2);
    social_settle(host);
    const auto tab = host.overlay().social().screen().tab_rect(social::NewsKind::News);
    const LPARAM over_cards = MAKELPARAM(static_cast<WORD>(std::lround(tab.max.x + 200.0F)),
                                         static_cast<WORD>(std::lround(tab.max.y + 150.0F)));
    host.send(WM_MOUSEMOVE, 0, over_cards);
    static int step = 0;
    host.set_frame_hook([&host] {
        // Two seconds down, two seconds up, a wheel notch every other frame.
        const int phase = (step++ / 120) % 2;
        if (step % 2 == 0) host.send(WM_MOUSEWHEEL, MAKEWPARAM(0, phase == 0 ? -WHEEL_DELTA : WHEEL_DELTA), 0);
    });
    for (int i = 0; i < 30; ++i) host.frame();
    if (capture) host.capture("");
}

void social_toasts_steady(ScenarioHost& host, bool capture) {
    auto& channel = social_mock(host, true, false);
    static std::int64_t next = 100;
    host.set_frame_hook([&host, &channel] {
        // Keep three notifications on screen, entering and leaving as usual.
        const auto& toasts = host.overlay().social().toasts();
        if (toasts.visible().size() + toasts.queued() < 3) {
            channel.events.push(social::NewsPosted{social::NewsItem{next, social::NewsKind::News,
                "Notification " + std::to_string(next), "Body", std::nullopt, 0}});
            ++next;
        }
    });
    wait_for(host, std::chrono::seconds(10), "toasts_shown", [&] {
        return host.overlay().social().toasts().visible().size() == 3 && host.overlay().social().screen_fonts_ready();
    });
    if (capture) host.capture("");
}

using Runner = std::function<void(ScenarioHost&, bool)>;

void edge_base(ScenarioHost& host, bool elytra = true) {
    host.overlay().gui().set_open(false);
    for (const auto& module : host.modules().all()) {
        if (module->show_in_arraylist()) module->set_enabled(false);
    }
    set_enabled(host, "arraylist", true);
    for (const char* id : {"edge.auto_crystal", "edge.velocity", "edge.no_fall", "edge.sprint",
                           "edge.nametags", "edge.freecam", "edge.burrow", "edge.timer"}) {
        set_enabled(host, id, true);
    }
    set_enabled(host, "edge.elytra_fly", elytra);
    settle(host);
}

void edge_captures(ScenarioHost& host, bool capture, const std::function<void()>& trigger) {
    trigger();
    int frame = 0;
    for (const auto& [time, suffix] : std::vector<std::pair<float, const char*>>{
             {0.060F, "t060"}, {0.120F, "t120"}, {0.220F, "t220"},
             {0.320F, "t320"}, {0.500F, "t500"}, {1.600F, "done"}}) {
        const int target = static_cast<int>(std::lround(time / host.dt()));
        while (frame < target) { host.frame(); ++frame; }
        if (capture) host.capture(suffix);
    }
}

const std::map<std::string, Runner>& scenarios() {
    static const std::map<std::string, Runner> table{
        {"edgelit_reference", [](ScenarioHost& h, bool c) {
            edge_base(h, false);
            set_enabled(h, "edge.elytra_fly", true);
            for (int i = 0; i < static_cast<int>(std::lround(0.240F / h.dt())); ++i) h.frame();
            if (c) h.capture("");
        }},
        {"edgelit_idle", [](ScenarioHost& h, bool c) {
            edge_base(h);
            if (c) h.capture("");
        }},
        {"edgelit_on", [](ScenarioHost& h, bool c) {
            edge_base(h, false);
            edge_captures(h, c, [&] { set_enabled(h, "edge.elytra_fly", true); });
        }},
        {"edgelit_off", [](ScenarioHost& h, bool c) {
            edge_base(h);
            edge_captures(h, c, [&] { set_enabled(h, "edge.elytra_fly", false); });
        }},
        {"edgelit_mode", [](ScenarioHost& h, bool c) {
            edge_base(h);
            edge_captures(h, c, [&] {
                h.modules().find("edge.velocity")->settings().front().get<Choice>().index = 1;
            });
        }},
        {"edgelit_no_effects", [](ScenarioHost& h, bool c) {
            edge_base(h);
            auto* settings = dynamic_cast<ArrayListModule*>(h.modules().find("arraylist"));
            settings->glow.get<float>() = 0.0F;
            settings->shadow.get<float>() = 0.0F;
            h.frame();
            if (c) h.capture("");
        }},
        {"edgelit_no_pulse", [](ScenarioHost& h, bool c) {
            edge_base(h, false);
            dynamic_cast<ArrayListModule*>(h.modules().find("arraylist"))->pulse.get<bool>() = false;
            edge_captures(h, c, [&] { set_enabled(h, "edge.elytra_fly", true); });
        }},
        {"edgelit_scale", [](ScenarioHost& h, bool c) {
            edge_base(h);
            dynamic_cast<ArrayListModule*>(h.modules().find("arraylist"))->scale.get<float>() = 1.25F;
            settle(h);
            if (c) h.capture("");
        }},
        {"edgelit_scale_cycle", [](ScenarioHost& h, bool c) {
            edge_base(h);
            auto& scale = dynamic_cast<ArrayListModule*>(h.modules().find("arraylist"))->scale.get<float>();
            for (float value : {2.0F, 0.5F, 2.0F, 0.5F}) {
                scale = value;
                settle(h);
            }
            if (c) h.capture("");
        }},
        {"edgelit_single", [](ScenarioHost& h, bool c) {
            edge_base(h);
            for (const auto& module : h.modules().all()) {
                if (module->show_in_arraylist()) module->set_enabled(module->id() == "edge.timer");
            }
            settle(h);
            if (c) h.capture("");
        }},
        {"edgelit_empty", [](ScenarioHost& h, bool c) {
            edge_base(h);
            for (const auto& module : h.modules().all()) if (module->show_in_arraylist()) module->set_enabled(false);
            settle(h);
            if (c) h.capture("");
        }},
        {"closed", [](ScenarioHost& h, bool c) {
            base(h);
            h.overlay().gui().set_open(false);
            settle(h);
            if (c) h.capture("");
        }},
        {"toasts", [](ScenarioHost& h, bool c) { toasts(h, c, false); }},
        {"toasts_menu", [](ScenarioHost& h, bool c) { toasts(h, c, true); }},
        {"social_live", social_live},
        {"social_live_login", social_live_login},
        {"social_login", [](ScenarioHost& h, bool c) { social_scenario(h, c, "login"); }},
        {"social_login_success", [](ScenarioHost& h, bool c) { social_scenario(h, c, "success"); }},
        {"social_login_error", [](ScenarioHost& h, bool c) { social_scenario(h, c, "error"); }},
        {"social_login_network", [](ScenarioHost& h, bool c) { social_scenario(h, c, "network"); }},
        {"social_home", [](ScenarioHost& h, bool c) { social_scenario(h, c, "home"); }},
        {"social_changelog", [](ScenarioHost& h, bool c) { social_scenario(h, c, "changelog"); }},
        {"social_empty", [](ScenarioHost& h, bool c) { social_scenario(h, c, "empty"); }},
        {"social_many", [](ScenarioHost& h, bool c) { social_scenario(h, c, "many"); }},
        {"social_settings", [](ScenarioHost& h, bool c) { social_scenario(h, c, "settings"); }},
        {"social_animation", social_animation},
        {"social_typing", social_typing},
        {"social_login_states", social_login_states},
        {"social_live_chat", social_live_chat},
        {"social_admin", social_admin},
        {"social_admin_focus", social_admin_focus},
        {"social_sign_in_gate", social_sign_in_gate},
        {"social_update_notice", social_update_notice},
        {"social_changelog_marks", social_changelog_marks},
        {"social_global", social_global},
        {"social_dm", social_dm},
        {"social_friends", social_friends},
        {"social_friend_remove", social_friend_remove},
        {"social_first_open", social_first_open},
        {"social_scroll", social_scroll},
        {"social_toasts_steady", social_toasts_steady},
        {"toasts_click", toasts_click},
        {"main_menu_social", [](ScenarioHost& h, bool c) { main_menu_social(h, c, false); }},
        {"main_menu_social_badge", [](ScenarioHost& h, bool c) { main_menu_social(h, c, true); }},
        {"main_menu_social_click", main_menu_social_click},
        {"main_menu_idle", [](ScenarioHost& h, bool c) {
            // The menu without Social, for comparing frame times.
            h.overlay().gui().set_open(false);
            h.overlay().set_main_menu_active(true);
            for (int i = 0; i < static_cast<int>(std::ceil(0.5F / h.dt())); ++i) h.frame();
            if (c) h.capture("");
        }},
        {"social_device_recreate", [](ScenarioHost& h, bool c) { social_scenario(h, c, "device"); }},
        {"open", [](ScenarioHost& h, bool c) {
            base(h);
            settle(h);
            log_panels(h);
            if (c) h.capture("");
        }},
        {"clickgui_reference", [](ScenarioHost& h, bool c) {
            base(h, false);
            set_enabled(h, "arraylist", false);
            for (const char* id : {"demo.fullbright", "demo.zoom", "demo.block_outline", "demo.freelook",
                                   "demo.toggle_sprint", "demo.auto_walk", "demo.japanese", "demo.inventory_hud",
                                   "demo.fps_boost", "example"}) {
                set_enabled(h, id, true);
            }
            h.overlay().gui().set_module_expanded("example", true);
            settle(h);
            log_panels(h);
            if (c) h.capture("");
        }},
        {"collapsed", [](ScenarioHost& h, bool c) {
            base(h);
            for (std::size_t i = 0; i < category_count; ++i) h.overlay().gui().set_panel_collapsed(static_cast<Category>(i), true);
            settle(h);
            log_panels(h);
            if (c) h.capture("");
        }},
        {"settings", [](ScenarioHost& h, bool c) {
            base(h);
            if (Module* example = h.modules().find("example")) example->set_keybind('R');
            h.overlay().gui().set_module_expanded("example", true);
            settle(h);
            log_panels(h);
            if (c) h.capture("");
        }},
        {"dropdown", [](ScenarioHost& h, bool c) {
            base(h);
            h.overlay().gui().set_panel_position(Category::Misc, 522.0F, 880.0F);
            h.overlay().gui().set_module_expanded("example", true);
            settle(h);
            h.overlay().gui().open_overlay("example", "alignment");
            settle(h);
            h.log(h.overlay().gui().overlay_open() ? "dropdown open" : "dropdown NOT open");
            if (c) h.capture("");
        }},
        {"colorpicker", [](ScenarioHost& h, bool c) {
            base(h);
            h.overlay().gui().set_module_expanded("example", true);
            settle(h);
            h.overlay().gui().open_overlay("example", "tint");
            settle(h);
            h.log(h.overlay().gui().overlay_open() ? "color picker open" : "color picker NOT open");
            if (c) h.capture("");
        }},
        {"scroll", [](ScenarioHost& h, bool c) {
            base(h);
            for (const auto& module : h.modules().all()) {
                if (module->category() == Category::Visual) h.overlay().gui().set_module_expanded(module->id(), true);
            }
            settle(h);
            h.overlay().gui().set_panel_scroll(Category::Visual, 240.0F * h.overlay().gui().ui_scale());
            settle(h);
            log_panels(h);
            if (c) h.capture("");
        }},
        {"arraylist_on", [](ScenarioHost& h, bool c) {
            base(h);
            h.overlay().gui().set_open(false);
            settle(h);
            timed_captures(h, c, [&] { set_enabled(h, "demo.motion_blur", true); });
        }},
        {"arraylist_off", [](ScenarioHost& h, bool c) {
            base(h);
            h.overlay().gui().set_open(false);
            settle(h);
            timed_captures(h, c, [&] { set_enabled(h, "demo.toggle_sprint", false); });
        }},
        {"arraylist_glass", [](ScenarioHost& h, bool c) {
            base(h, false);
            h.overlay().gui().set_open(false);
            for (const char* id : {"demo.environment", "demo.inventory_hud", "demo.toggle_sprint",
                                   "demo.toggle_sneak", "demo.block_outline", "demo.motion_blur",
                                   "demo.fps_boost", "demo.crosshair", "demo.fullbright",
                                   "demo.auto_walk", "demo.freelook", "demo.zoom"}) {
                set_enabled(h, id, true);
            }
            settle(h);
            if (c) h.capture("");
        }},
        {"arraylist_mode", [](ScenarioHost& h, bool c) {
            base(h);
            h.overlay().gui().set_open(false);
            set_enabled(h, "demo.fps_boost", true);
            settle(h);
            timed_captures(h, c, [&] {
                auto* m = h.modules().find("demo.fps_boost");
                for (auto& setting : m->settings()) {
                    if (setting.id == "mode") setting.get<Choice>().index = 1;
                }
            });
        }},
        {"shadow_on", [](ScenarioHost& h, bool c) {
            base(h);
            if (auto* cfg = clickgui(h)) cfg->panel_shadow.get<bool>() = true;
            settle(h);
            if (c) h.capture("");
        }},
        {"shadow_off", [](ScenarioHost& h, bool c) {
            base(h);
            if (auto* cfg = clickgui(h)) cfg->panel_shadow.get<bool>() = false;
            settle(h);
            if (c) h.capture("");
        }},
        {"shadow_profile", [](ScenarioHost& h, bool c) {
            base(h, false);
            set_enabled(h, "arraylist", false);
            auto& gui = h.overlay().gui();
            // Measured panels: tall expanded Visual (600 rpx) and one collapsed header.
            gui.set_panel_position(Category::Visual, 200.0F, 100.0F);
            gui.set_debug_panel_height(Category::Visual, 600.0F);
            gui.set_panel_position(Category::Movement, 500.0F, 100.0F);
            gui.set_panel_collapsed(Category::Movement, true);
            // Others far away so their shadows cannot overlap the measured ones.
            gui.set_panel_position(Category::Player, 900.0F, 100.0F);
            gui.set_panel_position(Category::Misc, 1100.0F, 100.0F);
            gui.set_panel_position(Category::Client, 1300.0F, 100.0F);
            settle(h);
            log_panels(h);
            if (c) h.capture("");
        }},
    };
    return table;
}

// --- Input tests -------------------------------------------------------------

struct Tester {
    ScenarioHost& host;
    int failures = 0;

    static LPARAM point(float x, float y) {
        return MAKELPARAM(static_cast<WORD>(static_cast<short>(std::lround(x))), static_cast<WORD>(static_cast<short>(std::lround(y))));
    }
    void frames(int count = 1) { for (int i = 0; i < count; ++i) host.frame(); }
    void move(float x, float y) { host.send(WM_MOUSEMOVE, 0, point(x, y)); frames(); }
    void button(UINT message, float x, float y, WPARAM extra = 0) { host.send(message, extra, point(x, y)); frames(); }
    void left_click(float x, float y) { move(x, y); button(WM_LBUTTONDOWN, x, y, MK_LBUTTON); button(WM_LBUTTONUP, x, y); }
    void right_click(float x, float y) { move(x, y); button(WM_RBUTTONDOWN, x, y, MK_RBUTTON); button(WM_RBUTTONUP, x, y); }
    void middle_click(float x, float y) { move(x, y); button(WM_MBUTTONDOWN, x, y, MK_MBUTTON); button(WM_MBUTTONUP, x, y); }
    void key(int vk) {
        host.send(WM_KEYDOWN, static_cast<WPARAM>(vk), 1);
        frames();
        host.send(WM_KEYUP, static_cast<WPARAM>(vk), static_cast<LPARAM>(0xC0000001));
        frames();
    }
    void wheel(int delta) { host.send(WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(static_cast<short>(delta))), 0); frames(); }

    void expect(bool condition, const char* name) {
        host.log(std::string(condition ? "PASS " : "FAIL ") + name);
        if (!condition) ++failures;
    }

    ClickGui& gui() { return host.overlay().gui(); }
    ClickGui::PanelRect rect(Category category) {
        for (const auto& r : gui().panel_rects()) if (r.category == category) return r;
        return {category, 0, 0, 0, 0};
    }
    float s() { return gui().ui_scale(); }
    float body_top(const ClickGui::PanelRect& r) {
        return r.y + gm::px(mt::header_height, s()) + gm::px(mt::separator, s());
    }
    // Resolve rows from registration order rather than a fixed demo ordinal.
    // Callers keep preceding modules collapsed before measuring these points.
    std::pair<Category, int> module_row(const char* id) {
        const Module& target = module(id);
        int index = 0;
        for (const auto& candidate : host.modules().all()) {
            if (candidate.get() == &target) return {target.category(), index};
            if (candidate->category() == target.category()) ++index;
        }
        throw std::runtime_error(std::string("Missing input-test module: ") + id);
    }
    ImVec2 row(const char* id) {
        const auto [category, index] = module_row(id);
        const auto r = rect(category);
        return {r.x + gm::px(mt::panel_width * 0.4F, s()),
                body_top(r) + gm::px(mt::module_height * (static_cast<float>(index) + 0.5F), s())};
    }
    // A point in the first setting of a module, with preceding modules collapsed.
    ImVec2 first_setting(const char* id, bool slider = false) {
        const auto [category, module_index] = module_row(id);
        const auto r = rect(category);
        const float offset = mt::module_height * static_cast<float>(module_index + 1) + mt::setting_gap_top;
        const float inside = slider ? mt::setting_height - mt::slider_track_from_bottom : mt::setting_height * 0.5F;
        return {r.x + gm::px(mt::panel_width * 0.5F, s()), body_top(r) + gm::px(offset + inside, s())};
    }
    ImVec2 header(Category category) {
        const auto r = rect(category);
        return {r.x + gm::px(mt::panel_width * 0.4F, s()), r.y + gm::px(mt::header_height * 0.5F, s())};
    }
    Module& module(const char* id) {
        if (Module* found = host.modules().find(id)) return *found;
        throw std::runtime_error(std::string("Missing input-test module: ") + id);
    }
    void reset() {
        gui().set_open(true);
        for (std::size_t i = 0; i < category_count; ++i) {
            gui().set_panel_collapsed(static_cast<Category>(i), false);
            gui().set_panel_scroll(static_cast<Category>(i), 0.0F);
        }
        for (const auto& m : host.modules().all()) gui().set_module_expanded(m->id(), false);
        move(-100.0F, -100.0F);
        settle(host);
    }
};

} // namespace

std::vector<std::string> scenario_names() {
    std::vector<std::string> names;
    for (const auto& [name, runner] : scenarios()) names.push_back(name);
    return names;
}

bool run_scenario(const std::string& name, ScenarioHost& host, bool capture) {
    const auto found = scenarios().find(name);
    if (found == scenarios().end()) return false;
    found->second(host, capture);
    return true;
}

int run_input_tests(ScenarioHost& host) {
    Tester t{host};
    t.reset();
    auto& gui = t.gui();

    // Click = press and release on the same element without moving.
    {
        Module& fullbright = t.module("demo.fullbright");
        const bool before = fullbright.enabled();
        const ImVec2 p = t.row("demo.fullbright");
        t.left_click(p.x, p.y);
        t.expect(fullbright.enabled() != before, "left click toggles module");
        // Double click: DOWN, UP, DBLCLK (treated as DOWN), UP -> two toggles.
        t.button(WM_LBUTTONDOWN, p.x, p.y, MK_LBUTTON);
        t.button(WM_LBUTTONUP, p.x, p.y);
        const bool after_first = fullbright.enabled();
        t.button(WM_LBUTTONDBLCLK, p.x, p.y, MK_LBUTTON);
        t.button(WM_LBUTTONUP, p.x, p.y);
        t.expect(after_first != fullbright.enabled() && fullbright.enabled() == !before, "double click toggles twice");
        // Press, move >= 3 px, release on the same row: not a click.
        const bool held = fullbright.enabled();
        t.button(WM_LBUTTONDOWN, p.x, p.y, MK_LBUTTON);
        t.move(p.x + 6.0F, p.y);
        t.button(WM_LBUTTONUP, p.x + 6.0F, p.y);
        t.expect(fullbright.enabled() == held, "moving 6 px cancels the click");
    }

    // Drag a header outside the window and release there.
    {
        t.reset();
        const ImVec2 h = t.header(Category::Movement);
        t.move(h.x, h.y);
        t.button(WM_LBUTTONDOWN, h.x, h.y, MK_LBUTTON);
        t.move(h.x + 20.0F, h.y + 10.0F);
        t.move(-800.0F, h.y + 300.0F);
        t.button(WM_LBUTTONUP, -800.0F, h.y + 300.0F);
        const auto r = t.rect(Category::Movement);
        const float min_x = -(gm::px(mt::panel_width, t.s()) - gm::px(mt::keep_visible_x, t.s()));
        t.expect(std::fabs(r.x - min_x) < 1.0F, "drag outside the window preserves the minimum visible panel width");
        t.move(400.0F, 400.0F);
        t.expect(std::fabs(t.rect(Category::Movement).x - r.x) < 0.5F, "drag ends on release outside the window");
    }

    // Focus loss during a drag ends it and keeps the position.
    {
        t.reset();
        const ImVec2 h = t.header(Category::Player);
        const float x0 = t.rect(Category::Player).x;
        t.move(h.x, h.y);
        t.button(WM_LBUTTONDOWN, h.x, h.y, MK_LBUTTON);
        t.move(h.x + 100.0F, h.y + 50.0F);
        host.send(WM_KILLFOCUS, 0, 0);
        t.frames(2);
        const float kept = t.rect(Category::Player).x;
        t.move(h.x + 300.0F, h.y + 50.0F);
        host.send(WM_SETFOCUS, 0, 0);
        t.frames();
        t.expect(std::fabs(kept - (x0 + 100.0F)) < 1.0F && std::fabs(t.rect(Category::Player).x - kept) < 0.5F,
                 "focus loss ends the drag and keeps the position");
    }

    // Overlapping panels: the front panel receives the click.
    {
        t.reset();
        gui.set_panel_position(Category::Player, t.rect(Category::Movement).x / t.s() + 20.0F, t.rect(Category::Movement).y / t.s());
        t.frames();
        const ImVec2 front = t.header(Category::Player);
        t.left_click(front.x, front.y); // brings Player to the front
        const ImVec2 p = t.row("demo.auto_gg");
        Module& player_first = t.module("demo.auto_gg");
        const bool a = player_first.enabled();
        std::vector<std::pair<Module*, bool>> movement_states;
        for (const auto& module : host.modules().all())
            if (module->category() == Category::Movement) movement_states.emplace_back(module.get(), module->enabled());
        t.left_click(p.x, p.y);
        const bool movement_unchanged = std::all_of(movement_states.begin(), movement_states.end(),
            [](const auto& entry) { return entry.first->enabled() == entry.second; });
        t.expect(player_first.enabled() != a && movement_unchanged, "overlap: only the front panel reacts");
        const auto player_position = gm::initial_panel_position(static_cast<int>(Category::Player), host.width() / t.s());
        gui.set_panel_position(Category::Player, player_position.x, player_position.y);
    }

    // Click outside every panel does nothing.
    {
        t.reset();
        std::vector<bool> states;
        for (const auto& m : host.modules().all()) states.push_back(m->enabled());
        t.left_click(t.host.width() - 300.0F, t.host.height() - 120.0F);
        bool same = true;
        std::size_t i = 0;
        for (const auto& m : host.modules().all()) same = same && m->enabled() == states[i++];
        t.expect(same && gui.open(), "click outside panels changes nothing");
    }

    // Slider drag keeps its value while the panel scrolls.
    {
        t.reset();
        bool reached_zoom = false;
        for (const auto& m : host.modules().all()) {
            if (m->category() != Category::Visual) continue;
            reached_zoom = reached_zoom || m->id() == "demo.zoom";
            gui.set_module_expanded(m->id(), reached_zoom);
        }
        settle(host);
        Module& zoom = t.module("demo.zoom");
        // Keep earlier modules collapsed so Factor is visible; later expanded
        // modules supply the scrollable content used during the slider drag.
        const float s = t.s();
        const ImVec2 p = t.first_setting("demo.zoom", true);
        t.move(p.x, p.y);
        t.button(WM_LBUTTONDOWN, p.x, p.y, MK_LBUTTON);
        const float value = zoom.settings()[0].get<float>();
        const float scroll_before = gui.panel_scroll_target(Category::Visual);
        t.wheel(-120);
        t.frames(10);
        const float after = zoom.settings()[0].get<float>();
        t.button(WM_LBUTTONUP, p.x, p.y);
        t.expect(gui.panel_scroll_target(Category::Visual) > scroll_before && std::fabs(after - value) < 1e-6F,
                 "slider value unaffected by scrolling during a drag");
        // Precise wheel: four 30-unit deltas equal one notch.
        const float start = gui.panel_scroll_target(Category::Visual);
        for (int i = 0; i < 4; ++i) t.wheel(-30);
        t.expect(std::fabs(gui.panel_scroll_target(Category::Visual) - start - mt::wheel_step * s) < 1.0F,
                 "high-resolution wheel deltas accumulate (4 x 30 = 1 notch)");
    }

    // Dropdown closes when its panel is dragged or scrolled; Esc priority.
    {
        t.reset();
        gui.set_module_expanded("demo.crosshair", true);
        settle(host);
        const ImVec2 mode = t.first_setting("demo.crosshair");
        t.left_click(mode.x, mode.y);
        const bool opened = gui.overlay_open();
        const ImVec2 h = t.header(Category::Visual);
        t.move(h.x, h.y);
        t.button(WM_LBUTTONDOWN, h.x, h.y, MK_LBUTTON);
        t.move(h.x + 30.0F, h.y);
        t.button(WM_LBUTTONUP, h.x + 30.0F, h.y);
        t.expect(opened && !gui.overlay_open(), "dropdown closes when its panel is dragged");

        t.reset();
        gui.set_module_expanded("demo.crosshair", true);
        settle(host);
        const ImVec2 mode2 = t.first_setting("demo.crosshair");
        t.left_click(mode2.x, mode2.y);
        t.key(VK_ESCAPE);
        t.expect(!gui.overlay_open() && gui.open(), "Esc closes the dropdown first");
        t.key(VK_ESCAPE);
        t.expect(!gui.open(), "second Esc closes the GUI");
        gui.set_open(true);
        t.frames();
    }

    // Keybind capture.
    {
        t.reset();
        Module& fullbright = t.module("demo.fullbright");
        const ImVec2 p = t.row("demo.fullbright");
        const int original = fullbright.keybind();
        t.middle_click(p.x, p.y);
        t.expect(gui.capturing_keybind(), "middle click starts keybind capture");
        t.key(VK_ESCAPE);
        t.expect(!gui.capturing_keybind() && fullbright.keybind() == original && gui.open(), "Esc cancels capture only");
        t.middle_click(p.x, p.y);
        t.key(VK_BACK);
        t.expect(fullbright.keybind() == 0, "Backspace clears the keybind");
        t.middle_click(p.x, p.y);
        host.send(WM_XBUTTONDOWN, MAKEWPARAM(0, XBUTTON1), Tester::point(p.x, p.y));
        t.frames();
        host.send(WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1), Tester::point(p.x, p.y));
        t.frames();
        t.expect(fullbright.keybind() == VK_XBUTTON1, "Mouse4 is assigned");
        t.middle_click(p.x, p.y);
        Module& crosshair = t.module("demo.crosshair");
        const bool crosshair_state = crosshair.enabled();
        const ImVec2 other = t.row("demo.crosshair");
        t.left_click(other.x, other.y);
        t.expect(!gui.capturing_keybind() && crosshair.enabled() == crosshair_state && fullbright.keybind() == VK_XBUTTON1,
                 "left click cancels capture and is not passed on");
        t.middle_click(p.x, p.y);
        t.key(VK_INSERT);
        t.expect(fullbright.keybind() == VK_INSERT && gui.open(), "GUI open key can be assigned without closing the GUI");
        fullbright.set_keybind(original);
    }
    char summary[64];
    std::snprintf(summary, sizeof summary, "input tests: %d failure(s)", t.failures);
    host.log(summary);
    return t.failures;
}

} // namespace medicon::preview
