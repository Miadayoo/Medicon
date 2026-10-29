#include <medicon/gui/Fonts.hpp>
#include <medicon/gui/Clock.hpp>
#include <medicon/gui/Shadow.hpp>
#include <medicon/gui/SocialUi.hpp>
#include <medicon/gui/Theme.hpp>
#include <medicon/gui/Textures.hpp>
#include <medicon/gui/Toasts.hpp>
#include <medicon/social/LicenseStore.hpp>
#include <medicon/social/Service.hpp>

#include <imgui.h>

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace medicon;
using namespace medicon::gui;
using namespace std::chrono_literals;
namespace timing = theme::social::timing;

int checks = 0;
void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

Toast toast(ToastKind kind, std::string body, std::int64_t sender = 0) {
    return Toast{kind, ToastTarget::Home, "title", std::move(body), sender};
}

// Advances in small steps, like frames.
void run(ToastManager& toasts, float seconds, bool in_menu, int hovered = -1) {
    for (float t = 0.0F; t < seconds; t += 0.01F) toasts.advance(0.01F, in_menu, hovered);
}

bool approx(float a, float b, float tolerance = 1e-4F) { return std::fabs(a - b) <= tolerance; }

void test_toast_stack() {
    ToastManager toasts;
    for (int i = 0; i < 4; ++i) toasts.push(toast(ToastKind::News, std::to_string(i)));
    check(toasts.visible().size() == 3 && toasts.queued() == 1, "three visible, the rest queued");
    check(toasts.visible()[0].phase == ToastManager::Phase::Entering &&
          ToastManager::opacity(toasts.visible()[0]) == 0.0F &&
          ToastManager::morph(toasts.visible()[0].anim).scale == 0.0F, "entering starts as an invisible point");

    run(toasts, timing::toast_enter + 0.01F, false);
    const auto shown = ToastManager::morph(toasts.visible()[0].anim);
    check(toasts.visible()[0].phase == ToastManager::Phase::Holding && toasts.visible()[0].anim == timing::toast_enter &&
          shown.opacity == 1.0F && approx(shown.scale, 1.0F) && approx(shown.stretch, 1.0F) && approx(shown.round, 1.0F) &&
          approx(shown.content, 1.0F), "the whole card after 560 ms");
    run(toasts, timing::toast_hold_game, false);
    check(toasts.visible()[0].phase == ToastManager::Phase::Leaving, "3 s while playing, counted after the entrance");
    run(toasts, timing::toast_enter / timing::toast_exit_speed - 0.03F, false);
    check(toasts.visible().size() == 3 && toasts.visible()[0].phase == ToastManager::Phase::Leaving,
          "leaving keeps its slot while it plays back");
    run(toasts, 0.05F, false);
    check(toasts.visible().size() == 1 && toasts.queued() == 0 && toasts.visible()[0].toast.body == "3",
          "finished toasts leave after about 470 ms and the queued one takes a slot");

    for (int i = 0; i < 40; ++i) toasts.push(toast(ToastKind::News, std::to_string(i)));
    check(toasts.visible().size() == 3 && toasts.queued() == ToastManager::max_queued, "the queue is bounded");
}

void test_toast_morph() {
    // Spec toast-anim 2: each stage within its window, overshoots as stated.
    float peak_scale = 0.0F, peak_stretch = 0.0F;
    for (float t = 0.0F; t <= timing::toast_enter; t += 0.001F) {
        const auto m = ToastManager::morph(t);
        peak_scale = std::max(peak_scale, m.scale);
        peak_stretch = std::max(peak_stretch, m.stretch);
    }
    check(peak_scale > 1.08F && peak_scale < 1.12F, "the circle pops about 10 % past full size");
    check(peak_stretch > 1.03F && peak_stretch < 1.05F, "the stretch overshoots about 4 %");
    check(approx(ToastManager::morph(0.200F).scale, 1.0F) && approx(ToastManager::morph(0.060F).opacity, 0.5F) &&
          ToastManager::morph(0.120F).opacity == 1.0F, "pop by 200 ms, linear fade by 120 ms");
    check(ToastManager::morph(0.160F).stretch == 0.0F && ToastManager::morph(0.160F).round == 0.0F &&
          approx(ToastManager::morph(0.480F).stretch, 1.0F) && approx(ToastManager::morph(0.480F).round, 1.0F),
          "stretch and corner radius from 160 to 480 ms");
    check(ToastManager::morph(0.360F).content == 0.0F && approx(ToastManager::morph(0.560F).content, 1.0F),
          "content from 360 to 560 ms");

    ToastManager toasts;
    toasts.push(toast(ToastKind::News, "a"));
    toasts.advance(5.0F, false, -1);
    check(approx(toasts.visible()[0].anim, theme::timing::max_dt), "a stalled frame advances at most 100 ms");

    ToastManager slow;
    slow.set_animation_speed(timing::toast_debug_speed);
    slow.push(toast(ToastKind::News, "a"));
    run(slow, timing::toast_enter + 0.01F, false);
    check(slow.visible()[0].phase == ToastManager::Phase::Entering, "slow motion: still entering after 560 ms");
    run(slow, timing::toast_enter * 3.0F + 0.02F, false);
    check(slow.visible()[0].phase == ToastManager::Phase::Holding, "and whole after 4 x 560 ms");
}

