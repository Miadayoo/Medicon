#pragma once

#include "medicon/gui/GuiMath.hpp"

#include <chrono>

namespace medicon::gui {

// Animation time source. Real time uses the steady (QPC-backed) clock; a fixed
// step makes screenshots and tests reproducible.
class FrameClock {
public:
    // Returns dt = clamp(now - last, 0, 0.1 s).
    float tick() {
        if (fixed_step_ > 0.0F) {
            elapsed_ = fixed_step_;
            return clamp_dt(fixed_step_);
        }
        const auto now = std::chrono::steady_clock::now();
        if (!started_) {
            started_ = true;
            last_ = now;
            elapsed_ = 0.0F;
            return 0.0F;
        }
        const double seconds = std::chrono::duration<double>(now - last_).count();
        last_ = now;
        elapsed_ = static_cast<float>(std::max(0.0, seconds));
        return clamp_dt(seconds);
    }
    void set_fixed_step(float seconds) { fixed_step_ = seconds; }
    float fixed_step() const { return fixed_step_; }
    // Wall time for animations which must complete even after a slow frame.
    float elapsed() const noexcept { return elapsed_; }

private:
    std::chrono::steady_clock::time_point last_{};
    bool started_ = false;
    float fixed_step_ = 0.0F;
    float elapsed_ = 0.0F;
};

} // namespace medicon::gui
