#pragma once

#include <winrt/Windows.Media.Control.h>

namespace medicon::music::detail {
// Kept separate so disconnected/empty WinRT responses can be exercised without
// controlling a user's player or relying on a race in the Windows media broker.
winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus
playback_status(const winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession& session) noexcept;
}