void test_toast_menu_timing() {
    ToastManager menu;
    menu.push(toast(ToastKind::News, "a"));
    run(menu, timing::toast_enter + 0.01F, true);
    run(menu, timing::toast_hold_game + 0.2F, true);
    check(menu.visible()[0].phase == ToastManager::Phase::Holding, "4 s in menus, not 3");
    run(menu, 10.0F, true, 0);
    check(menu.visible()[0].phase == ToastManager::Phase::Holding, "hovering pauses the timer in menus");
    // 3.2 s were already spent; 0.85 s more crosses 4 s but not the ~470 ms exit after it.
    run(menu, 0.85F, true);
    check(menu.visible().size() == 1 && menu.visible()[0].phase == ToastManager::Phase::Leaving,
          "the timer resumes once the cursor leaves");

    // Hovering a leaving toast turns it back from where it is.
    run(menu, 0.15F, true);
    const float anim = menu.visible()[0].anim;
    check(anim < timing::toast_enter && anim > 0.0F, "part of the way out");
    menu.advance(0.0F, true, 0);
    check(menu.visible()[0].phase == ToastManager::Phase::Entering && menu.visible()[0].anim == anim,
          "a leaving toast under the cursor enters again without a jump");
    run(menu, timing::toast_enter - anim + 0.02F, true, 0);
    check(menu.visible()[0].phase == ToastManager::Phase::Holding && menu.visible()[0].time < 0.05F,
          "whole again, with the shown timer started over");

    ToastManager game;
    game.push(toast(ToastKind::News, "a"));
    run(game, timing::toast_enter + timing::toast_hold_game + 0.05F, false, 0);
    check(game.visible()[0].phase == ToastManager::Phase::Leaving, "no hover pause while playing");
    run(game, 0.1F, false, 0);
    check(game.visible()[0].phase == ToastManager::Phase::Leaving, "and no way back while playing");
}

void test_dm_merge() {
    ToastManager toasts;
    toasts.push(toast(ToastKind::Dm, "first", 5));
    run(toasts, timing::toast_enter + 2.0F, false);
    toasts.push(toast(ToastKind::Dm, "second", 5));
    check(toasts.visible().size() == 1 && toasts.visible()[0].merged == 1 && toasts.visible()[0].toast.body == "second",
          "a DM from the same sender updates the toast");
    check(toasts.visible()[0].phase == ToastManager::Phase::Holding && toasts.visible()[0].time == 0.0F &&
          toasts.visible()[0].relayout, "and restarts its timer, laid out again at its new width");

    run(toasts, timing::toast_hold_game + 0.1F, false);
    const float anim = toasts.visible()[0].anim;
    check(toasts.visible()[0].phase == ToastManager::Phase::Leaving && anim < timing::toast_enter, "leaving");
    toasts.push(toast(ToastKind::Dm, "third", 5));
    check(toasts.visible()[0].phase == ToastManager::Phase::Entering && toasts.visible()[0].anim == anim &&
          toasts.visible()[0].merged == 2, "a DM merged into a leaving toast brings it back from where it is");
    run(toasts, timing::toast_enter, false);
    check(toasts.visible()[0].phase == ToastManager::Phase::Holding && toasts.visible()[0].time < timing::toast_enter,
          "with a fresh timer");

    toasts.push(toast(ToastKind::Dm, "other", 6));
    toasts.push(toast(ToastKind::News, "news", 5));
    check(toasts.visible().size() == 3, "other senders and kinds get their own toast");

    ToastManager queued;
    for (int i = 0; i < 3; ++i) queued.push(toast(ToastKind::News, "n"));
    queued.push(toast(ToastKind::Dm, "a", 9));
    queued.push(toast(ToastKind::Dm, "b", 9));
    check(queued.queued() == 1, "DMs waiting in the queue merge too");
}

