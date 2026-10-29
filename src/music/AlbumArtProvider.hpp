#pragma once

#include <medicon/music/MusicSessionManager.hpp>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include <chrono>
#include <stop_token>
#include <thread>

namespace medicon::music {

// Dedicated worker only. Cancellation/timeout bounds shutdown even when an
// unresponsive player never completes its WinRT request.
template<class Operation>
auto await_result(const Operation& operation, std::stop_token stop) {
    if (!operation) throw winrt::hresult_invalid_argument(L"Missing media operation");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (operation.Status() == winrt::Windows::Foundation::AsyncStatus::Started) {
        if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) {
            operation.Cancel();
            throw winrt::hresult_canceled();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return operation.GetResults();
}

class AlbumArtProvider {
public:
    std::shared_ptr<const AlbumArt> load(
        const winrt::Windows::Storage::Streams::IRandomAccessStreamReference& thumbnail,
        std::stop_token stop);
private:
    // Keep a single CPU image across playback-only updates and renderer loss.
    std::shared_ptr<const AlbumArt> cached_;
};

} // namespace medicon::music
