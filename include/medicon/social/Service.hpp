#pragma once

#include "medicon/social/Events.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace medicon::social {

// The network half of Social: SocialConfig, LicenseStore, SocialNet and
// SocialApi behind one Channel (spec 4.2). Start it after the client finished
// initializing and never from DllMain; it only spawns threads.
class SocialService {
public:
    struct Options {
        std::filesystem::path directory; // holds social.json and license.bin
        std::string client_version;
        // Called from network threads; must be thread-safe. Receives no keys,
        // tokens or message bodies.
        std::function<void(std::string_view)> log;
    };

    explicit SocialService(Options options);
    ~SocialService();
    SocialService(const SocialService&) = delete;
    SocialService& operator=(const SocialService&) = delete;

    void start();
    // Non-blocking. For the game window going away, when the process ends soon.
    void request_stop() noexcept;
    // Closes the WebSocket and joins the threads within 2 s (spec 4.3). Returns
    // false when a thread had to be detached. Never call it from DllMain.
    bool stop();

    // Valid for the lifetime of this object.
    Channel& channel() noexcept;

    // %APPDATA%\Medicon (spec 4.10).
    static std::filesystem::path default_directory();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A short chime played asynchronously from memory (spec 4.7).
void play_notification_sound() noexcept;

} // namespace medicon::social