void test_social_ui() {
    social::Channel channel;
    int chimes = 0;
    SocialUi ui;
    FontManager fonts;
    ui.attach(&channel, [&] { ++chimes; });
    check(!ui.wants_fonts(), "no Social fonts before anything is shown");
    const auto news = [](std::int64_t id) {
        return social::NewsPosted{social::NewsItem{id, social::NewsKind::News, "お知らせ\n本文", "", std::nullopt, 0}};
    };

    channel.events.push(social::SettingsLoaded{});
    channel.events.push(news(1));
    channel.events.push(news(1));
    ui.pump(fonts, false);
    check(ui.toasts().visible().size() == 1, "a duplicate news id shows one toast");
    check(ui.toasts().visible()[0].toast.body == "お知らせ 本文", "bodies are flattened to one line");
    check(ui.toasts().visible()[0].toast.title == "News" && ui.toasts().visible()[0].toast.target == ToastTarget::News,
          "a news toast opens the News tab");
    check(chimes == 1 && ui.wants_fonts(), "a toast chimes and requests the Social face");

    social::NotifySettings quiet;
    quiet.in_game = false;
    quiet.sound = false;
    channel.events.push(social::SettingsLoaded{quiet, false});
    channel.events.push(news(2));
    ui.pump(fonts, false);
    check(ui.toasts().visible().size() == 1, "in-game notifications off hides them while playing");
    channel.events.push(news(3));
    ui.pump(fonts, true);
    check(ui.toasts().visible().size() == 2 && chimes == 1, "but not in menus; sound off is silent");

    social::NotifySettings no_news;
    no_news.news = false;
    channel.events.push(social::SettingsLoaded{no_news, false});
    channel.events.push(news(4));
    ui.pump(fonts, true);
    check(ui.toasts().visible().size() == 2, "a kind turned off shows nothing");
    check(ui.store().news(social::NewsKind::News).size() == 4, "the store still keeps every item");

    channel.events.push(social::StateChanged{social::ConnectionState::Replaced, {}});
    ui.pump(fonts, true);
    check(ui.toasts().visible().size() == 3 && ui.toasts().visible()[2].toast.kind == ToastKind::System &&
          ui.toasts().visible()[2].toast.body == "Signed in from another location", "4009 is reported (spec 2.6)");

    channel.events.push(social::SessionStarted{social::UserRef{1, "fao"}, 0});
    ui.pump(fonts, true);
    const auto first = channel.api.pop(std::chrono::steady_clock::now());
    const auto second = channel.api.pop(std::chrono::steady_clock::now());
    check(first && second && std::get<social::FetchNews>(*first).kind == social::NewsKind::News &&
          std::get<social::FetchNews>(*second).kind == social::NewsKind::Changelog,
          "a hello reloads both news lists");
}

// Spec 4.9: with no notification showing, a frame only checks the event queue.
void test_idle_cost() {
    social::Channel channel;
    SocialUi ui;
    FontManager fonts;
    MeshShadowRenderer shadows;
    ui.attach(&channel, {});
    channel.events.push(social::SettingsLoaded{});
    ui.pump(fonts, false);
    const FrameInfo frame;
    const PointerInput pointer;
    constexpr int frames = 1'000'000;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) {
        ui.pump(fonts, false);
        ui.render(nullptr, shadows, frame, pointer, false);
    }
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / frames;
    std::cout << "idle Social cost: " << ns << " ns per frame\n";
    check(ns < 2000.0, "an idle frame costs well under 2 us");
}

void test_social_font() {
    ImGui::CreateContext();
    {
        struct Host final : TextureHost {
            ImTextureID create(const std::uint8_t*, int width, int height) override {
                return width > 0 && height > 0 ? static_cast<ImTextureID>(++uploads) : 0;
            }
            void release(ImTextureID) override {}
            int uploads = 0;
        } host;
        SocialFonts fonts;
        fonts.request(SocialText::Title, "お知らせ件");
        check(!fonts.ready(), "not ready before the face is loaded");
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!fonts.has(SocialText::Title, "お知らせ件") && std::chrono::steady_clock::now() < deadline) {
            fonts.update(1080.0F, &host);
            std::this_thread::sleep_for(5ms);
        }
        check(fonts.ready() && fonts.has(SocialText::Title, "お知らせ件"), "Social Japanese glyphs load on a worker");
        check(fonts.measure(SocialText::Title, "お知らせ") > 0, "Japanese text is measurable");
        const auto revision = fonts.revision();
        fonts.request(SocialText::Title, "明日の夜にサーバーを停止します");
        const auto next_deadline = std::chrono::steady_clock::now() + 10s;
        while (!fonts.has(SocialText::Title, "明日の夜にサーバーを停止します") && std::chrono::steady_clock::now() < next_deadline) {
            fonts.update(1080.0F, &host);
            std::this_thread::sleep_for(5ms);
        }
        check(fonts.revision() > revision && fonts.has(SocialText::Title, "明日の夜にサーバーを停止します"),
              "new glyphs rebuild the separate Social atlas");
        const auto stable = fonts.revision();
        fonts.update(1080.0F, &host);
        check(fonts.revision() == stable, "known glyphs do not trigger another rebuild");
    }
    ImGui::DestroyContext();
}

