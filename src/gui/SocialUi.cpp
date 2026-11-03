#include <medicon/gui/SocialUi.hpp>

#include <medicon/gui/Fonts.hpp>
#include <medicon/gui/Theme.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <string>
#include <type_traits>
#include <utility>

namespace medicon::gui {
namespace {

using namespace std::chrono_literals;

std::string one_line(std::string text) {
    std::replace(text.begin(), text.end(), '\r', ' ');
    std::replace(text.begin(), text.end(), '\n', ' ');
    return text;
}

// Spec 2.6: the reasons for not reconnecting must reach the user, also while
// the Social screen is closed.
const char* state_message(const social::StateChanged& change) {
    using social::ConnectionState;
    if (change.state == ConnectionState::Replaced) return "Signed in from another location";
    if (change.state != ConnectionState::AuthFailed) return nullptr;
    if (change.reason == "invalid_key") return "Invalid license key";
    if (change.reason == "revoked") return "This license key has been revoked";
    if (change.reason == "banned") return "This account is suspended";
    if (change.reason == "unauthorized") return "Account verification failed";
    if (change.reason == "username_taken") return "That username is already taken";
    if (change.reason == "credential_load_failed") return "Saved account could not be read";
    if (change.reason == "credential_save_failed") return "Could not save the account";
    if (change.reason == "bad_server_url") return "This client's server URL is invalid";
    return "Could not sign in";
}

} // namespace

void SocialUi::attach(social::Channel* channel, std::function<void()> play_sound) {
    channel_ = channel;
    play_sound_ = std::move(play_sound);
}

void SocialUi::request_screen_glyphs() {
    if (!glyphs_requested_) {
        SocialScreen::request_glyphs(screen_fonts_);
        AltScreen::request_glyphs(screen_fonts_);
        glyphs_requested_ = true;
    }
    // Events may have arrived before the screen was first opened.
    if (store_.me()) {
        screen_fonts_.request(SocialText::Title, store_.me()->name);
        screen_fonts_.request(SocialText::Body, store_.me()->name);
    }
    for (const auto kind : {social::NewsKind::News, social::NewsKind::Changelog}) {
        for (const auto& item : store_.news(kind)) {
            screen_fonts_.request(SocialText::Title, item.title);
            screen_fonts_.request(SocialText::Body, item.body);
            if (item.version) screen_fonts_.request(SocialText::Small, *item.version);
        }
    }
}

void SocialUi::open_screen(SocialView view) {
    request_screen_glyphs();
    alts_.close();
    screen_.open(view);
}

void SocialUi::open_news(social::NewsKind kind) {
    request_screen_glyphs();
    alts_.close();
    screen_.open_news(kind);
}

void SocialUi::open_alts() {
    alt_choice_.reset();
    alt_chosen_.reset();
    request_screen_glyphs();
    alts_.open();
}

void SocialUi::choose_alt(bool reconnect) {
    alts_.close(); // Retire any result from an earlier selection dialog.
    open_alts();
    alt_choice_ = reconnect;
}

void SocialUi::open_from_toast(const Toast& toast) {
    if(!toast.owner.empty()) return;
    // Spec 4.7: the clicked notification opens what it is about.
    switch (toast.target) {
    case ToastTarget::News: open_news(social::NewsKind::News); break;
    case ToastTarget::Changelog: open_news(social::NewsKind::Changelog); break;
    case ToastTarget::Friends: open_screen(SocialView::Friends); break;
    case ToastTarget::Dm:
        request_screen_glyphs();
        alts_.close();
        screen_.open_dm(toast.sender);
        break;
    case ToastTarget::Home: open_screen(SocialView::Home); break;
    }
}

void SocialUi::request_message_glyphs(const social::ChatMessage& message) {
    screen_fonts_.request(SocialText::Body, message.body);
    screen_fonts_.request(SocialText::Title, message.author.name);
}

void SocialUi::sync_ime() {
    if (!ime_) return;
    // The IME follows whichever Social input has the keyboard.
    auto* input = screen_.opened() ? screen_.focused_input() : nullptr;
    ime_->set_active(input != nullptr, input ? input->caret_position() : ImVec2{}, input ? input->caret_height() : 0.0F);
    if (!input) return;
    const auto composition = ime_->composition();
    input->set_composition(composition.text, composition.cursor);
    if (!composition.text.empty()) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, composition.text.data(), static_cast<int>(composition.text.size()),
                                             nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<std::size_t>(std::max(size, 0)), ' ');
        WideCharToMultiByte(CP_UTF8, 0, composition.text.data(), static_cast<int>(composition.text.size()), utf8.data(),
                            size, nullptr, nullptr);
        screen_fonts_.request(SocialText::Body, utf8);
    }
    if (const auto result = ime_->take_result(); !result.empty()) input->type(result);
}

