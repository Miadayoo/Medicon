#include "medicon/modules/RotationsModule.hpp"

#include <algorithm>
#include <cmath>

namespace medicon {

RotationsModule::RotationsModule()
    : Module(module_id, "Rotations", "Routes gameplay yaw/pitch requests and controls their render interpolation.",
             Category::Misc),
      always(add_setting({"always", "Always", "Keep render rotation active and aligned with the current view when no feature requests a target.", false})),
      speed(add_setting({"speed", "Speed", "Maximum render rotation speed in degrees per second; it is reached at an angular distance of 180 degrees.", 1300.0F, 30.0F, 1440.0F, 10.0F})) {}

void RotationsModule::set_server_rotation(float yaw, float pitch) noexcept {
    if (!std::isfinite(yaw) || !std::isfinite(pitch)) return;
    server_yaw.store(std::remainder(yaw, 360.0F), std::memory_order_relaxed);
    server_pitch.store(std::clamp(pitch, -90.0F, 90.0F), std::memory_order_relaxed);
    server_rotation_valid.store(true, std::memory_order_release);
}

void RotationsModule::reset_render() noexcept {
    render_actor_ = 0;
    rendered_ = {};
}

void RotationsModule::on_enable() noexcept {
    reset_render();
    server_rotation_valid.store(false, std::memory_order_release);
    render_enabled_.store(true, std::memory_order_release);
}

void RotationsModule::on_disable() noexcept {
    render_enabled_.store(false, std::memory_order_release);
    reset_render();
    server_rotation_valid.store(false, std::memory_order_release);
}

bool RotationsModule::update_render(std::uintptr_t actor, float view_yaw, float view_pitch,
                                    float delta_seconds, RenderAngles& result) noexcept {
    if (!enabled() || !actor || !server_rotation_valid.load(std::memory_order_acquire) ||
        !std::isfinite(view_yaw) || !std::isfinite(view_pitch)) {
        reset_render();
        return false;
    }
    const float yaw = server_yaw.load(std::memory_order_relaxed);
    const float pitch = server_pitch.load(std::memory_order_relaxed);
    if (!std::isfinite(yaw) || !std::isfinite(pitch)) { reset_render(); return false; }
    if (render_actor_ != actor) {
        render_actor_ = actor;
        rendered_ = {std::remainder(view_yaw, 360.0F), std::clamp(view_pitch, -90.0F, 90.0F)};
    }
    const float delta = std::isfinite(delta_seconds) ? std::clamp(delta_seconds, 0.0F, 0.1F) : 0.0F;
    const float configured_speed = speed.get<float>();
    const float maximum_speed = std::isfinite(configured_speed) ? std::clamp(configured_speed, 0.0F, 10000.0F) : 1300.0F;
    const float yaw_delta = std::remainder(yaw - rendered_.yaw, 360.0F);
    const float pitch_delta = std::clamp(pitch, -90.0F, 90.0F) - rendered_.pitch;
    const float distance = std::hypot(yaw_delta, pitch_delta);
    if (distance > 0.0001F && delta > 0.0F) {
        const float step = maximum_speed * std::min(distance / 180.0F, 1.0F) * delta;
        const float fraction = std::min(step / distance, 1.0F);
        rendered_.yaw = std::remainder(rendered_.yaw + yaw_delta * fraction, 360.0F);
        rendered_.pitch = std::clamp(rendered_.pitch + pitch_delta * fraction, -90.0F, 90.0F);
    }
    result = rendered_;
    return true;
}

} // namespace medicon
