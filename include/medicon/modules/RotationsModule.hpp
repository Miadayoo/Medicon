#pragma once

#include "medicon/core/Module.hpp"

#include <atomic>
#include <cstdint>

namespace medicon {

class RotationsModule final : public Module {
public:
    static constexpr const char* module_id = "rotations";
    RotationsModule();

    Setting& always;
    Setting& speed;
    Setting& sprint_fix;

    // Authoritative requested angles passed from gameplay modules to the
    // native gameplay bridge; render interpolation keeps its own state.
    std::atomic<float> server_yaw{};
    std::atomic<float> server_pitch{};
    std::atomic_bool server_rotation_valid{};

    void set_server_rotation(float yaw, float pitch) noexcept;

    struct RenderAngles { float yaw{}, pitch{}; };
    // Present-thread state shared by all rotation requesters. Rendering owns
    // both axes here; no gameplay module writes a model's render pitch.
    bool update_render(std::uintptr_t actor, float view_yaw, float view_pitch,
                       float delta_seconds, RenderAngles& result) noexcept;
    void reset_render() noexcept;
    [[nodiscard]] bool render_enabled() const noexcept { return render_enabled_.load(std::memory_order_acquire); }

protected:
    void on_enable() noexcept override;
    void on_disable() noexcept override;

private:
    std::uintptr_t render_actor_{};
    RenderAngles rendered_;
    std::atomic_bool render_enabled_{};
};

} // namespace medicon
