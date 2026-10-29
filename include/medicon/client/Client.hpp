#pragma once

#include <medicon/core/ConfigStore.hpp>
#include <medicon/core/ModuleManager.hpp>
#include <medicon/commands/CommandManager.hpp>
#include <medicon/gui/ClickGui.hpp>
#include <medicon/gui/Overlay.hpp>
#include <medicon/hooks/HookManager.hpp>
#include <medicon/memory/signature_registry.hpp>
#include <medicon/sdk/Gameplay.hpp>
#include <filesystem>
#include <string>

namespace medicon {

// One client per host. Call tick/render/key_event/stop on the host's UI thread.
// A native host may call game_tick from baseTick while holding its render lock.
class Client final {
public:
    explicit Client(std::filesystem::path config_path);
    ~Client();
    bool start();
    bool stop();
    void tick(float delta_seconds);
    void game_tick(sdk::GameplayAccess& game, float delta_seconds, bool allow_actions = true);
    void clear_gameplay() noexcept;
    // The copied frame the overlay draws, while it has a camera; a native host
    // refreshes its camera fields every Present (BedrockGameplay::refresh_view).
    [[nodiscard]] sdk::GameplayFrame* gameplay_view() noexcept {
        return gameplay_frame_.ready && gameplay_frame_.projection_ready ? &gameplay_frame_ : nullptr;
    }
    // The copied world snapshot, also when the camera profile is unavailable.
    [[nodiscard]] const sdk::GameplayFrame* gameplay_snapshot() const noexcept {
        return gameplay_frame_.ready ? &gameplay_frame_ : nullptr;
    }
    // Before ImGui::NewFrame(); true when the host must re-upload its font texture.
    bool prepare_frame();
    void render(); // An ImGui context and an active NewFrame() are required.
    // Returns true when the GUI owns the key and the host must not forward it to the game.
    bool key_event(int key, bool pressed, bool repeat = false);
    // Execute on the same thread as tick/render/key_event. Saves changed binds
    // immediately and reports a persistence failure without overwriting bad JSON.
    CommandResult execute_command(std::string_view input);
    [[nodiscard]] bool capturing_input() const noexcept { return overlay_.gui().open() || overlay_.main_menu().active() || overlay_.social().screen_visible(); }
    bool scan_signatures(); // Explicit, potentially expensive; never calls a game function.
    [[nodiscard]] bool running() const noexcept { return running_; }
    [[nodiscard]] ModuleManager& modules() noexcept { return modules_; }
    [[nodiscard]] ClickGui& gui() noexcept { return overlay_.gui(); }
    [[nodiscard]] gui::Overlay& overlay() noexcept { return overlay_; }
    [[nodiscard]] HookManager& hooks() noexcept { return hooks_; }
    [[nodiscard]] const memory::SignatureRegistry& signatures() const noexcept { return signatures_; }
    [[nodiscard]] const std::string& last_error() const noexcept { return error_; }
    [[nodiscard]] static std::filesystem::path default_config_path();
private:
    void save_config();

    ModuleManager modules_;
    ConfigStore config_;
    CommandManager commands_;
    gui::Overlay overlay_;
    HookManager hooks_;
    memory::SignatureRegistry signatures_;
    sdk::GameplayFrame gameplay_frame_;
    float gameplay_age_ = 1.0F;
    float elapsed_ = 0.0F;
    std::string error_;
    bool running_{};
    bool config_writable_{true};
};

} // namespace medicon
