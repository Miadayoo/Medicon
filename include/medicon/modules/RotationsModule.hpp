#pragma once

#include "medicon/core/Module.hpp"

#include <atomic>

namespace medicon {

class RotationsModule final : public Module {
public:
    static constexpr const char* module_id = "rotations";
    RotationsModule();

    Setting& always;
    Setting& speed;

    // Authoritative requested angles passed from gameplay modules to the
    // native gameplay bridge; render interpolation keeps its own state.
    std::atomic<float> server_yaw{};
    std::atomic<float> server_pitch{};
    std::atomic_bool server_rotation_valid{};

    void set_server_rotation(float yaw, float pitch) noexcept;
};

} // namespace medicon