void test_screen_state() {
    SocialScreen screen;
    social::SocialStore store;
    check(!screen.visible() && screen.showing_login(store), "initial screen is closed and requires login");
    screen.key_event(VK_ESCAPE, true);
    check(!screen.opened(), "closed screen ignores input");
    screen.open();
    check(screen.opened() && screen.view() == SocialView::Home, "open selects home");
    screen.on_event(social::LoginFinished{false, "invalid_key"});
    check(screen.login_error() == "invalid_key", "login errors are retained for display");
    store.apply(social::StateChanged{social::ConnectionState::Online, {}});
    screen.on_event(social::LoginFinished{true, {}});
    check(!screen.showing_login(store) && screen.view() == SocialView::Home && screen.login_error().empty(), "successful login shows home");
    screen.on_event(social::LogoutFinished{false});
    store.apply(social::StateChanged{social::ConnectionState::NoLicense, {}});
    screen.on_event(social::StateChanged{social::ConnectionState::NoLicense, {}});
    check(screen.showing_login(store) && screen.login_error() == "logout_failed", "failed credential removal is visible after logout");
    screen.key_event(VK_ESCAPE, true);
    check(!screen.opened(), "escape closes the screen");
    Reversible animation;
    animation.step(true, 0.09F, 0.18F, 0.12F);
    check(std::abs(animation.p - 0.5F) < 0.001F, "opening uses elapsed time");
    animation.step(false, 0.06F, 0.18F, 0.12F);
    check(animation.settled(false), "reversal closes from current progress");
    animation.step(true, 0.18F, 0.18F, 0.12F);
    check(animation.settled(true), "opening settles after 180 ms");
    animation.step(false, 0.12F, 0.18F, 0.12F);
    check(animation.settled(false), "closing settles after 120 ms");
    FrameClock clock;
    clock.set_fixed_step(0.25F);
    check(clock.tick() <= 0.1F && std::abs(clock.elapsed() - 0.25F) < 0.001F,
          "Social receives elapsed time even when other animations clamp slow frames");
}

