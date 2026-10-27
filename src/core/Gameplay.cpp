#include <medicon/sdk/Gameplay.hpp>

#include <cmath>

namespace medicon::sdk {

bool world_to_screen(const GameplayFrame& frame, Vec3 world, Vec2 viewport, Vec2& screen) noexcept {
    if (!frame.ready || !frame.projection_ready || !std::isfinite(viewport.x) ||
        !std::isfinite(viewport.y) || viewport.x <= 0 || viewport.y <= 0 ||
        !std::isfinite(frame.fov.x) || !std::isfinite(frame.fov.y) ||
        frame.fov.x <= 0 || frame.fov.y <= 0) return false;
    const Vec3 relative{world.x - frame.camera.x, world.y - frame.camera.y, world.z - frame.camera.z};
    const auto& m = frame.view_matrix;
    const float x = m[0] * relative.x + m[1] * relative.y + m[2] * relative.z + m[3];
    const float y = m[4] * relative.x + m[5] * relative.y + m[6] * relative.z + m[7];
    const float z = m[8] * relative.x + m[9] * relative.y + m[10] * relative.z + m[11];
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || z >= -0.01F) return false;
    const Vec2 projected{viewport.x * 0.5F * (1.0F + x / -z * frame.fov.x),
                         viewport.y * 0.5F * (1.0F - y / -z * frame.fov.y)};
    if (!std::isfinite(projected.x) || !std::isfinite(projected.y)) return false;
    screen = projected;
    return true;
}

} // namespace medicon::sdk
