#include <medicon/client/Client.hpp>
#include <medicon/modules/BuiltinModules.hpp>
#include <medicon/modules/GameplayModules.hpp>
#include <medicon/modules/CustomModelProbeModule.hpp>
#include <medicon/gui/GameplayVisuals.hpp>
#include <medicon/memory/module_view.hpp>
#include <Windows.h>
#include <ShlObj.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace medicon {

Client::Client(std::filesystem::path path)
    : config_(std::move(path)), commands_(modules_), overlay_(modules_) {
    register_builtin_modules(modules_);
    // The GUI requests saves on discrete changes (drag end, toggle, close) — never per frame.
    overlay_.set_save_handler([this] { save_config(); });
}

Client::~Client() { stop(); }

void Client::save_config() {
    // A config that failed to load is never overwritten automatically.
    if (!running_ || !config_writable_) return;
    if (!config_.save(modules_, error_)) overlay_.gui().set_status(error_);
}

bool Client::start() {
    if (running_) return true;
    error_.clear();
    config_writable_ = config_.load(modules_, error_);
    // Keep defaults and surface a malformed-config error without destroying the file.
    overlay_.gui().set_status(config_writable_ ? "Foundation ready / host renderer" : error_);
    // A saved enabled ClickGUI is a previous session's open state, not a request
    // to capture input on injection. Only its bound key opens the new session.
    overlay_.gui().set_open(false);
    // CustomModel starts OFF on injection, even if the previous session saved
    // it ON. F8 enables it after the model's native resources are prepared.
    if (auto* probe = modules_.find(CustomModelProbeModule::module_id)) probe->set_enabled(false);
    overlay_.set_main_menu_active(false);
    overlay_.start_splash();
    running_ = true;
    return true;
}

bool Client::stop() {
    if (!running_) return true;
    if (!hooks_.clear(error_)) return false;
    overlay_.set_main_menu_active(false);
    // Save enabled state before shutdown invokes module disable callbacks.
    const bool saved = !config_writable_ || config_.save(modules_, error_);
    modules_.shutdown();
    clear_gameplay();
    running_ = false;
    return saved;
}

void Client::tick(float seconds) {
    if (!running_) return;
    if (!std::isfinite(seconds) || seconds < 0.0F) seconds = 0.0F;
    elapsed_ += seconds;
    gameplay_age_ += seconds;
    if (gameplay_age_ > 0.25F) clear_gameplay();
    modules_.tick(seconds);
}

void Client::game_tick(sdk::GameplayAccess& game, float seconds, bool allow_actions) {
    if (!running_) return;
    if (!game.frame().ready) { clear_gameplay(); return; }
    if (!std::isfinite(seconds) || seconds <= 0.0F) seconds = 0.05F;
    // The host serializes this game-thread entry point with UI settings edits.
    // No game-owned pointers are used by tick() or render().
    if (allow_actions && !capturing_input()) {
        for (const auto& module : modules_.all()) {
            if (!module->enabled()) continue;
            if (auto* gameplay = dynamic_cast<GameplayModule*>(module.get()))
                gameplay->game_tick(game, seconds);
        }
    }
    gameplay_frame_ = game.frame();
    gameplay_age_ = 0.0F;
}

void Client::clear_gameplay() noexcept {
    gameplay_frame_.ready = false;
    gameplay_frame_.projection_ready = false;
    gameplay_frame_.actors.clear();
    gameplay_age_ = 1.0F;
}

bool Client::prepare_frame() {
    if (!running_) return false;
    return overlay_.prepare_frame();
}

void Client::render() {
    if (!running_) return;
    // How far this frame is between the last game tick and the next (20 per second).
    const float partial_tick = std::clamp(gameplay_age_ / 0.05F, 0.0F, 1.0F);
    gui::render_gameplay_visuals(modules_, gameplay_frame_, elapsed_, partial_tick);
    modules_.render();
    overlay_.render();
}

bool Client::key_event(int key, bool pressed, bool repeat) {
    if (!running_) return false;
    return overlay_.key_event(key, pressed, repeat);
}

CommandResult Client::execute_command(std::string_view input) {
    if (!running_) return {true, false, false, "Medicon is not initialized."};
    auto result = commands_.execute(input);
    if (result.changed) {
        modules_.reset_keys();
        if (!config_writable_) {
            result.success = false;
            result.message += " Changed for this session; config was not saved because the existing file is invalid.";
        } else if (!config_.save(modules_, error_)) {
            result.success = false;
            result.message += " Changed for this session; could not save: " + error_;
        }
    }
    return result;
}

bool Client::scan_signatures() {
    error_.clear();
    const auto module = memory::current_process_module(&error_);
    if (!module) return false;
    signatures_.scan(memory::solstice_signatures(), module->executable_regions, module->image);
    return true;
}

std::filesystem::path Client::default_config_path() {
    PWSTR path = nullptr;
    const auto result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &path);
    if (FAILED(result)) throw std::runtime_error("Cannot locate LocalAppData for the client configuration.");
    const auto config = std::filesystem::path(path) / L"Medicon" / L"config.json";
    CoTaskMemFree(path);
    return config;
}

} // namespace medicon