void test_background_cache() {
    using Microsoft::WRL::ComPtr;
    // The fixture is encoded to a file and read back, then handed over as bytes
    // the same way the bundled back.png is.
    struct Fixture {
        std::filesystem::path directory = std::filesystem::temp_directory_path() /
            ("medicon-background-test-" + std::to_string(GetCurrentProcessId()));
        Fixture() { std::filesystem::create_directories(directory); }
        ~Fixture() {
            std::error_code ignored;
            std::filesystem::remove_all(directory, ignored);
        }
    } environment;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    check(SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE, "initialize WIC for fixture");
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(factory.GetAddressOf()))) &&
              SUCCEEDED(factory->CreateStream(stream.GetAddressOf())) &&
              SUCCEEDED(stream->InitializeFromFilename((environment.directory / "back.png").c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf())) &&
              SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
              SUCCEEDED(encoder->CreateNewFrame(frame.GetAddressOf(), nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
              SUCCEEDED(frame->SetSize(64, 64)), "create background PNG fixture");
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        check(SUCCEEDED(frame->SetPixelFormat(&format)) && IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA), "PNG supports BGRA fixture");
        std::vector<std::uint8_t> pixels(64 * 64 * 4);
        for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
            const auto i = static_cast<std::size_t>(y * 64 + x) * 4;
            pixels[i] = pixels[i + 1] = pixels[i + 2] = x < 32 ? 0 : 255;
            pixels[i + 3] = 128;
        }
        check(SUCCEEDED(frame->WritePixels(64, 64 * 4, static_cast<UINT>(pixels.size()), pixels.data())) &&
              SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit()), "write PNG fixture");
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    struct Host final : TextureHost {
        int uploads = 0, releases = 0, width = 0, height = 0;
        std::vector<std::uint8_t> pixels;
        ImTextureID create(const std::uint8_t* bytes, int w, int h) override {
            width = w; height = h;
            pixels.assign(bytes, bytes + static_cast<std::size_t>(w) * h * 4);
            return static_cast<ImTextureID>(++uploads);
        }
        void release(ImTextureID) override { ++releases; }
    } host;
    std::ifstream file(environment.directory / "back.png", std::ios::binary);
    const std::vector<std::uint8_t> png((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    check(!png.empty(), "read PNG fixture");
    SocialBackground background(png);
    const auto wait = [&](ImVec2 size) {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!background.ready(size) && std::chrono::steady_clock::now() < deadline) {
            background.update(size, &host);
            std::this_thread::sleep_for(5ms);
        }
        check(background.ready(size), "background decode, blur and upload complete");
    };
    wait({160, 160});
    const auto center = static_cast<std::size_t>((host.height / 2) * host.width + host.width / 2 - 1) * 4;
    check(host.pixels[center] > 0 && host.pixels[center] < 255, "blur softens the black-white boundary");
    check(host.pixels[3] == 128 && host.pixels[center + 3] == 128 && host.pixels[0] < 4 &&
          host.pixels[static_cast<std::size_t>(host.width - 1) * 4] > 250,
          "blur preserves transparency and hard edges");
    background.update({160, 160}, &host);
    check(host.uploads == 1, "unchanged background is cached");
    wait({192, 160});
    check(host.uploads == 2 && host.releases == 1, "resize regenerates and releases the previous texture");
    background.forget_texture();
    background.update({192, 160}, &host);
    check(background.ready({192, 160}) && host.uploads == 3, "new renderer reuploads cached pixels");

    // The panel picture: its own size and a lighter blur, from the same decoded image.
    const auto panel_deadline = std::chrono::steady_clock::now() + 10s;
    while (!background.panel_ready({100, 80}) && std::chrono::steady_clock::now() < panel_deadline) {
        background.update_panel({100, 80}, 1.0F, &host);
        std::this_thread::sleep_for(5ms);
    }
    check(background.panel_ready({100, 80}) && host.width == 100 && host.height == 80,
          "the panel picture is rendered at the content size");
    check(background.ready({192, 160}) && host.uploads == 4, "and leaves the backdrop as it was");

    // The real back.png bundled in the binary decodes and blurs as well.
    SocialBackground bundled;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!bundled.ready({320, 180}) && !bundled.failed() && std::chrono::steady_clock::now() < deadline) {
        bundled.update({320, 180}, &host);
        std::this_thread::sleep_for(5ms);
    }
    check(!bundled.failed() && bundled.ready({320, 180}), "bundled back.png decodes");
    check(host.width > 0 && host.height > 0 && host.pixels[3] == 255, "bundled back.png is opaque");
}

