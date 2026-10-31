#pragma once

#include <medicon/alt/Auth.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace medicon::alt {

// The worker behind the AltManager: it owns alts.bin and runs one Microsoft
// sign-in at a time. Idle until the first command; it opens no connection and no
// window of its own accord. Credentials stay in the service; the native login
// hook can request a signed token, but the render/UI channel never receives it.
class AltService {
public:
    struct Options {
        std::filesystem::path directory; // holds alts.bin
        std::string client_version;
        // Called from the worker; must be thread-safe. Receives no tokens.
        std::function<void(std::string_view)> log;
        std::string application_id = std::string(client_id);
        // Optional transport/desktop substitute for end-to-end worker tests.
        std::function<std::unique_ptr<SignInHost>(Channel&)> make_sign_in_host = {};
    };

    explicit AltService(Options options);
    ~AltService();
    AltService(const AltService&) = delete;
    AltService& operator=(const AltService&) = delete;

    void start();
    // Non-blocking.
    void request_stop() noexcept;
    bool finished() const noexcept;
    // Returns false when the worker, stuck in a network call, had to be detached.
    bool stop(std::chrono::milliseconds timeout);

    // Valid for the lifetime of this object.
    Channel& channel() noexcept;
    void set_game_bridge(bool available);
    // Used before the game's own token request starts, so its native response
    // feeds both NetherNet assertions and the later LoginPacket.
    std::optional<std::string> native_authorization(std::string_view url);
    void native_auth_result(unsigned status);
    // Validates the native token without issuing a different token here.
    // nullopt: preserve native login; empty string: reject stale/wrong identity.
    std::optional<std::string> connection_token(std::string_view native_token);
    void connection_used(std::string_view token);

private:
    struct State;
    std::shared_ptr<State> state_;
    std::thread thread_;
};

} // namespace medicon::alt
