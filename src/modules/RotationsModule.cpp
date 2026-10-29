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

} // namespace medicon