void test_music_setting_persistence() {
    // Exercise the production file writer/loader without credentials or a live
    // server; this service remains in NoLicense throughout the test.
    struct Directory {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
            (L"MediconMusicPrefs-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        ~Directory() {
            std::error_code error;
            std::filesystem::remove(path / "social.json", error);
            std::filesystem::remove(path, error);
        }
    } fixture;
    check(std::filesystem::create_directory(fixture.path), "create isolated music preference fixture");
    const auto file = fixture.path / "social.json";
    std::ofstream(file) << R"({"music":{"enabled":true},"other":42})";
    const auto loaded = [&](social::SocialService& service) {
        std::optional<social::SettingsLoaded> settings;
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!settings && std::chrono::steady_clock::now() < deadline) {
            std::vector<social::Event> events;
            service.channel().events.drain(events);
            for (const auto& event : events)
                if (const auto* value = std::get_if<social::SettingsLoaded>(&event)) settings = *value;
            if (!settings) std::this_thread::sleep_for(2ms);
        }
        check(settings.has_value(), "native service loads persisted music settings");
        return *settings;
    };
    for (bool enabled : {false, true}) {
        social::SocialService service({fixture.path, "music-setting-test", {}});
        service.start();
        auto settings = loaded(service);
        check(settings.music_enabled == !enabled, "restart restores the last saved music switch");
        const auto old_size = std::filesystem::file_size(file);
        settings.notify.sound = false;
        service.channel().commands.push(social::SaveSettings{settings.notify, enabled});
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::filesystem::file_size(file) == old_size && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(2ms);
        check(std::filesystem::file_size(file) != old_size, "native settings writer commits the music switch");
        check(service.stop(), "settings worker joins after saving");
        std::ifstream stream(file);
        const std::string saved((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        const auto config = social::parse_config(saved).config;
        check(config.music_enabled == enabled && !config.notify.sound && saved.find("42") != std::string::npos,
              "native save keeps music, notification and unrelated settings across restart");
    }
    social::SocialService restarted({fixture.path, "music-setting-test", {}});
    restarted.start();
    check(loaded(restarted).music_enabled, "a final restart reloads music on");
    check(restarted.stop(), "final preference reload shuts down cleanly");
}

void test_license_store() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("medicon-social-test-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const social::LicenseStore store(directory / "license.bin");
    std::string error;
    check(!store.load(error) && error.empty(), "no file means no key");
    const std::string key = "MDC-ABCD-EFGH-JKMN-PQRS";
    check(store.save(key, error), "save: " + error);
    std::ifstream stream(store.file(), std::ios::binary);
    const std::string sealed((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    stream.close();
    check(!sealed.empty() && sealed.find(key) == std::string::npos, "license.bin never holds the plain key");
    check(store.load(error) == key, "DPAPI round trip");

    std::ofstream(store.file(), std::ios::binary | std::ios::trunc) << "garbage";
    check(!store.load(error) && !error.empty() && error.find("garbage") == std::string::npos,
          "an unreadable file is reported without its content");
    check(store.clear(error) && !std::filesystem::exists(store.file()), "logout deletes the file");
    check(store.clear(error), "clearing twice is fine");
    std::filesystem::remove_all(directory);
}


double fake_now = 100.0;
double fake_clock() { return fake_now; }

ImVector<ImWchar> characters(std::wstring_view text) {
    ImVector<ImWchar> result;
    for (const wchar_t c : text) result.push_back(static_cast<ImWchar>(c));
    return result;
}

void test_text_input_animation() {
    using Input = SocialTextInput;
    // The pose: sinks for 60 ms, rises for 120 ms, no overshoot, settled at 180 ms.
    const float depth = std::clamp(14.0F * 0.12F, 1.5F, 3.0F);
    check(Input::pose(0.0, 14.0F).dip == 0.0F && Input::pose(0.0, 14.0F).blend == 0.0F, "starts in place, grey");
    check(std::abs(Input::pose(0.33 * 0.18, 14.0F).dip - depth) < 0.01F, "deepest after a third");
    check(Input::pose(0.18, 14.0F).dip == 0.0F && Input::pose(0.18, 14.0F).blend == 1.0F, "settled after 180 ms");
    check(Input::pose(5.0, 14.0F).dip == 0.0F && Input::pose(5.0, 14.0F).blend == 1.0F, "stays settled");
    check(std::abs(Input::pose(0.0594, 6.0F).dip - 1.5F) < 0.01F && std::abs(Input::pose(0.0594, 60.0F).dip - 3.0F) < 0.01F,
          "the dip is clamped to 1.5..3 px");
    float last_blend = 0.0F;
    for (int i = 0; i <= 180; ++i) {
        const auto pose = Input::pose(i / 1000.0, 14.0F);
        check(pose.dip >= 0.0F && pose.dip <= depth + 0.001F, "never above the line, never deeper than the dip");
        check(pose.blend >= last_blend - 0.0001F && pose.blend <= 1.0F, "the colour only moves toward normal");
        if (i < 59) check(pose.blend == 0.0F, "the start colour holds while sinking");
        last_blend = pose.blend;
    }

    Input::set_clock(fake_clock);
    Input input(300);
    input.set_focused(true);
    fake_now = 100.0;
    input.type(characters(L"ab"));
    fake_now = 100.05;
    input.type(characters(L"c"));
    check(input.spawn_time(0) == 100.0 && input.spawn_time(1) == 100.0 && input.spawn_time(2) == 100.05,
          "each character keeps its own start; typing more never restarts earlier ones");
    check(input.animating(100.2) && !input.animating(100.05 + 0.181), "animating until the last one settles");
    input.key(VK_LEFT, false);
    fake_now = 100.1;
    input.type(characters(L"X"));
    check(input.text() == L"abXc" && input.spawn_time(2) == 100.1 && input.spawn_time(1) == 100.0 &&
          input.spawn_time(3) == 100.05, "a character inserted in the middle moves alone");
    input.key(VK_BACK, false);
    check(input.text() == L"abc" && input.spawn_time(2) == 100.05, "deleting removes its start time too");

    // Clusters: one start time and one caret step each.
    check(social_cluster_end(L"éx", 0) == 2, "a combining accent joins its letter");
    check(social_cluster_end(L"が", 0) == 2, "a combining voicing mark joins its kana");
    check(social_cluster_end(L"\U0001F44D\U0001F3FD!", 0) == 4, "a skin tone joins its emoji");
    check(social_cluster_end(L"\U0001F469‍\U0001F4BB", 0) == 5, "zero-width-joined emoji are one cluster");
    check(social_cluster_end(L"\U0001F1EF\U0001F1F5\U0001F1FA", 0) == 4, "two regional indicators make one flag");
    input.clear();
    fake_now = 101.0;
    input.type(characters(L"\U0001F44D"));
    fake_now = 101.05;
    input.type(characters(L"\U0001F3FD"));
    check(input.text().size() == 4 && input.spawn_time(0) == 101.05 && input.spawn_time(3) == 101.05,
          "a mark typed later animates the whole cluster it joins");
    input.key(VK_BACK, false);
    check(input.text().empty(), "Backspace removes the whole cluster");

    // Paste: the first 24 characters fire 12 ms apart, the rest appear at once.
    fake_now = 102.0;
    input.paste(L"0123456789abcdefghijklmnopqrstuvwxyz\r\nsecond line");
    check(input.text() == L"0123456789abcdefghijklmnopqrstuvwxyz", "a paste keeps the first line");
    check(input.spawn_time(0) == 102.0 && std::abs(input.spawn_time(5) - 102.06) < 1e-9 &&
          std::abs(input.spawn_time(23) - (102.0 + 23 * 0.012)) < 1e-9 && input.spawn_time(24) == 0.0 &&
          input.spawn_time(35) == 0.0, "pasted characters are staggered by 12 ms up to 24");

    // Overwriting a selection animates only the new text.
    input.key('A', true);
    fake_now = 103.0;
    input.type(characters(L"z"));
    check(input.text() == L"z" && input.spawn_time(0) == 103.0, "typing over a selection animates the new character");

    // At most 64 characters move at once; the rest are shown settled.
    input.clear();
    fake_now = 104.0;
    input.type(characters(std::wstring(70, L'k')));
    check(input.spawn_time(63) == 104.0 && input.spawn_time(64) == 0.0 && input.spawn_time(69) == 0.0,
          "characters past 64 in motion are settled at once");

    input.clear();
    input.set_animation(false);
    input.type(characters(L"q"));
    check(input.spawn_time(0) == 0.0 && !input.animating(fake_now), "off: nothing animates");
    Input::set_clock(nullptr);
    const double real = Input::clock();
    check(real > 0.0 && Input::clock() >= real, "the default clock is the real-time counter");
}

// Draws `input` and returns the vertices it produced.
std::vector<ImDrawVert> draw_input(const SocialFonts& fonts, const SocialTextInput& input, double now) {
    fake_now = now;
    ImDrawList list(ImGui::GetDrawListSharedData());
    list._ResetForNewFrame();
    list.PushClipRect({0.0F, 0.0F}, {4096.0F, 4096.0F});
    {
        const SocialFonts::Scope scope(fonts, &list);
        // Disabled hides the caret, so the glyph quads are the last vertices.
        input.draw(&list, fonts, SocialRect{{10.3F, 20.0F}, {1200.0F, 56.0F}}, 1.0F, 1.0F, "", false, 1.0e9);
    }
    return {list.VtxBuffer.begin(), list.VtxBuffer.end()};
}

void test_text_input_drawing() {
    ImGui::CreateContext();
    {
        // A frame in progress sets up the shared draw-list data (circle tessellation, flags).
        auto& io = ImGui::GetIO();
        io.DisplaySize = {1920.0F, 1080.0F};
        io.DeltaTime = 1.0F / 60.0F;
        unsigned char* atlas = nullptr;
        int atlas_w = 0, atlas_h = 0;
        io.Fonts->GetTexDataAsRGBA32(&atlas, &atlas_w, &atlas_h);
        ImGui::NewFrame();
        struct Host final : TextureHost {
            ImTextureID create(const std::uint8_t*, int, int) override { return static_cast<ImTextureID>(++uploads); }
            void release(ImTextureID) override {}
            int uploads = 0;
        } host;
        SocialFonts fonts;
        const std::wstring typed = L"Medicon social 123 あい";
        fonts.request(SocialText::Body, "Medicon social 123 \xE3\x81\x82\xE3\x81\x84*");
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!fonts.has(SocialText::Body, "Medicon social 123 \xE3\x81\x82\xE3\x81\x84*") &&
               std::chrono::steady_clock::now() < deadline) {
            fonts.update(1080.0F, &host);
            std::this_thread::sleep_for(5ms);
        }
        check(fonts.ready(), "Social font for the input drawing test");
        SocialTextInput::set_clock(fake_clock);
        SocialTextInput animated(300), plain(300);
        plain.set_animation(false);
        fake_now = 200.0;
        for (auto* input : {&animated, &plain}) {
            input->set_focused(true);
            input->type(characters(typed));
        }
        const auto settled = draw_input(fonts, animated, 200.5);
        const auto reference = draw_input(fonts, plain, 200.5);
        const auto same = [](const ImDrawVert& a, const ImDrawVert& b) {
            return a.pos.x == b.pos.x && a.pos.y == b.pos.y && a.uv.x == b.uv.x && a.uv.y == b.uv.y && a.col == b.col;
        };
        check(settled.size() == reference.size() && std::equal(settled.begin(), settled.end(), reference.begin(), same),
              "once settled, the animated input draws exactly what the plain path draws");
        const auto moving = draw_input(fonts, animated, 200.06);
        check(moving.size() == reference.size(), "a moving frame draws the same glyphs");
        bool dipped = false;
        for (std::size_t i = 0; i < moving.size(); ++i)
            if (moving[i].pos.y > reference[i].pos.y + 1.0F) dipped = true;
        check(dipped, "characters sink while moving");

        // Settled neighbours do not move while one character in the middle animates.
        SocialTextInput middle(300);
        middle.set_animation(false);
        middle.set_focused(true);
        middle.type(characters(L"abcd"));
        middle.key(VK_LEFT, false);
        middle.key(VK_LEFT, false);
        middle.set_animation(true);
        fake_now = 300.0;
        middle.type(characters(L"X"));
        SocialTextInput flat(300);
        flat.set_animation(false);
        flat.set_focused(true);
        flat.type(characters(L"abXcd"));
        const auto during = draw_input(fonts, middle, 300.06);
        const auto still = draw_input(fonts, flat, 300.06);
        // Glyph quads: 4 vertices each, after the field's own shapes (same in both).
        check(during.size() == still.size(), "same vertex count with one moving character");
        const std::size_t glyphs_from = during.size() - 5 * 4;
        bool neighbours_still = true, moved = false;
        for (std::size_t i = glyphs_from; i < during.size(); ++i) {
            const bool x_glyph = (i - glyphs_from) / 4 == 2;
            if (x_glyph) moved = moved || during[i].pos.y != still[i].pos.y;
            // Runs are measured piece by piece, so allow float rounding, not a pixel.
            else neighbours_still = neighbours_still && std::abs(during[i].pos.x - still[i].pos.x) < 0.01F &&
                                    during[i].pos.y == still[i].pos.y;
        }
        check(moved && neighbours_still, "only the inserted character moves; its neighbours stay put");

        // Cost of the moving path (spec: under 0.1 ms per frame).
        SocialTextInput busy(300);
        busy.set_focused(true);
        fake_now = 400.0;
        busy.type(characters(std::wstring(64, L'm')));
        const auto time = [&](const SocialTextInput& input, double now) {
            ImDrawList list(ImGui::GetDrawListSharedData());
            constexpr int frames = 2000;
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < frames; ++i) {
                list._ResetForNewFrame();
                list.PushClipRect({0.0F, 0.0F}, {4096.0F, 4096.0F});
                const SocialFonts::Scope scope(fonts, &list);
                fake_now = now;
                input.draw(&list, fonts, SocialRect{{10.0F, 20.0F}, {1200.0F, 56.0F}}, 1.0F, 1.0F, "", true, 1.0e9);
            }
            return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / frames;
        };
        const double moving_us = time(busy, 400.05), settled_us = time(busy, 401.0);
        std::cout << "text input draw: " << settled_us << " us settled, " << moving_us << " us with 64 moving\n";
        check(moving_us - settled_us < 100.0, "64 moving characters add well under 0.1 ms");
        SocialTextInput::set_clock(nullptr);
        ImGui::EndFrame();
    }
    ImGui::DestroyContext();
}

void test_open_from_toast() {
    SocialScreen screen;
    screen.open_news(social::NewsKind::Changelog);
    check(screen.opened() && screen.view() == SocialView::Home && screen.news_tab() == social::NewsKind::Changelog,
          "a Change Log toast opens Home on the Change Log tab");
    screen.open(SocialView::Settings);
    screen.open_news(social::NewsKind::News);
    check(screen.view() == SocialView::Home && screen.news_tab() == social::NewsKind::News, "and a news toast on News");
}

} // namespace

int main() {
    try {
        test_toast_stack();
        test_toast_morph();
        test_toast_menu_timing();
        test_dm_merge();
        test_social_ui();
        test_idle_cost();
        test_social_font();
        test_screen_state();
        test_open_from_toast();
        test_text_input_animation();
        test_text_input_drawing();
        test_background_cache();
        test_license_store();
        test_music_setting_persistence();
        std::cout << "social ui tests passed (" << checks << " checks)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "social ui tests FAILED: " << error.what() << '\n';
        return 1;
    }
}
