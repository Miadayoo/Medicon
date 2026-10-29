#include <medicon/gui/MusicWidget.hpp>
#include <medicon/gui/Draw.hpp>
#include <medicon/gui/GuiMath.hpp>
#include <medicon/gui/Shadow.hpp>
#include <medicon/gui/SocialFonts.hpp>
#include <medicon/gui/Textures.hpp>
#include <medicon/music/AudioVisualizer.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace medicon::gui {
namespace {
constexpr float duration = 0.35F;
constexpr float pi = std::numbers::pi_v<float>;
bool same_song(const music::MusicSnapshot& a, const music::MusicSnapshot& b) {
    return a.title == b.title && a.artist == b.artist && a.source_app == b.source_app;
}
MusicWidgetPose blend(const MusicWidgetPose& a, const MusicWidgetPose& b, float t) {
    return {std::lerp(a.x,b.x,t), std::lerp(a.y,b.y,t), std::lerp(a.width,b.width,t),
            std::lerp(a.height,b.height,t), std::lerp(a.radius,b.radius,t), std::lerp(a.compact,b.compact,t)};
}
void music_icon(ImDrawList* list, ImVec2 center, float size, float alpha) {
    const auto p = [&](float x, float y) { return ImVec2{center.x + x * size, center.y + y * size}; };
    const auto color = to_u32({0.94F, 0.95F, 1, 1}, alpha);
    list->AddLine(p(-0.17F, 0.25F), p(-0.17F, -0.35F), color, size * 0.09F);
    list->AddLine(p(0.31F, 0.14F), p(0.31F, -0.46F), color, size * 0.09F);
    list->AddLine(p(-0.17F, -0.35F), p(0.31F, -0.46F), color, size * 0.12F);
    list->AddEllipseFilled(p(-0.29F, 0.28F), {size * 0.18F, size * 0.12F}, color);
    list->AddEllipseFilled(p(0.19F, 0.17F), {size * 0.18F, size * 0.12F}, color);
}
// Rounded segment, with a restrained halo. Uses the shared draw list/atlas.
void bar(ImDrawList* list, ImVec2 a, ImVec2 b, float width, Color color, float alpha) {
    list->AddLine(a, b, to_u32(color, alpha * 0.07F), width + 3);
    list->AddLine(a, b, to_u32(color, alpha), width);
    list->AddCircleFilled(a, width * 0.5F, to_u32(color, alpha), 8);
    list->AddCircleFilled(b, width * 0.5F, to_u32(color, alpha), 8);
}

// A textured disk with an antialiased rim. Rotate the UVs inside the circular
// mesh so the cached album texture can spin without a new bitmap or shader.
void spinning_art(ImDrawList* list, ImTextureID texture, const music::AlbumArt& image,
                  ImVec2 center, float radius, float angle, float alpha) {
    constexpr int segments = 48;
    const float u_radius = 0.5F * std::min(1.0F, static_cast<float>(image.height) / image.width);
    const float v_radius = 0.5F * std::min(1.0F, static_cast<float>(image.width) / image.height);
    const float fringe = std::min(0.65F, radius * 0.2F);
    const ImU32 color = to_u32(theme::white, alpha);
    const ImU32 transparent = to_u32(theme::white, 0);
    list->PushTextureID(texture);
    list->PrimReserve(segments * 9, segments * 2 + 1);
    const auto base = list->_VtxCurrentIdx;
    list->PrimWriteVtx(center, {0.5F, 0.5F}, color);
    for (int i = 0; i < segments; ++i) {
        const float theta = static_cast<float>(i) / segments * 2 * pi;
        const float cs = std::cos(theta), sn = std::sin(theta);
        const float u = std::cos(theta - angle) * u_radius;
        const float v = std::sin(theta - angle) * v_radius;
        for (int ring = 0; ring < 2; ++ring) {
            const float r = radius + (ring == 0 ? -fringe : fringe);
            list->PrimWriteVtx({center.x + cs * r, center.y + sn * r},
                {0.5F + u * r / radius, 0.5F + v * r / radius}, ring == 0 ? color : transparent);
        }
    }
    for (int i = 0; i < segments; ++i) {
        const auto inner = base + 1 + static_cast<unsigned>(i) * 2;
        const auto outer = inner + 1;
        const auto next_inner = base + 1 + static_cast<unsigned>((i + 1) % segments) * 2;
        const auto next_outer = next_inner + 1;
        const auto index = [&](unsigned value) { list->PrimWriteIdx(static_cast<ImDrawIdx>(value)); };
        index(base); index(inner); index(next_inner);
        index(inner); index(outer); index(next_outer);
        index(inner); index(next_outer); index(next_inner);
    }
    list->PopTextureID();
}

struct ArtVertex { ImVec2 position, uv; float opacity; };

int clip_art(const std::array<ArtVertex, 6>& in, int count, std::array<ArtVertex, 6>& out,
             float boundary, bool keep_right) {
    if (count == 0) return 0;
    const auto inside = [&](const ArtVertex& v) { return keep_right ? v.position.x >= boundary : v.position.x <= boundary; };
    int written = 0;
    auto previous = in[count - 1];
    for (int i = 0; i < count; ++i) {
        const auto current = in[i];
        if (inside(previous) != inside(current)) {
            const float t = std::clamp((boundary - previous.position.x) / (current.position.x - previous.position.x), 0.0F, 1.0F);
            out[written++] = {{boundary, std::lerp(previous.position.y, current.position.y, t)},
                {std::lerp(previous.uv.x, current.uv.x, t), std::lerp(previous.uv.y, current.uv.y, t)},
                std::lerp(previous.opacity, current.opacity, t)};
        }
        if (inside(current)) out[written++] = current;
        previous = current;
    }
    return written;
}

// Slide the artwork underneath a fixed circular aperture. Clipping the same
// antialiased disk's triangles keeps both the circular rim and the moving seam
// clean; a rectangular scissor alone would leak art outside the round icon.
void sliding_art(ImDrawList* list, ImTextureID texture, const music::AlbumArt& image,
                 ImVec2 center, float radius, float angle, float alpha, float offset) {
    if (std::abs(offset) < 0.001F) { spinning_art(list, texture, image, center, radius, angle, alpha); return; }
    if (std::abs(offset) >= radius * 2) return;
    constexpr int segments = 48, triangles = segments * 3;
    constexpr int max_indices = triangles * 9, max_vertices = triangles * 5;
    const float fringe = std::min(0.65F, radius * 0.2F);
    const float u_radius = 0.5F * std::min(1.0F, static_cast<float>(image.height) / image.width);
    const float v_radius = 0.5F * std::min(1.0F, static_cast<float>(image.width) / image.height);
    const float cs = std::cos(angle), sn = std::sin(angle);
    const auto vertex = [&](ImVec2 position, float opacity) {
        const float x = (position.x - center.x - offset) / radius, y = (position.y - center.y) / radius;
        return ArtVertex{position, {0.5F + (x * cs + y * sn) * u_radius, 0.5F + (-x * sn + y * cs) * v_radius}, opacity};
    };
    std::array<ArtVertex, segments * 2 + 1> vertices;
    vertices[0] = vertex(center, 1);
    for (int i = 0; i < segments; ++i) {
        const float theta = static_cast<float>(i) / segments * 2 * pi;
        for (int ring = 0; ring < 2; ++ring) {
            const float r = radius + (ring == 0 ? -fringe : fringe);
            vertices[1 + i * 2 + ring] = vertex({center.x + std::cos(theta) * r, center.y + std::sin(theta) * r}, ring == 0 ? 1 : 0);
        }
    }
    list->PushTextureID(texture);
    list->PrimReserve(max_indices, max_vertices);
    int indices = 0, count = 0;
    const auto triangle = [&](int a, int b, int c) {
        std::array<ArtVertex, 6> polygon{vertices[a], vertices[b], vertices[c]}, clipped;
        int n = clip_art(polygon, 3, clipped, center.x + offset - radius, true);
        n = clip_art(clipped, n, polygon, center.x + offset + radius, false);
        if (n < 3) return;
        const auto base = list->_VtxCurrentIdx;
        for (int i = 0; i < n; ++i)
            list->PrimWriteVtx(polygon[i].position, polygon[i].uv, to_u32(theme::white, alpha * polygon[i].opacity));
        for (int i = 1; i < n - 1; ++i) {
            list->PrimWriteIdx(static_cast<ImDrawIdx>(base));
            list->PrimWriteIdx(static_cast<ImDrawIdx>(base + i));
            list->PrimWriteIdx(static_cast<ImDrawIdx>(base + i + 1));
        }
        count += n;
        indices += (n - 2) * 3;
    };
    for (int i = 0; i < segments; ++i) {
        const int inner = 1 + i * 2, outer = inner + 1;
        const int next_inner = 1 + ((i + 1) % segments) * 2, next_outer = next_inner + 1;
        triangle(0, inner, next_inner);
        triangle(inner, outer, next_outer);
        triangle(inner, next_outer, next_inner);
    }
    list->PrimUnreserve(max_indices - indices, max_vertices - count);
    list->PopTextureID();
}
} // namespace

