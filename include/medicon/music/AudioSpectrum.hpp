#pragma once

#include <array>
#include <cstddef>
#include <span>

namespace medicon::music {

inline constexpr std::size_t bar_count = 48;
using AudioLevels = std::array<float, bar_count>;

// Hann-windowed, overlapping FFT. Input is mono PCM in [-1,1]. No allocation
// occurs while processing samples. Bands use logarithmic frequency spacing.
class AudioSpectrum {
public:
    static constexpr std::size_t fft_size = 1024;
    void reset(float sample_rate = 48000.0F);
    void push(std::span<const float> samples);
    const AudioLevels& levels() const noexcept { return levels_; }
private:
    void analyze();
    float sample_rate_ = 48000.0F;
    std::array<float, fft_size> samples_{};
    std::size_t filled_ = 0;
    AudioLevels levels_{};
};

// Frame-rate independent attack/release, also used when capture stops delivering
// packets. Paused media is explicitly gated to silence by the widget.
void smooth_levels(AudioLevels& current, const AudioLevels& target, float dt);

} // namespace medicon::music
