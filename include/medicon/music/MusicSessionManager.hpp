#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace medicon::music {

struct AlbumArt {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};
struct MusicSnapshot {
    bool available = false;
    bool playing = false;
    std::string title, artist, source_app;
    std::shared_ptr<const AlbumArt> art;
};

// Owns its MTA worker and all WinRT event subscriptions. Snapshots are immutable;
// the render thread never waits for a WinRT operation or accesses a session.
class MusicSessionManager {
public:
    MusicSessionManager();
    ~MusicSessionManager();
    MusicSessionManager(const MusicSessionManager&) = delete;
    MusicSessionManager& operator=(const MusicSessionManager&) = delete;
    std::shared_ptr<const MusicSnapshot> snapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace medicon::music