void MusicWidgetTracks::update(std::shared_ptr<const music::MusicSnapshot> next, bool animate) {
    if (!next) return;
    if (!current_ || !animate) {
        clear();
        current_ = std::move(next);
    } else if (same_song(*current_, *next)) {
        current_ = std::move(next); // Metadata/art arrived later for the same song.
        pending_.reset();
    } else if (previous_) {
        pending_ = std::move(next); // Do not snap either visible layer on a rapid skip.
    } else {
        previous_ = std::move(current_);
        current_ = std::move(next);
        elapsed_ = 0;
        just_started_ = true;
    }
}

void MusicWidgetTracks::advance(float dt) {
    if (!previous_) return;
    if (just_started_) { just_started_ = false; return; }
    elapsed_ = std::min(duration, elapsed_ + clamp_dt(dt));
    if (elapsed_ < duration) return;
    previous_.reset();
    if (pending_) {
        auto next = std::move(pending_);
        update(std::move(next), true);
        just_started_ = false; // Its zero-progress frame is drawn by this render.
    }
}

void MusicWidgetTracks::clear() {
    current_.reset(); previous_.reset(); pending_.reset();
    elapsed_ = duration;
    just_started_ = false;
}

float MusicWidgetTracks::progress() const noexcept {
    if (!previous_) return 1;
    const float t = std::clamp(elapsed_ / duration, 0.0F, 1.0F);
    const float u = 1 - t;
    return t < 0.5F ? 4 * t * t * t : 1 - 4 * u * u * u;
}

