#include <medicon/music/AudioSpectrum.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace medicon::music {

void AudioSpectrum::reset(float sample_rate) {
    sample_rate_ = std::isfinite(sample_rate) && sample_rate >= 8000 ? sample_rate : 48000.0F;
    samples_.fill(0);
    levels_.fill(0);
    filled_ = 0;
}

void AudioSpectrum::push(std::span<const float> samples) {
    for (float sample : samples) {
        samples_[filled_++] = std::isfinite(sample) ? std::clamp(sample, -1.0F, 1.0F) : 0.0F;
        if (filled_ != fft_size) continue;
        analyze();
        std::move(samples_.begin() + fft_size / 2, samples_.end(), samples_.begin());
        filled_ = fft_size / 2;
    }
}

void AudioSpectrum::analyze() {
    constexpr float pi = std::numbers::pi_v<float>;
    std::array<std::complex<float>, fft_size> fft{};
    float mean = 0;
    for (float sample : samples_) mean += sample / static_cast<float>(fft_size);
    for (std::size_t i = 0; i < fft_size; ++i)
        fft[i] = (samples_[i] - mean) * (0.5F - 0.5F * std::cos(2 * pi * static_cast<float>(i) / (fft_size - 1)));
    for (std::size_t i = 1, j = 0; i < fft_size; ++i) {
        std::size_t bit = fft_size >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(fft[i], fft[j]);
    }
    for (std::size_t length = 2; length <= fft_size; length <<= 1) {
        const auto step = std::polar(1.0F, -2 * pi / static_cast<float>(length));
        for (std::size_t i = 0; i < fft_size; i += length) {
            std::complex<float> phase{1, 0};
            for (std::size_t j = 0; j < length / 2; ++j) {
                const auto a = fft[i + j], b = fft[i + j + length / 2] * phase;
                fft[i + j] = a + b;
                fft[i + j + length / 2] = a - b;
                phase *= step;
            }
        }
    }
    const float upper = std::min(18000.0F, sample_rate_ * 0.48F);
    for (std::size_t band = 0; band < bar_count; ++band) {
        const float low = 40 * std::pow(upper / 40, static_cast<float>(band) / bar_count);
        const float high = 40 * std::pow(upper / 40, static_cast<float>(band + 1) / bar_count);
        const auto first = std::clamp(static_cast<std::size_t>(low * fft_size / sample_rate_), std::size_t{1}, fft_size / 2 - 1);
        const auto last = std::clamp(static_cast<std::size_t>(std::ceil(high * fft_size / sample_rate_)), first + 1, fft_size / 2);
        float power = 0;
        for (auto bin = first; bin < last; ++bin) power = std::max(power, std::norm(fft[bin]));
        const float magnitude = std::sqrt(power) * (4.0F / fft_size);
        const float db = 20 * std::log10(std::max(magnitude, 1e-7F));
        levels_[band] = std::clamp((db + 65) / 65, 0.0F, 1.0F);
    }
}

void smooth_levels(AudioLevels& current, const AudioLevels& target, float dt) {
    dt = std::isfinite(dt) ? std::clamp(dt, 0.0F, 1.0F) : 0.0F;
    for (std::size_t i = 0; i < bar_count; ++i) {
        const float value = std::isfinite(target[i]) ? std::clamp(target[i], 0.0F, 1.0F) : 0;
        const float tau = value > current[i] ? 0.035F : 0.19F;
        current[i] += (value - current[i]) * (1 - std::exp(-dt / tau));
        if (current[i] < 0.0001F) current[i] = 0;
    }
}

} // namespace medicon::music
