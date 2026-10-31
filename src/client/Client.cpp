#include <medicon/client/Client.hpp>
#include <medicon/modules/BuiltinModules.hpp>
#include <medicon/modules/GameplayModule.hpp>
#include <medicon/modules/CustomModelProbeModule.hpp>
#include <medicon/modules/RotationsModule.hpp>
#include <medicon/modules/DerpModule.hpp>
#include <medicon/modules/AimbotModule.hpp>
#include <medicon/modules/KillauraModule.hpp>
#include <medicon/modules/SprintModule.hpp>
#include <medicon/modules/VelocityModule.hpp>
#include <medicon/gui/GameplayVisuals.hpp>
#include <medicon/memory/module_view.hpp>
#include <Windows.h>
#include <ShlObj.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace medicon {

namespace {

class RotationVariableRoute final : public sdk::GameplayAccess {
public:
    RotationVariableRoute(sdk::GameplayAccess& game, RotationsModule& rotations) noexcept
        : game_(game), rotations_(rotations) {
        const auto& frame = game_.frame();
        rotations_.set_server_rotation(frame.yaw, frame.pitch);
    }

    const sdk::GameplayFrame& frame() const noexcept override { return game_.frame(); }
    void begin_rotation_control(bool enabled) noexcept override { game_.begin_rotation_control(enabled); }
    void finish_rotation_control() noexcept override { game_.finish_rotation_control(); }
    void finish_rotation_control(bool requested) noexcept override { game_.finish_rotation_control(requested); }

    bool rotate(float yaw, float pitch) override {
        rotations_.set_server_rotation(yaw, pitch);
        const bool applied = game_.rotate(server_yaw(), server_pitch());
        rotation_requested_ = rotation_requested_ || applied;
        return applied;
    }

    bool rotate_head_and_body(float yaw) override {
        const float pitch = rotations_.server_rotation_valid.load(std::memory_order_acquire)
            ? server_pitch() : frame().pitch;
        rotations_.set_server_rotation(yaw, pitch);
        const bool applied = game_.rotate_head_and_body(server_yaw(), server_pitch());
        rotation_requested_ = rotation_requested_ || applied;
        return applied;
    }

    bool rotate_head_and_body(float yaw, float pitch) override {
        rotations_.set_server_rotation(yaw, pitch);
        const bool applied = game_.rotate_head_and_body(server_yaw(), server_pitch());
        rotation_requested_ = rotation_requested_ || applied;
        return applied;
    }

    [[nodiscard]] bool has_rotation_request() const noexcept { return rotation_requested_; }

    bool restore_view() {
        const auto& current = frame();
        rotations_.set_server_rotation(current.yaw, current.pitch);
        return game_.rotate_head_and_body(server_yaw(), server_pitch());
    }

    bool attack(std::uintptr_t actor_id) override { return game_.attack(actor_id); }
    bool set_sprinting(bool sprinting) override { return game_.set_sprinting(sprinting); }
    bool set_velocity(sdk::Vec3 velocity) override { return game_.set_velocity(velocity); }
    sdk::KnockbackApplyResult apply_knockback(sdk::Vec3 velocity, std::int64_t received_timestamp) override {
        return game_.apply_knockback(velocity, received_timestamp);
    }
    bool send_latency_reply(const sdk::LatencyReply& reply) override { return game_.send_latency_reply(reply); }
    bool swing() override { return game_.swing(); }
    bool place_block(sdk::BlockPos position, bool auto_select) override {
        return game_.place_block(position, auto_select);
    }
    bool replaceable(sdk::BlockPos position) const override { return game_.replaceable(position); }

private:
    float server_yaw() const noexcept { return rotations_.server_yaw.load(std::memory_order_relaxed); }
    float server_pitch() const noexcept { return rotations_.server_pitch.load(std::memory_order_relaxed); }

    sdk::GameplayAccess& game_;
    RotationsModule& rotations_;
    bool rotation_requested_{};
};

} // namespace

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
    auto* rotations = dynamic_cast<RotationsModule*>(modules_.find(RotationsModule::module_id));
    const auto* derp = dynamic_cast<DerpModule*>(modules_.find(DerpModule::module_id));
    const auto* aimbot = dynamic_cast<AimbotModule*>(modules_.find(AimbotModule::module_id));
    const auto* killaura = dynamic_cast<KillauraModule*>(modules_.find(KillauraModule::module_id));
    auto* sprint = dynamic_cast<SprintModule*>(modules_.find(SprintModule::module_id));
    auto* velocity = dynamic_cast<VelocityModule*>(modules_.find(VelocityModule::module_id));
    const auto* killaura_rotate = killaura ? std::get_if<Choice>(&killaura->rotate.value) : nullptr;
    const bool has_rotation_requester = (derp && derp->enabled()) || (aimbot && aimbot->enabled()) ||
        (killaura && killaura->enabled() && killaura_rotate && killaura_rotate->index == 1);
    // Feature rotation modes (for example Killaura's Simple) own their angle
    // requests independently. Third-person interpolation remains an explicit
    // Rotations-module feature.
    const bool rotation_render_enabled = rotations && rotations->enabled() &&
        (has_rotation_requester || rotations->always.get<bool>());
    const bool rotation_control_enabled = has_rotation_requester || rotation_render_enabled;
    game.begin_rotation_control(rotation_control_enabled, rotation_render_enabled);
    RotationVariableRoute rotation_route(game, *rotations);
    const bool gameplay_actions_allowed = allow_actions && !capturing_input();
    if (sprint) sprint->update(rotation_route, gameplay_actions_allowed);
    if (velocity) velocity->update(rotation_route, gameplay_actions_allowed);
    if (gameplay_actions_allowed) {
        for (const auto& module : modules_.all()) {
            if (!module->enabled()) continue;
            if (auto* gameplay = dynamic_cast<GameplayModule*>(module.get()); gameplay && gameplay != sprint && gameplay != velocity)
                gameplay->game_tick(rotation_route, seconds);
        }
    }
    if (rotation_control_enabled && !rotation_route.has_rotation_request())
        (void)rotation_route.restore_view();
    game.finish_rotation_control(rotation_route.has_rotation_request());
    gameplay_frame_ = game.frame();
    gameplay_age_ = 0.0F;
}

void Client::clear_gameplay() noexcept {
    if (auto* velocity = dynamic_cast<VelocityModule*>(modules_.find(VelocityModule::module_id)))
        velocity->reset_delay();
    if (auto* rotations = dynamic_cast<RotationsModule*>(modules_.find(RotationsModule::module_id)))
        rotations->reset_render();
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