void MusicWidgetPresentation::update(bool playing, float dt) {
    if (!playing) { playing_ = false; elapsed_ = 0; return; }
    if (!playing_) { playing_ = true; elapsed_ = 0; return; }
    // Use elapsed wall time, rather than the animation's 100 ms frame clamp.
    if (std::isfinite(dt) && dt > 0) elapsed_ = std::min(4.0, elapsed_ + static_cast<double>(dt));
}

void MusicWidgetVisibility::update(bool shown, float dt) {
    namespace timing = theme::social::timing;
    // Like ToastManager's first render, the first frame is an invisible point.
    if (shown && !shown_ && anim_ == 0) {
        shown_ = true;
        return;
    }
    shown_ = shown;
    const float step = clamp_dt(dt) * (shown ? 1.0F : -timing::toast_exit_speed);
    anim_ = std::clamp(anim_ + step, 0.0F, timing::toast_enter);
}

MusicWidgetPose MusicWidgetVisibility::apply(const MusicWidgetPose& pose) const {
    const auto m = morph();
    auto result = pose;
    result.width = (pose.height + (pose.width - pose.height) * m.stretch) * m.scale;
    result.height = pose.height * m.scale;
    result.radius = std::lerp(pose.height * 0.5F, pose.radius, m.round) * m.scale;
    result.x = pose.x + (pose.width - result.width) * 0.5F;
    result.y = pose.y + (pose.height - result.height) * 0.5F;
    return result;
}