void SocialUi::set_texture_host(TextureHost* host) noexcept {
    if (texture_host_ == host) return;
    // The host owns the GPU resources and is about to be destroyed. Retain the
    // CPU-side cached images so the new renderer can upload them again.
    background_.forget_texture();
    screen_fonts_.forget_texture();
    music_.set_texture_host(host);
    texture_host_ = host;
}

void SocialUi::pump(FontManager& fonts, bool in_menu) {
    if (alt_channel_ && alt_channel_->events.drain(alt_scratch_)) {
        for (const auto& event : alt_scratch_) alts_.on_event(event, screen_fonts_);
        alt_scratch_.clear();
    }
    if (const auto result = alts_.consume_use_finished(); result && result->ok && alts_.opened() && alt_choice_) {
        alt_chosen_ = *alt_choice_;
        alt_choice_.reset();
        alts_.close();
    }
    if (!alts_.opened()) alt_choice_.reset();
    if (!channel_) return;
    const bool timed = store_.debug_timing();
    const auto start = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (channel_->events.drain(scratch_)) {
        for (const auto& event : scratch_) on_event(event, fonts, in_menu);
        scratch_.clear();
    }
    if (timed) add_time(start);
}

void SocialUi::render(ImDrawList* list, IShadowRenderer& shadows, const FrameInfo& frame, const PointerInput& pointer,
                      bool in_menu, bool prefetch, const std::function<void()>& over_screen) {
    music_.set_enabled(store_.music_enabled());
    music_.update(screen_fonts_);
    const bool timed = store_.debug_timing();
    const auto start = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    prefetch = prefetch && channel_ && !screen_visible();
    if (screen_visible() || !toasts_.empty() || music_.visible() || prefetch)
        draw(list, shadows, frame, pointer, in_menu, prefetch, over_screen);
    if (!timed) return;
    add_time(start);
    ++timing_.frames;
    timing_.total_us += frame_us_;
    timing_.max_us = std::max(timing_.max_us, frame_us_);
    frame_us_ = 0.0;
}

void SocialUi::draw(ImDrawList* list, IShadowRenderer& shadows, const FrameInfo& frame, const PointerInput& pointer,
                    bool in_menu, bool prefetch, const std::function<void()>& over_screen) {
    const ImVec2 size{frame.client_w, frame.client_h};
    // Later news glyphs are requested as their events arrive (on_event).
    if (prefetch && !glyphs_requested_) request_screen_glyphs();
    screen_fonts_.update(std::min(frame.client_h, frame.client_w * 1080.0F / 1920.0F), texture_host_);
    if (screen_visible() || prefetch) {
        background_.update(size, texture_host_);
        background_.update_panel(SocialScreen::content_size(size), SocialScreen::picture_blur(size), texture_host_);
    }
    if (prefetch) SocialScreen::prepare(shadows, size);
    // Toasts take clicks only where the client draws a cursor (spec 4.7).
    const PointerInput toast_pointer = in_menu ? pointer : PointerInput{};
    if (screen_visible()) {
        // A notification drawn over the panel takes the pointer from what lies under it.
        PointerInput screen_pointer = pointer;
        if (toast_pointer.valid && (toasts_.covers(toast_pointer.position) || music_.captures_pointer(toast_pointer)))
            screen_pointer = PointerInput{};
        if (screen_.visible()) {
            sync_ime();
            SocialScreen::Context context{list, screen_fonts_, background_, shadows, store_, channel_, frame, screen_pointer,
                                          backdrop_time_};
            screen_.render(context);
        }
        if (alts_.visible()) {
            AltScreen::Context context{list, screen_fonts_, background_, shadows, alt_channel_, frame, screen_pointer,
                                       backdrop_time_};
            alts_.render(context);
        }
        if (over_screen) over_screen();
    }
    music_.set_enabled(store_.music_enabled()); // Apply a Settings click in this same frame.
    // Toasts wait (timers included) until their face is in the atlas.
    ToastFrame toast_frame;
    toast_frame.display = size;
    toast_frame.dt = frame.dt;
    toast_frame.in_menu = in_menu;
    toast_frame.offset_y = store_.notify().in_game_offset_y;
    toast_frame.pointer = toast_pointer;
    toast_frame.music_companion = music_.visible();
    if (!toasts_.empty() && screen_fonts_.ready()) {
        toasts_.render(list, screen_fonts_, shadows, toast_frame);
        if (const auto clicked = toasts_.consume_click()) open_from_toast(*clicked);
    }
    music_.render(list, screen_fonts_, shadows, toasts_, toast_frame);
}

