#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

namespace medicon::social {

// Reconnect delays (spec 2.6): 1, 2, 4, 8, 16, then 30 s for every later
// attempt, each scaled by a random factor in [0.8, 1.2] so clients that lost the
// same server do not reconnect in lockstep. reset() after a hello.
class Backoff {
public:
    // unit_random is a uniform sample in [0, 1).
    std::chrono::milliseconds next(double unit_random) noexcept {
        constexpr std::array<int, 6> seconds{1, 2, 4, 8, 16, 30};
        const int base = seconds[static_cast<std::size_t>(std::min<int>(attempt_, seconds.size() - 1))];
        ++attempt_;
        const double jitter = 0.8 + 0.4 * std::clamp(unit_random, 0.0, 1.0);
        return std::chrono::milliseconds(static_cast<long long>(std::llround(base * 1000.0 * jitter)));
    }
    void reset() noexcept { attempt_ = 0; }
    int attempt() const noexcept { return attempt_; }

private:
    int attempt_ = 0;
};

} // namespace medicon::social