void MusicWidgetAnimator::update(bool available, bool compact, MusicWidgetPose target, float dt) {
    if (!available) { state_ = MusicWidgetState::Hidden; return; }
    target.compact = compact ? 1.0F : 0.0F;
    if (state_ == MusicWidgetState::Hidden) {
        pose_ = from_ = target_ = target;
        compact_ = compact;
        elapsed_ = duration;
    } else if (compact != compact_) {
        from_ = pose_;
        elapsed_ = 0;
        compact_ = compact;
    } else if (elapsed_ >= duration &&
               (std::abs(target.x - target_.x) > 0.5F || std::abs(target.y - target_.y) > 0.5F ||
                std::abs(target.width - target_.width) > 0.5F || std::abs(target.height - target_.height) > 0.5F)) {
        from_ = pose_;
        elapsed_ = 0;
    }
    target_ = target;
    elapsed_ = std::min(duration, elapsed_ + clamp_dt(dt));
    pose_ = blend(from_, target_, ease_out_cubic(elapsed_ / duration));
    const bool shape_settled = std::abs(pose_.compact - target_.compact) < 0.00001F;
    state_ = elapsed_ >= duration || shape_settled ? (compact ? MusicWidgetState::Compact : MusicWidgetState::Expanded) :
                                  (compact ? MusicWidgetState::Collapsing : MusicWidgetState::Expanding);
}

MusicWidgetPose NotificationMusicCoordinator::target(const ToastManager& toasts, const ToastFrame& frame,
                                                    bool manual_compact) {
    namespace metrics = theme::social::metrics;
    const float s = std::max(0.001F, frame.display.y / metrics::reference_height);
    const float top = (metrics::toast_top + (frame.in_menu ? 0 : frame.offset_y)) * s;
    const bool compact = manual_compact || !toasts.empty();
    MusicWidgetPose result;
    result.width = (compact ? 64 : 340) * s;
    result.height = (compact ? 64 : 100) * s;
    result.radius = (compact ? 32 : 18) * s;
    result.x = (frame.display.x - result.width) * 0.5F;
    result.y = top;
    if (!toasts.empty()) {
        // Use the first card's final left edge while it enters as a circle,
        // then follow any actual spring overshoot and animated stack reflow.
        const auto anchor = toasts.music_anchor();
        const float left = anchor ? anchor->x : frame.display.x * 0.5F - metrics::toast_min_width * s * 0.5F;
        const float center_y = anchor ? anchor->y : top + metrics::toast_height * s * 0.5F;
        result.x = left - (14 + 64) * s;
        result.y = center_y - result.height * 0.5F;
    }
    // Extremely narrow windows use a smaller widget and a reserved toast lane.
    result.width = std::min(result.width, std::max(1.0F, frame.display.x - 8 * s));
    result.height = std::min(result.height, std::max(1.0F, frame.display.y - 8 * s));
    result.x = std::clamp(result.x, 0.0F, std::max(0.0F, frame.display.x - result.width));
    result.y = std::clamp(result.y, 0.0F, std::max(0.0F, frame.display.y - result.height));
    result.radius = std::min(result.radius, std::min(result.width, result.height) * 0.5F);
    return result;
}

