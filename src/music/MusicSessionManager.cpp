#include <medicon/music/MusicSessionManager.hpp>
#include "AlbumArtProvider.hpp"
#include "SessionState.hpp"

#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace medicon::music {
winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus
detail::playback_status(const winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession& session) noexcept {
    using Status = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;
    if (!session) return Status::Closed;
    try {
        // A disappearing session can return S_OK with no playback-info object.
        // Dereferencing that object is an access violation, not an HRESULT
        // exception, so catch (...) alone cannot protect the host process.
        const auto info = session.GetPlaybackInfo();
        return info ? info.PlaybackStatus() : Status::Closed;
    } catch (...) { return Status::Closed; }
}

namespace {
using namespace winrt::Windows::Media::Control;
using Status = GlobalSystemMediaTransportControlsSessionPlaybackStatus;
using Session = GlobalSystemMediaTransportControlsSession;
using detail::playback_status;

struct Shared {
    std::atomic<bool> changed{true}, metadata{true};
    std::mutex mutex;
    std::condition_variable_any wake;
    std::shared_ptr<const MusicSnapshot> latest = std::make_shared<MusicSnapshot>();
    void signal(bool media) {
        if (media) metadata.store(true);
        changed.store(true);
        wake.notify_all();
    }
    void publish(MusicSnapshot value) {
        std::lock_guard lock(mutex);
        latest = std::make_shared<MusicSnapshot>(std::move(value));
    }
};

Session choose(const GlobalSystemMediaTransportControlsSessionManager& manager) {
    auto current = manager.GetCurrentSession();
    if (current && playback_status(current) == Status::Playing) return current;
    const auto sessions = manager.GetSessions();
    if (sessions)
        for (const auto& candidate : sessions) if (playback_status(candidate) == Status::Playing) return candidate;
    if (current && playback_status(current) == Status::Paused) return current;
    if (sessions)
        for (const auto& candidate : sessions) if (playback_status(candidate) == Status::Paused) return candidate;
    return nullptr;
}

bool same_active_source(const Session& a, const Session& b) {
    if (!a || !b) return false;
    try {
        const auto previous = playback_status(a);
        return (previous == Status::Playing || previous == Status::Paused) &&
               !a.SourceAppUserModelId().empty() && a.SourceAppUserModelId() == b.SourceAppUserModelId();
    } catch (...) { return false; }
}

void monitor(const std::shared_ptr<Shared>& shared, std::stop_token stop) {
    const auto manager = await_result(GlobalSystemMediaTransportControlsSessionManager::RequestAsync(), stop);
    if (!manager) throw winrt::hresult_invalid_argument(L"Missing media session manager");
    auto current_changed = manager.CurrentSessionChanged(winrt::auto_revoke,
        [shared](const auto&, const auto&) { shared->signal(true); });
    auto sessions_changed = manager.SessionsChanged(winrt::auto_revoke,
        [shared](const auto&, const auto&) { shared->signal(true); });
    Session selected{nullptr};
    Session::MediaPropertiesChanged_revoker media_changed;
    Session::PlaybackInfoChanged_revoker playback_changed;
    AlbumArtProvider art;
    MusicSnapshot value;
    while (!stop.stop_requested()) {
        shared->changed.exchange(false);
        bool metadata = shared->metadata.exchange(false);
        const auto next = choose(manager);
        if (next != selected) {
            // Windows can return a new wrapper for the same source when it
            // changes from current/playing to a paused session in GetSessions.
            // Rebind its events without losing cached metadata or art.
            const bool continuing = same_active_source(selected, next);
            media_changed.revoke();
            playback_changed.revoke();
            selected = next;
            if (!continuing) {
                value = {};
            }
            metadata = true;
            if (selected) {
                media_changed = selected.MediaPropertiesChanged(winrt::auto_revoke,
                    [shared](const auto&, const auto&) { shared->signal(true); });
                playback_changed = selected.PlaybackInfoChanged(winrt::auto_revoke,
                    [shared](const auto&, const auto&) { shared->signal(false); });
            }
        }
        if (!selected) {
            shared->publish({});
        } else {
            const auto playback = playback_status(selected);
            // Keep paused sessions subscribed for resume, but only show media
            // while it is actually playing.
            value.available = playback == Status::Playing;
            value.playing = playback == Status::Playing;
            if (!value.playing) shared->publish(value); // Do not wait for metadata to report a stop.
            try { value.source_app = winrt::to_string(selected.SourceAppUserModelId()); } catch (...) {}
            if (metadata) {
                try {
                    const auto properties = await_result(selected.TryGetMediaPropertiesAsync(), stop);
                    if (!properties) throw winrt::hresult_invalid_argument(L"Missing media properties");
                    if (!shared->metadata.load()) {
                        const auto title = winrt::to_string(properties.Title());
                        const auto artist = winrt::to_string(properties.Artist());
                        if (title != value.title || artist != value.artist) value.art.reset();
                        value.title = title;
                        value.artist = artist;
                        value.available = value.playing = playback_status(selected) == Status::Playing;
                        shared->publish(value); // Display text without waiting for art decoding.
                        const auto image = art.load(properties.Thumbnail(), stop);
                        if (!shared->metadata.load()) value.art = image;
                    }
                } catch (...) {
                    shared->metadata.store(true); // Retry failed metadata, retaining the last good text.
                }
            }
            if (value.title.empty()) value.title = "Now playing";
            // Playback may have stopped while an async metadata/art read ran.
            value.available = value.playing = playback_status(selected) == Status::Playing;
            shared->publish(value);
        }
        std::unique_lock lock(shared->mutex);
        shared->wake.wait_for(lock, stop, std::chrono::seconds(1), [&] { return shared->changed.load(); });
    }
    // Revokers and sessions are destroyed here, on their owning MTA worker.
}
} // namespace

struct MusicSessionManager::Impl {
    std::shared_ptr<Shared> shared = std::make_shared<Shared>();
    std::jthread worker{[state = shared](std::stop_token stop) {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            while (!stop.stop_requested()) {
                try { monitor(state, stop); }
                catch (...) { state->publish({}); }
                if (!stop.stop_requested()) {
                    std::unique_lock lock(state->mutex);
                    state->wake.wait_for(lock, stop, std::chrono::seconds(3), [] { return false; });
                }
            }
            winrt::uninit_apartment();
        } catch (...) { state->publish({}); }
    }};
};

MusicSessionManager::MusicSessionManager() : impl_(std::make_unique<Impl>()) {}
MusicSessionManager::~MusicSessionManager() = default;
std::shared_ptr<const MusicSnapshot> MusicSessionManager::snapshot() const {
    std::lock_guard lock(impl_->shared->mutex);
    return impl_->shared->latest;
}

} // namespace medicon::music