void SocialUi::update_sign_in_gate() {
    if (!startup_sign_in_ || !channel_ || !store_.state_known()) return;
    using social::ConnectionState;
    // Gate only when no credential is saved or authentication was refused.
    const auto state = store_.state();
    if (state == ConnectionState::NoLicense || state == ConnectionState::AuthFailed) gate_ = true;
    if (!gate_) return;
    // Keep it up whatever closed it; showing_login() draws the username form.
    if (!screen_.opened()) open_screen(SocialView::Home);
    screen_.set_locked(true);
}

SocialUi::Timing SocialUi::take_timing() noexcept { return std::exchange(timing_, {}); }

void SocialUi::add_time(std::chrono::steady_clock::time_point start) noexcept {
    frame_us_ += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
}

void SocialUi::on_event(const social::Event& event, FontManager& fonts, bool in_menu) {
    const bool changed = store_.apply(event);
    screen_.on_event(event);
    if (const auto* login = std::get_if<social::LoginFinished>(&event); login && login->ok && gate_) {
        // The account was verified: on to the Main Menu (or game) under the panel.
        gate_ = false;
        screen_.set_locked(false);
        screen_.close();
    }
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, social::SettingsLoaded>) {
            toasts_.set_animation_speed(value.debug_toast_slow_motion ? theme::social::timing::toast_debug_speed : 1.0F);
        } else if constexpr (std::is_same_v<T, social::SessionStarted>) {
            screen_fonts_.request(SocialText::Body, value.me.name);
            screen_fonts_.request(SocialText::Title, value.me.name);
            if (startup_sign_in_ && !std::exchange(welcomed_, true)) {
                Toast toast;
                toast.kind = ToastKind::System;
                toast.title = "Medicon";
                toast.body = one_line("Welcome to " + value.me.name);
                notify(std::move(toast), fonts, in_menu, true);
                // Right after a loader update, say so once, after auth succeeded.
                if (!update_version_.empty() && !std::exchange(update_notified_, true)) {
                    Toast updated;
                    updated.kind = ToastKind::System;
                    updated.title = "Medicon";
                    updated.body = one_line("Updated to " + update_version_);
                    notify(std::move(updated), fonts, in_menu, true);
                }
            }
            // Spec 2.6: reload what the views show after every (re)connect: news,
            // friends, and the newest 50 of every conversation already loaded
            // (merged by id with what is shown).
            for (const auto kind : {social::NewsKind::News, social::NewsKind::Changelog}) {
                if (store_.loading(kind)) continue;
                social::FetchNews request;
                request.kind = kind;
                channel_->api.push(request);
                store_.set_loading(kind, true);
            }
            channel_->api.push(social::FetchFriends{});
            for (const social::ConversationId id : {social::global_conversation, screen_.dm_with()}) {
                auto& conversation = store_.conversation(id);
                if (!conversation.loaded || conversation.loading) continue;
                channel_->api.push(social::FetchMessages{id, std::nullopt, 50});
                conversation.loading = true;
            }
        } else if constexpr (std::is_same_v<T, social::NewsPosted>) {
            screen_fonts_.request(SocialText::Title, value.item.title);
            screen_fonts_.request(SocialText::Body, value.item.body);
            if (value.item.version) screen_fonts_.request(SocialText::Small, *value.item.version);
            if (!changed) return; // already known from a page fetch
            const auto& item = value.item;
            const bool changelog = item.kind == social::NewsKind::Changelog;
            Toast toast;
            toast.kind = ToastKind::News;
            toast.target = changelog ? ToastTarget::Changelog : ToastTarget::News;
            toast.title = changelog ? "Change Log" : "News";
            toast.body = one_line(item.version ? *item.version + "  " + item.title : item.title);
            notify(std::move(toast), fonts, in_menu, store_.notify().news);
        } else if constexpr (std::is_same_v<T, social::NewsPage>) {
            for (const auto& item : value.items) {
                screen_fonts_.request(SocialText::Title, item.title);
                screen_fonts_.request(SocialText::Body, item.body);
                if (item.version) screen_fonts_.request(SocialText::Small, *item.version);
            }
        } else if constexpr (std::is_same_v<T, social::MessagePage>) {
            for (const auto& message : value.items) request_message_glyphs(message);
        } else if constexpr (std::is_same_v<T, social::MessageReceived>) {
            request_message_glyphs(value.message);
            if (!changed || value.conversation == social::global_conversation) return;
            // Spec 4.7: a DM notifies unless that DM is on screen.
            if (screen_.opened() && screen_.view() == SocialView::Dm && screen_.dm_with() == value.conversation) return;
            Toast toast;
            toast.kind = ToastKind::Dm;
            toast.target = ToastTarget::Dm;
            toast.title = value.message.author.name;
            toast.body = one_line(value.message.body);
            toast.sender = value.message.author.id;
            notify(std::move(toast), fonts, in_menu, store_.notify().dm);
        } else if constexpr (std::is_same_v<T, social::FriendsLoaded>) {
            for (const auto& entry : value.list.friends) screen_fonts_.request(SocialText::Body, entry.user.name);
            for (const auto* users : {&value.list.incoming, &value.list.outgoing})
                for (const auto& user : *users) screen_fonts_.request(SocialText::Body, user.name);
        } else if constexpr (std::is_same_v<T, social::SearchFinished>) {
            for (const auto& result : value.items) screen_fonts_.request(SocialText::Body, result.user.name);
        } else if constexpr (std::is_same_v<T, social::FriendRequested>) {
            screen_fonts_.request(SocialText::Body, value.from.name);
            if (!changed) return;
            Toast toast;
            toast.kind = ToastKind::Friend;
            toast.target = ToastTarget::Friends;
            toast.title = "Friend request";
            toast.body = value.from.name + " wants to be friends";
            notify(std::move(toast), fonts, in_menu, store_.notify().friend_requests);
        } else if constexpr (std::is_same_v<T, social::FriendUpdated>) {
            screen_fonts_.request(SocialText::Body, value.user.name);
        } else if constexpr (std::is_same_v<T, social::StateChanged>) {
            // The next account (or the same one after a new key) is greeted again.
            if (value.state == social::ConnectionState::NoLicense || value.state == social::ConnectionState::AuthFailed)
                welcomed_ = false;
            if (!changed) return;
            if (const char* message = state_message(value)) {
                Toast toast;
                toast.kind = ToastKind::System;
                toast.title = "Medicon";
                toast.body = message;
                notify(std::move(toast), fonts, in_menu, true);
            }
        }
    }, event);
}