MusicWidget::MusicWidget() = default;
MusicWidget::~MusicWidget() = default;
void MusicWidget::set_texture_host(TextureHost* host) noexcept {
    if (host == host_) return;
    // TextureHost owns resources and frees them when replaced; retain CPU art.
    host_ = host;
    art_textures_ = {};
}
void MusicWidget::set_preview(std::shared_ptr<const music::MusicSnapshot> value, music::AudioLevels levels) {
    preview_ = std::move(value);
    preview_levels_ = levels;
    if (audio_) audio_->set_enabled(false);
}
void MusicWidget::set_enabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    if (!enabled) {
        if (audio_) audio_->set_enabled(false);
        presentation_.update(false, 0);
        hit_pose_.reset();
        pressed_ = false;
    }
}
bool MusicWidget::available() const noexcept { return enabled_ && snapshot_ && snapshot_->available && snapshot_->playing; }
bool MusicWidget::covers(ImVec2 point) const noexcept {
    if (!available() || !hit_pose_) return false;
    const auto& p = *hit_pose_;
    if (point.x < p.x || point.y < p.y || point.x >= p.x + p.width || point.y >= p.y + p.height) return false;
    const float r = std::clamp(p.radius, 0.0F, std::min(p.width, p.height) * 0.5F);
    const float dx = point.x - std::clamp(point.x, p.x + r, p.x + p.width - r);
    const float dy = point.y - std::clamp(point.y, p.y + r, p.y + p.height - r);
    return dx * dx + dy * dy <= r * r;
}
bool MusicWidget::captures_pointer(const PointerInput& pointer) const noexcept {
    return available() && pointer.valid && (pressed_ || covers(pointer.position));
}
void MusicWidget::handle_pointer(const PointerInput& pointer, bool notifications) {
    if (!pointer.valid || notifications) { pressed_ = false; return; }
    const bool hovered = covers(pointer.position);
    const auto& left = pointer.buttons[0];
    if (left.pressed) pressed_ = hovered;
    if (left.released) {
        if (pressed_ && hovered) presentation_.toggle();
        pressed_ = false;
    }
    if (!left.down && !left.pressed && !left.released) pressed_ = false;
}
void MusicWidget::update(SocialFonts& fonts) {
    if (!enabled_) snapshot_.reset(); // Do not start native providers while disabled.
    else if (preview_) snapshot_ = preview_;
    else {
        if (!sessions_) sessions_ = std::make_unique<music::MusicSessionManager>();
        snapshot_ = sessions_->snapshot();
        if (available() && !audio_) audio_ = std::make_unique<music::AudioVisualizer>();
        if (audio_) audio_->set_enabled(available() && snapshot_->playing);
    }
    if (available()) {
        tracks_.update(snapshot_, visibility_.morph().content > 0);
        if (requested_title_ != snapshot_->title) {
            requested_title_ = snapshot_->title;
            fonts.request(SocialText::Title, requested_title_);
        }
        if (requested_artist_ != snapshot_->artist) {
            requested_artist_ = snapshot_->artist;
            fonts.request(SocialText::Small, requested_artist_);
        }
    }
    if (!available()) {
        presentation_.update(false, 0);
        hit_pose_.reset();
        pressed_ = false;
        if (!visible()) finish_hide();
    }
    upload_art();
}
void MusicWidget::finish_hide() {
    animator_.update(false, false, {}, 0);
    tracks_.clear();
    levels_.fill(0);
    level_time_ = 0;
    hit_pose_.reset();
    pressed_ = false;
}
void MusicWidget::upload_art() {
    // A session can vanish with no remaining art or metadata. Keep the last
    // drawn content intact throughout its exit, then retain paused art as usual.
    const std::array images{
        tracks_.current() ? tracks_.current()->art : (snapshot_ ? snapshot_->art : nullptr),
        tracks_.previous() ? tracks_.previous()->art : nullptr};
    // Retain the old song's existing texture, including while its replacement
    // art loads. Two songs with the same album share one upload.
    for (auto& slot : art_textures_) {
        if (slot.image && slot.image != images[0] && slot.image != images[1]) {
            if (slot.texture && host_) host_->release(slot.texture);
            slot = {};
        }
    }
    for (const auto& image : images) {
        if (!image || !host_ || art_texture(image) || image->width <= 0 || image->height <= 0 ||
            image->width > 192 || image->height > 192 ||
            image->rgba.size() != static_cast<std::size_t>(image->width * image->height * 4)) continue;
        for (auto& slot : art_textures_) {
            if (slot.texture) continue;
            slot.texture = host_->create(image->rgba.data(), image->width, image->height);
            if (slot.texture) slot.image = image;
            break;
        }
    }
}

ImTextureID MusicWidget::art_texture(const std::shared_ptr<const music::AlbumArt>& image) const noexcept {
    if (image) for (const auto& slot : art_textures_) if (slot.image == image) return slot.texture;
    return 0;
}

