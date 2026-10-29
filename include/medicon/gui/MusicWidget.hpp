#pragma once

#include "Toasts.hpp"
#include <medicon/music/AudioSpectrum.hpp>
#include <medicon/music/MusicSessionManager.hpp>

#include <memory>
#include <optional>

namespace medicon::music { class AudioVisualizer; }
namespace medicon::gui {
class TextureHost;

enum class MusicWidgetState { Hidden, Expanded, Collapsing, Compact, Expanding };

struct MusicWidgetPose {
    float x = 0, y = 0, width = 340, height = 100, radius = 18;
    float compact = 0;
};

// Retargets from the currently drawn pose, including during a reversal. Every
// property uses the same 350 ms cubic timeline. A moving toast anchor does not
// restart the morph each frame.
class MusicWidgetAnimator {
public:
    void update(bool available, bool compact, MusicWidgetPose target, float dt);
    const MusicWidgetPose& pose() const noexcept { return pose_; }
    MusicWidgetState state() const noexcept { return state_; }
private:
    MusicWidgetPose pose_{}, from_{}, target_{};
    MusicWidgetState state_ = MusicWidgetState::Hidden;
    bool compact_ = false;
    float elapsed_ = 0.35F;
};

class NotificationMusicCoordinator {
public:
    static MusicWidgetPose target(const ToastManager& toasts, const ToastFrame& frame, bool manual_compact = false);
};

// Automatic sizing is used until the first click. A manual choice survives
// pause/resume and track changes; the four-second timer never overrides it.
class MusicWidgetPresentation {
public:
    void update(bool playing, float dt);
    void toggle() noexcept { manual_compact_ = !compact(); }
    bool compact() const noexcept { return manual_compact_.value_or(elapsed_ >= 4.0); }
private:
    bool playing_ = false;
    double elapsed_ = 0;
    std::optional<bool> manual_compact_;
};

// The actual toast timeline: circle pop, spring stretch, delayed content, then
// the same path backwards. Reversing never resets the current timeline value.
class MusicWidgetVisibility {
public:
    void update(bool shown, float dt);
    ToastManager::Morph morph() const { return ToastManager::morph(anim_); }
    float opacity() const { return morph().opacity; }
    MusicWidgetPose apply(const MusicWidgetPose& pose) const;
private:
    float anim_ = 0;
    bool shown_ = false;
};

// One instance lives with SocialUi, independently of network attachment. Native
// providers are lazy, so headless UI tests and deterministic previews can inject
// snapshots without starting COM/audio workers.
class MusicWidget {
public:
    MusicWidget();
    ~MusicWidget();
    MusicWidget(const MusicWidget&) = delete;
    MusicWidget& operator=(const MusicWidget&) = delete;
    void set_texture_host(TextureHost* host) noexcept;
    void update(SocialFonts& fonts);
    void render(ImDrawList* list, const SocialFonts& fonts, IShadowRenderer& shadows,
                const ToastManager& toasts, const ToastFrame& frame);
    bool available() const noexcept;
    // Includes the outgoing animation after playback has stopped.
    bool visible() const noexcept { return available() || visibility_.opacity() > 0; }
    float opacity() const noexcept { return visibility_.opacity(); }
    ToastManager::Morph appearance() const { return visibility_.morph(); }
    bool compact_preferred() const noexcept { return presentation_.compact(); }
    float art_rotation() const noexcept { return art_rotation_; }
    // Uses the last drawn rounded shape; captured presses keep their release
    // away from panels underneath even if the pointer leaves the widget.
    bool captures_pointer(const PointerInput& pointer) const noexcept;
    MusicWidgetState state() const noexcept { return animator_.state(); }
    const MusicWidgetPose& pose() const noexcept { return animator_.pose(); }
    // Preview/testing source. Passing nullptr returns to the native providers.
    void set_preview(std::shared_ptr<const music::MusicSnapshot> snapshot, music::AudioLevels levels = {});
private:
    void upload_art();
    void finish_hide();
    bool covers(ImVec2 point) const noexcept;
    void handle_pointer(const PointerInput& pointer, bool notifications);
    std::unique_ptr<music::MusicSessionManager> sessions_;
    std::unique_ptr<music::AudioVisualizer> audio_;
    std::shared_ptr<const music::MusicSnapshot> snapshot_, preview_;
    std::shared_ptr<const music::MusicSnapshot> displayed_;
    std::shared_ptr<const music::AlbumArt> uploaded_;
    music::AudioLevels levels_{}, preview_levels_{};
    MusicWidgetAnimator animator_;
    MusicWidgetPresentation presentation_;
    MusicWidgetVisibility visibility_;
    std::optional<MusicWidgetPose> hit_pose_;
    bool pressed_ = false;
    TextureHost* host_ = nullptr;
    ImTextureID texture_ = 0;
    double hue_time_ = 0;
    float level_time_ = 0;
    float art_rotation_ = 0;
    std::string requested_title_, requested_artist_, scratch_;
};
} // namespace medicon::gui