void SocialUi::notify(Toast toast, FontManager& fonts, bool in_menu, bool enabled) {
    // Spec 4.7: nothing for a kind that is turned off, or while playing with
    // in-game notifications off.
    if (!enabled || (!in_menu && !store_.notify().in_game)) return;
    (void)fonts;
    screen_fonts_.request(SocialText::Title, toast.title);
    screen_fonts_.request(SocialText::Body, toast.body);
    screen_fonts_.request(SocialText::Title, "+0123456789");
    fonts_requested_ = true;
    toasts_.push(std::move(toast));
    // A reconnect can deliver several at once; chime once for the burst.
    const auto now = std::chrono::steady_clock::now();
    if (store_.notify().sound && play_sound_ && now - last_sound_ >= 500ms) {
        last_sound_ = now;
        play_sound_();
    }
}

bool SocialUi::script_sound(bool in_menu) {
    const auto now=std::chrono::steady_clock::now();
    if((!in_menu&&!store_.notify().in_game)||!store_.notify().sound||!play_sound_||now-last_sound_<500ms) return false;
    last_sound_=now; play_sound_(); return true;
}
bool SocialUi::script_notification(std::string_view owner,std::uint64_t generation,std::string_view title,std::string_view body,
                                   FontManager& fonts,bool in_menu) {
    const auto now=std::chrono::steady_clock::now();
    if((!in_menu&&!store_.notify().in_game)||now-last_script_notification_<500ms) return false;
    last_script_notification_=now;
    Toast toast; toast.kind=ToastKind::System; toast.title=title; toast.body=body; toast.owner=owner; toast.generation=generation;
    notify(std::move(toast),fonts,in_menu,true); return true;
}

} // namespace medicon::gui