void MusicWidget::render(ImDrawList* list, const SocialFonts& fonts, IShadowRenderer& shadows,
                         const ToastManager& toasts, const ToastFrame& frame) {
    const bool ready = list && fonts.ready();
    const bool showing = available() && ready;
    if (showing) {
        handle_pointer(frame.pointer, !toasts.empty());
        presentation_.update(true, frame.dt);
        const bool preferred_compact = presentation_.compact();
        animator_.update(true, preferred_compact || !toasts.empty(),
                         NotificationMusicCoordinator::target(toasts, frame, preferred_compact), frame.dt);
    }
    else { hit_pose_.reset(); pressed_ = false; }
    visibility_.update(showing, frame.dt);
    const auto appearance = visibility_.morph();
    const float alpha = appearance.opacity;
    if (!available() && alpha == 0) {
        finish_hide();
        upload_art();
    }
    // While exiting, hold the current shape and use the last displayed song.
    // Resuming reverses opacity from here and retargets that same morph.
    if (!ready || alpha <= 0 || appearance.scale < 0.001F || !tracks_.current()) return;
    if (showing && appearance.content > 0 && fonts.has(SocialText::Title, tracks_.current()->title) &&
        fonts.has(SocialText::Small, tracks_.current()->artist)) tracks_.advance(frame.dt);
    upload_art();
    // The host can Present at hundreds of FPS; visualizer updates stay at 60 Hz.
    level_time_ += clamp_dt(frame.dt);
    constexpr float step = 1.0F / 60;
    if (level_time_ >= step) {
        music::AudioLevels target{};
        if (available()) target = preview_ ? preview_levels_ : (audio_ ? audio_->levels() : target);
        const float consumed = std::floor(level_time_ / step) * step;
        music::smooth_levels(levels_, target, consumed);
        level_time_ -= consumed;
    }
    hue_time_ += clamp_dt(frame.dt);
    art_rotation_ = std::fmod(art_rotation_ + clamp_dt(frame.dt) * (2 * pi / 40), 2 * pi);
    const auto& full_pose = animator_.pose();
    auto pose = visibility_.apply(full_pose);
    const float s = frame.display.y / theme::social::metrics::reference_height;
    // A resize can shrink the viewport below an in-flight pose. Clamp the drawn
    // geometry immediately while its target continues the shared morph.
    pose.width = std::min(pose.width, frame.display.x);
    pose.height = std::min(pose.height, frame.display.y);
    pose.x = std::clamp(pose.x, 0.0F, std::max(0.0F, frame.display.x - pose.width));
    pose.y = std::clamp(pose.y, 0.0F, std::max(0.0F, frame.display.y - pose.height));
    hit_pose_ = available() && alpha > 0.1F ? std::optional{pose} : std::nullopt;
    const bool hovered = frame.pointer.valid && covers(frame.pointer.position) && toasts.empty();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const ImVec2 min{pose.x, pose.y}, max{pose.x + pose.width, pose.y + pose.height};
    const ImVec2 center{(min.x + max.x) * 0.5F, (min.y + max.y) * 0.5F};
    const ShadowParams shadow{0.3F, 8, 0, 3, IM_COL32(12, 18, 35, 160)};
    const ImVec2 source{100, 100};
    const auto radii = CornerRadii::all(18);
    const ImVec2 circle_source{64, 64};
    const auto circle_radii = CornerRadii::all(32);
    shadows.prepare(source, radii, shadow);
    shadows.prepare(circle_source, circle_radii, shadow);
    const float plate_shadow = (1 - pose.compact) * appearance.round;
    shadows.draw_stretched(list, min, max, source, radii, shadow, plate_shadow * alpha);
    shadows.draw_stretched(list, min, max, circle_source, circle_radii, shadow, (1 - plate_shadow) * alpha);
    const SocialFonts::Scope scope(fonts, list);
    list->AddRectFilled(min, max, to_u32({13.0F/255, 17.0F/255, 27.0F/255, 238.0F/255}, alpha), pose.radius);
    list->AddRect(min, max, to_u32({230.0F/255, 239.0F/255, 1, (hovered ? 75.0F : 35.0F)/255}, alpha),
                  pose.radius, 0, std::max(0.5F, s * 0.7F));
    list->PushClipRect(min, max, true);
    const auto rainbow = [&](float p) {
        const float hue = static_cast<float>(std::fmod(0.92 - p + hue_time_ * 0.018 + 2, 1.0));
        return hsv_to_rgb({hue, 0.67F, 1}, 1);
    };
    // The toast's coloured dot rides its left cap until the content takes over.
    const float dot = alpha * (1 - appearance.content);
    if (dot > 0) {
        list->AddCircleFilled({min.x + pose.height * 0.5F, center.y},
            theme::social::metrics::toast_dot * 0.5F * s * appearance.scale, to_u32(rainbow(0), dot));
    }
    const float content = alpha * appearance.content;
    const float track_progress = tracks_.progress();
    const float expanded = std::clamp(1 - pose.compact * 1.65F, 0.0F, 1.0F) * content;
    if (expanded > 0) {
        const float travel = std::max(1.0F, pose.width - 28 * s);
        // Keep the frame and spectrum still; only the song's art/text slides.
        // The inset clip also keeps a moving image clear of rounded corners.
        list->PushClipRect({min.x + 14 * s, min.y}, {std::max(min.x + 14 * s, max.x - 14 * s), max.y}, true);
        const auto song = [&](const music::MusicSnapshot& track, float offset) {
            const float art_size = 52 * s;
            const ImVec2 a{min.x + 14 * s + offset, min.y + 10 * s}, b{a.x + art_size, a.y + art_size};
            if (const auto texture = art_texture(track.art)) {
                ImVec2 uv0{0, 0}, uv1{1, 1};
                if (track.art->width > track.art->height) {
                    const float crop = static_cast<float>(track.art->height) / track.art->width;
                    uv0.x = (1 - crop) * 0.5F; uv1.x = 1 - uv0.x;
                } else {
                    const float crop = static_cast<float>(track.art->width) / track.art->height;
                    uv0.y = (1 - crop) * 0.5F; uv1.y = 1 - uv0.y;
                }
                list->AddImageRounded(texture, a, b, uv0, uv1, to_u32(theme::white, expanded), 10 * s);
            } else {
                list->AddRectFilled(a, b, to_u32({0.14F, 0.18F, 0.27F, 1}, expanded), 10 * s);
                music_icon(list, {(a.x+b.x)*0.5F,(a.y+b.y)*0.5F}, 23*s, expanded);
            }
            const float x = b.x + 12 * s, room = std::max(0.0F, full_pose.width - 92 * s);
            fonts.draw(list, SocialText::Title, x, min.y + 13 * s, to_u32(theme::white, expanded),
                fonts.ellipsize(SocialText::Title, track.title, room, scratch_));
            fonts.draw(list, SocialText::Small, x, min.y + 38 * s, to_u32({0.68F, 0.73F, 0.82F, 1}, expanded),
                fonts.ellipsize(SocialText::Small, track.artist, room, scratch_));
        };
        if (tracks_.previous()) song(*tracks_.previous(), -travel * track_progress);
        song(*tracks_.current(), tracks_.previous() ? travel * (1 - track_progress) : 0);
        list->PopClipRect();
        const float width = std::max(1.0F, full_pose.width - 28 * s);
        const float step = width / static_cast<float>(music::bar_count);
        for (std::size_t i = 0; i < music::bar_count; ++i) {
            const float h = (3 + levels_[i] * 25) * s * (1 - pose.compact * 0.6F);
            const float x0 = min.x + 14 * s + (static_cast<float>(i) + 0.5F) * step;
            const float y = min.y + 80 * s;
            bar(list, {x0,y-h*0.5F}, {x0,y+h*0.5F}, std::max(0.5F, step * 0.42F),
                rainbow(static_cast<float>(i) / music::bar_count), expanded);
        }
    }
    const float compact = std::clamp((pose.compact - 0.25F) / 0.75F, 0.0F, 1.0F) * content;
    if (compact > 0) {
        const float radius = 19 * s, travel = radius * 2;
        const auto song = [&](const music::MusicSnapshot& track, float offset) {
            if (const auto texture = art_texture(track.art))
                sliding_art(list, texture, *track.art, center, radius, art_rotation_, compact, offset);
            else {
                list->PushClipRect({center.x - radius, center.y - radius}, {center.x + radius, center.y + radius}, true);
                music_icon(list, {center.x + offset, center.y}, 22 * s, compact);
                list->PopClipRect();
            }
        };
        if (tracks_.previous()) song(*tracks_.previous(), -travel * track_progress);
        song(*tracks_.current(), tracks_.previous() ? travel * (1 - track_progress) : 0);
        constexpr std::size_t count = 36;
        for (std::size_t i = 0; i < count; ++i) {
            const float angle = static_cast<float>(i) / count * 2 * pi - pi * 0.5F;
            const float level = levels_[i * music::bar_count / count];
            // Half-stroke inset keeps round caps inside the 64 px circle.
            const float inner = 24 * s, outer = (25 + level * 5.5F) * s;
            const auto point = [&](float r) { return ImVec2{center.x+std::cos(angle)*r, center.y+std::sin(angle)*r}; };
            bar(list, point(inner), point(outer), 1.8F*s, rainbow(static_cast<float>(i)/count), compact);
        }
    }
    list->PopClipRect();
}
} // namespace medicon::gui
