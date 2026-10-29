#include <medicon/music/AudioSpectrum.hpp>
#include <medicon/gui/MusicWidget.hpp>
#include <medicon/gui/SocialFonts.hpp>
#include <medicon/gui/Textures.hpp>
#include <medicon/gui/Shadow.hpp>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <thread>
#include <unordered_set>

using namespace medicon;
using namespace medicon::gui;
namespace {
int checks = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

std::shared_ptr<music::MusicSnapshot> song(const char* title, int color) {
    auto track = std::make_shared<music::MusicSnapshot>();
    track->available = track->playing = true;
    track->title = title;
    track->artist = "Artist";
    track->source_app = "Music.Test";
    auto art = std::make_shared<music::AlbumArt>();
    art->width = art->height = 2;
    art->rgba.assign(16, static_cast<std::uint8_t>(color));
    track->art = art;
    return track;
}

void track_slides() {
    MusicWidgetTracks tracks;
    auto a = song("First", 10), b = song("Second", 20), c = song("Third", 30), d = song("Fourth", 40);
    tracks.update(a, true);
    check(tracks.current() == a && !tracks.previous() && tracks.progress() == 1, "first playback does not slide from nothing");
    tracks.update(b, true);
    check(tracks.current() == b && tracks.previous() == a && tracks.progress() == 0, "song change retains both songs at the starting position");
    tracks.advance(0.016F);
    check(tracks.progress() == 0, "first slide frame preserves the outgoing song's position");
    tracks.advance(0.1F);
    const float before = tracks.progress();
    auto art_update = std::make_shared<music::MusicSnapshot>(*b);
    art_update->art = c->art;
    tracks.update(art_update, true);
    check(tracks.current() == art_update && tracks.progress() == before && tracks.previous() == a,
          "late art updates the incoming song without restarting its slide");
    tracks.update(c, true);
    tracks.update(d, true);
    check(tracks.current() == art_update && tracks.progress() == before, "rapid skips do not snap the visible pair");
    for (int i = 0; i < 4; ++i) tracks.advance(0.1F);
    check(tracks.current() == d && tracks.previous() == art_update && tracks.progress() == 0,
          "only the latest queued song follows the in-flight slide");
    float previous = 0;
    for (int i = 0; i < 30; ++i) {
        tracks.advance(1.0F / 60);
        check(tracks.progress() >= previous && tracks.progress() <= 1, "the slide moves continuously left without overshoot");
        previous = tracks.progress();
    }
    check(!tracks.previous() && tracks.current() == d && tracks.progress() == 1, "finished slide drops only the outgoing song");
    tracks.update(a, true);
    tracks.advance(0.016F);
    tracks.advance(std::numeric_limits<float>::quiet_NaN());
    tracks.advance(-1);
    check(tracks.progress() == 0, "invalid frame times cannot corrupt the slide");
    tracks.update(c, false);
    check(!tracks.previous() && tracks.current() == c && tracks.progress() == 1, "hidden songs do not queue a stale entrance slide");
    tracks.clear();
    check(!tracks.current() && !tracks.previous(), "hide completion releases all song snapshots");
}

void sliding_texture_lifecycle() {
    struct Host : TextureHost {
        ImTextureID next = 100;
        std::unordered_set<ImTextureID> live, art;
        int art_uploads = 0;
        std::size_t max_art = 0;
        ImTextureID create(const std::uint8_t*, int w, int h) override {
            const auto id = ++next;
            live.insert(id);
            if (w == 2 && h == 2) { art.insert(id); ++art_uploads; max_art = std::max(max_art, art.size()); }
            return id;
        }
        void release(ImTextureID id) override { check(live.erase(id) == 1, "textures are released once"); art.erase(id); }
    } host;
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1920, 1080};
    unsigned char* pixels; int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    io.Fonts->SetTexID(1);
    {
        SocialFonts fonts;
        MusicWidget widget;
        ToastManager toasts;
        NullShadowRenderer shadows;
        ToastFrame frame;
        frame.display = io.DisplaySize;
        frame.in_menu = true;
        frame.dt = 1.0F / 60;
        auto a = song("First", 10), b = song("Second", 20), c = song("Third", 30), d = song("Fourth", 40);
        widget.set_texture_host(&host);
        widget.set_preview(a);
        widget.set_enabled(false);
        widget.update(fonts);
        check(!widget.available() && !widget.visible() && host.art_uploads == 0, "disabled startup does not display or upload media");
        widget.set_enabled(true);
        widget.update(fonts);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!fonts.ready() && std::chrono::steady_clock::now() < deadline) {
            fonts.update(1080, &host);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(fonts.ready(), "music render fixture loads the real text atlas");
        const auto tick = [&](int count) {
            for (int i = 0; i < count; ++i) {
                widget.update(fonts);
                fonts.update(1080, &host);
                io.DeltaTime = frame.dt;
                ImGui::NewFrame();
                auto* list = ImGui::GetBackgroundDrawList();
                widget.render(list, fonts, shadows, toasts, frame);
                ImGui::Render();
                for (const auto& v : list->VtxBuffer)
                    check(std::isfinite(v.pos.x) && std::isfinite(v.pos.y) && std::isfinite(v.uv.x) && std::isfinite(v.uv.y),
                          "sliding circular meshes contain finite positions/UVs");
                for (const auto& cmd : list->CmdBuffer) {
                    if (!cmd.ElemCount) continue;
                    check(cmd.TextureId == 1 || host.live.contains(cmd.TextureId), "draws never reference released song textures");
                    for (unsigned index = 0; index < cmd.ElemCount; ++index)
                        check(cmd.VtxOffset + list->IdxBuffer[cmd.IdxOffset + index] < static_cast<unsigned>(list->VtxBuffer.Size),
                              "circular slide clipping leaves valid mesh indices");
                }
            }
        };
        tick(50);
        const auto first_texture = *host.art.begin();
        widget.set_preview(b);
        tick(1);
        check(widget.track_transitioning() && widget.track_progress() == 0 && host.art.size() == 2 && host.art.contains(first_texture),
              "outgoing GPU art stays alive while the incoming art is uploaded");
        tick(6);
        const float progress = widget.track_progress();
        widget.set_preview(c); tick(1);
        widget.set_preview(d); tick(1);
        check(widget.track_progress() > progress && host.art.size() == 2, "rapid skipping keeps two GPU images without resetting progress");
        tick(60);
        check(!widget.track_transitioning() && host.art.size() == 1 && !host.live.contains(first_texture),
              "completed/queued slides retire outgoing textures");
        auto same_album = std::make_shared<music::MusicSnapshot>(*d);
        same_album->title = "Same album";
        const int uploads = host.art_uploads;
        widget.set_preview(same_album); tick(30);
        check(host.art_uploads == uploads && host.art.size() == 1, "songs sharing album art reuse one GPU texture");
        frame.pointer.valid = true;
        const auto p = widget.pose();
        frame.pointer.position = {p.x + p.width / 2, p.y + p.height / 2};
        frame.pointer.buttons[0].pressed = frame.pointer.buttons[0].down = true;
        tick(1);
        frame.pointer.buttons[0] = {};
        frame.pointer.buttons[0].released = true;
        tick(1);
        frame.pointer = {};
        tick(25);
        check(widget.state() == MusicWidgetState::Compact, "render fixture selects the circular mode");
        widget.set_preview(b); tick(14);
        check(widget.track_transitioning() && widget.track_progress() > 0 && widget.track_progress() < 1,
              "circular art uses the same in-flight slide timeline");
        widget.set_preview(std::make_shared<music::MusicSnapshot>()); tick(1);
        check(widget.visible() && host.art.size() == 2, "stop during a slide retains both art layers for the exit");
        tick(45);
        check(!widget.visible() && host.art.empty() && host.max_art == 2, "exit retires all art and the cache never grows past two images");
        widget.set_preview(b); tick(50);
        widget.set_enabled(false); tick(1);
        check(!widget.available() && widget.visible(), "turning music off uses the outgoing animation");
        tick(45);
        check(!widget.visible() && host.art.empty(), "music off remains hidden even while the player is still playing");
        widget.set_enabled(true); tick(50);
        check(widget.available() && widget.visible() && widget.state() == MusicWidgetState::Compact,
              "music on restores playing media and the saved manual shape");
    }
    ImGui::DestroyContext();
}
music::AudioLevels tone(float hz, float amplitude = 0.4F, float rate = 48000) {
    music::AudioSpectrum fft;
    fft.reset(rate);
    std::array<float, music::AudioSpectrum::fft_size * 2> samples{};
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = amplitude * std::sin(2 * std::numbers::pi_v<float> * hz * static_cast<float>(i) / rate);
    // Deliberately split at non-FFT boundaries, as actual capture packets do.
    fft.push(std::span(samples).first(137));
    fft.push(std::span(samples).subspan(137));
    return fft.levels();
}
void spectrum() {
    const auto bass = tone(100), mid = tone(1500), treble = tone(9000), quiet = tone(1500, 0.01F);
    const auto peak = [](const auto& v) { return std::max_element(v.begin(), v.end()) - v.begin(); };
    check(peak(bass) < peak(mid) && peak(mid) < peak(treble), "FFT maps low/mid/high frequencies to ordered bands");
    check(*std::max_element(mid.begin(), mid.end()) > *std::max_element(quiet.begin(), quiet.end()) + 0.3F,
          "bands respond to amplitude");
    music::AudioSpectrum fft;
    std::array<float, 2048> silence{};
    fft.push(silence);
    check(std::all_of(fft.levels().begin(), fft.levels().end(), [](float x) { return x == 0; }), "silence is zero");
    silence.fill(std::numeric_limits<float>::quiet_NaN());
    fft.push(silence);
    check(std::all_of(fft.levels().begin(), fft.levels().end(), [](float x) { return std::isfinite(x); }), "bad samples are sanitized");
    music::AudioLevels current{}, full{};
    full.fill(1);
    music::smooth_levels(current, full, 0.05F);
    const float attack = current[0];
    current.fill(1);
    music::smooth_levels(current, {}, 0.05F);
    check(attack > 1-current[0], "attack is faster than release");
    for (int i = 0; i < 120; ++i) music::smooth_levels(current, {}, 1.0F/60);
    check(current[0] < 0.001F, "no-packet/paused input decays to silence");
    music::AudioLevels a{}, b{};
    for (int i = 0; i < 30; ++i) music::smooth_levels(a, full, 1.0F/30);
    for (int i = 0; i < 144; ++i) music::smooth_levels(b, full, 1.0F/144);
    check(std::abs(a[0]-b[0]) < 0.0001F, "smoothing is independent of render FPS");
    const auto low_rate = tone(1500, 0.4F, 44100);
    check(std::abs(peak(mid)-peak(low_rate)) <= 1, "bands preserve frequencies at 44.1 and 48 kHz");
}
void animation() {
    MusicWidgetAnimator animator;
    MusicWidgetPose expanded{400,16,340,100,18,0}, compact{200,12,64,64,32,1};
    animator.update(false, false, expanded, 0);
    check(animator.state() == MusicWidgetState::Hidden, "no media stays hidden");
    animator.update(true, false, expanded, 0);
    check(animator.state() == MusicWidgetState::Expanded, "music opens expanded");
    animator.update(true, true, compact, 0.1F);
    const auto mid = animator.pose();
    check(animator.state() == MusicWidgetState::Collapsing && mid.width < 340 && mid.width > 64, "notification morphs continuously");
    const float fraction = (340-mid.width)/(340-64);
    check(std::abs((mid.x-400)/(200-400)-fraction) < 0.0001F && std::abs(mid.compact-fraction) < 0.0001F,
          "position, width and content share one easing progress");
    animator.update(true, false, expanded, 0);
    check(animator.pose().width == mid.width && animator.pose().x == mid.x, "reverse starts at current drawn pose");
    animator.update(true, false, expanded, 0.05F);
    const auto reverse = animator.pose();
    animator.update(true, true, compact, 0);
    check(animator.pose().width == reverse.width && animator.pose().x == reverse.x, "second notification reverses without teleporting");
    for (int i = 0; i < 30; ++i) animator.update(true, true, compact, 1.0F/60);
    check(animator.state() == MusicWidgetState::Compact && animator.pose().radius == 32, "compact settles in 350 ms");
    auto moved = compact;
    moved.x -= 8;
    animator.update(true, true, moved, 0.01F);
    check(animator.state() == MusicWidgetState::Compact && animator.pose().width == 64,
          "following toast spring/reflow keeps an already circular widget Compact");
    animator.update(true, false, expanded, 0);
    for (int i = 0; i < 30; ++i) animator.update(true, false, expanded, 1.0F/60);
    check(animator.state() == MusicWidgetState::Expanded && animator.pose().width == 340, "last toast restores original instance");
    animator.update(false, false, expanded, 0.01F);
    check(animator.state() == MusicWidgetState::Hidden, "ending session hides widget");
}
void presentation() {
    MusicWidgetPresentation automatic;
    automatic.update(false, 8);
    automatic.update(true, 0);
    automatic.update(true, 3.9F);
    check(!automatic.compact(), "automatic player stays expanded for the first four seconds");
    automatic.update(true, 0.1F);
    check(automatic.compact(), "automatic player becomes compact after four seconds");
    automatic.toggle();
    automatic.update(true, 20);
    check(!automatic.compact(), "clicking automatic compact mode pins expanded instead of retriggering the timer");
    automatic.update(false, 0);
    automatic.update(true, 0);
    automatic.update(true, 10);
    check(!automatic.compact(), "manual expanded survives pause/resume and more than four seconds");
    automatic.toggle();
    automatic.update(true, 20);
    automatic.update(false, 0);
    automatic.update(true, 0);
    check(automatic.compact(), "a second click pins compact through pause/resume too");
    MusicWidgetPresentation resumed;
    resumed.update(true, 0);
    resumed.update(true, 6);
    check(resumed.compact(), "automatic timer uses elapsed wall time even after a slow frame");
    resumed.update(false, 0);
    resumed.update(true, 0);
    resumed.update(true, 3);
    check(!resumed.compact(), "resume starts a fresh automatic four-second display without a manual choice");
    resumed.update(true, std::numeric_limits<float>::quiet_NaN());
    resumed.update(true, -5);
    check(!resumed.compact(), "invalid frame times cannot expire the automatic timer");
}
void visibility() {
    MusicWidgetVisibility motion;
    const MusicWidgetPose card{400,16,340,100,18,0};
    const auto near = [](float a, float b) { return std::abs(a-b) < 0.0001F; };
    motion.update(true, 1.0F/60);
    check(motion.opacity() == 0 && motion.apply(card).width == 0, "first frame is the notification's invisible point");
    motion.update(true, 0.06F);
    auto p = motion.apply(card);
    check(near(p.width,p.height) && near(p.radius,p.height/2) && motion.morph().content == 0,
          "entry starts as a circle with no content, rather than a fading full card");
    motion.update(true, 0.06F);
    check(motion.morph().scale > 1 && motion.opacity() == 1, "circle pop overshoots exactly like the notification");
    motion.update(true, 0.08F);
    motion.update(true, 0.08F);
    p = motion.apply(card);
    check(p.width > p.height && p.width < card.width && motion.morph().content == 0,
          "the circle stretches before title, art or waveform appears");
    motion.update(true, 0.08F);
    motion.update(true, 0.08F);
    const auto reference = ToastManager::morph(0.44F);
    const auto opening = motion.morph();
    check(near(opening.scale,reference.scale) && near(opening.stretch,reference.stretch) &&
          near(opening.round,reference.round) && near(opening.content,reference.content),
          "music uses the notification's exact spring, corner and content curves");
    check(motion.apply(card).width > card.width && opening.content > 0 && opening.content < 1,
          "card width overshoots as content reveals");
    motion.update(true, 0.1F);
    motion.update(true, 0.04F);
    p = motion.apply(card);
    check(near(p.width,340) && near(p.height,100) && near(p.radius,18) && motion.morph().content == 1,
          "entry settles into the full player after the shared 560 ms timeline");
    motion.update(false, 0.1F);
    check(motion.morph().content < 1 && motion.opacity() == 1, "exit first removes content while the card remains solid");
    motion.update(false, 0.1F);
    motion.update(false, 0.05F);
    p = motion.apply(card);
    check(motion.morph().content == 0 && p.width < 340 && p.width > p.height,
          "exit contracts the card to a circle after its content disappears");
    const auto reversing = p;
    motion.update(true, 0);
    p = motion.apply(card);
    check(near(p.width,reversing.width) && near(p.radius,reversing.radius), "resume reverses the same morph without resetting geometry");
    motion.update(false, 0.1F);
    p = motion.apply(card);
    check(near(p.width,p.height) && near(p.radius,p.height/2), "last exit phase is a round pop, not a translucent rectangle");
    motion.update(false, 0.1F);
    motion.update(false, 0.1F);
    check(motion.opacity() == 0 && motion.apply(card).height == 0, "reverse timeline finishes at an invisible point");
    motion.update(true, std::numeric_limits<float>::quiet_NaN());
    check(motion.apply(card).width == 0, "invalid time cannot skip the first pop frame");
    MusicWidgetVisibility compact;
    compact.update(true, 0);
    compact.update(true, 0.1F);
    const auto disk = compact.apply({900,16,64,64,32,1});
    check(disk.width > 64 && near(disk.width,disk.height) && compact.morph().content == 0,
          "compact playback gets the same spring pop while remaining circular");
}
void coordination() {
    ToastManager toasts;
    ToastFrame frame;
    frame.display = {1920,1080};
    frame.in_menu = true;
    const auto open = NotificationMusicCoordinator::target(toasts, frame);
    check(open.x == 790 && open.y == 16, "expanded uses notification position");
    const auto manual = NotificationMusicCoordinator::target(toasts, frame, true);
    check(manual.x == 928 && manual.y == 16 && manual.width == 64 && manual.radius == 32,
          "manual compact mode stays at notification centre without a toast");
    for (int i = 0; i < 5; ++i) toasts.push({ToastKind::News, ToastTarget::Home, "News", "Body", 0});
    auto pose = NotificationMusicCoordinator::target(toasts, frame);
    check(pose.width == 64 && pose.height == 64 && pose.x + pose.width < 780, "stack reserves compact mode on left");
    check(NotificationMusicCoordinator::target(toasts, frame, true).x == pose.x,
          "notifications anchor manual compact mode beside the card too");
    for (int i = 0; i < 200; ++i) toasts.advance(0.02F, true, -1);
    check(!toasts.empty() && NotificationMusicCoordinator::target(toasts,frame).width == 64, "queued toasts keep companion compact");
    for (int i = 0; i < 700; ++i) toasts.advance(0.02F, true, -1);
    check(toasts.empty() && NotificationMusicCoordinator::target(toasts,frame).width == 340, "only final exit restores expanded");
    check(NotificationMusicCoordinator::target(toasts,frame,true).width == 64,
          "manual preference keeps compact mode after final toast");
    for (const ImVec2 size : {ImVec2{320,200}, ImVec2{1280,720}, ImVec2{3840,2160}}) {
        frame.display = size;
        frame.offset_y = 10000;
        frame.in_menu = false;
        pose = NotificationMusicCoordinator::target(toasts,frame);
        check(pose.x >= 0 && pose.y >= 0 && pose.x+pose.width <= size.x+0.01F && pose.y+pose.height <= size.y+0.01F,
              "widget stays in bounds at different sizes and offsets");
    }
}
void texture_lifecycle() {
    struct Host : TextureHost {
        int uploads = 0, releases = 0;
        ImTextureID create(const std::uint8_t*, int, int) override { return static_cast<ImTextureID>(++uploads); }
        void release(ImTextureID) override { ++releases; }
    } first, second;
    SocialFonts fonts;
    MusicWidget widget;
    auto media = std::make_shared<music::MusicSnapshot>();
    media->available = media->playing = true;
    media->title = "A song";
    auto art = std::make_shared<music::AlbumArt>();
    art->width = art->height = 1;
    art->rgba = {20,30,40,255};
    media->art = art;
    widget.set_preview(media);
    widget.set_texture_host(&first);
    for (int i = 0; i < 100; ++i) widget.update(fonts);
    check(first.uploads == 1, "album texture is reused across frames");
    widget.set_texture_host(nullptr);
    widget.set_texture_host(&second);
    widget.update(fonts);
    check(second.uploads == 1 && first.releases == 0, "renderer recreation uploads CPU art to new host");
    auto paused = std::make_shared<music::MusicSnapshot>(*media);
    paused->playing = false;
    widget.set_preview(paused);
    widget.update(fonts);
    check(!widget.available() && widget.state() == MusicWidgetState::Hidden && second.uploads == 1,
          "pause hides the widget and retains cached art");
    widget.set_preview(media);
    widget.update(fonts);
    check(widget.available() && second.uploads == 1, "resume reuses the same art texture");
    widget.set_preview(std::make_shared<music::MusicSnapshot>());
    widget.update(fonts);
    check(!widget.available() && second.releases == 1, "session removal releases art");
}
} // namespace
int main() {
    try {
        spectrum(); animation(); presentation(); visibility(); coordination(); texture_lifecycle(); track_slides(); sliding_texture_lifecycle();
        std::cout << "music tests passed (" << checks << " checks)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "music tests FAILED: " << error.what() << '\n'; return 1;
    }
}
