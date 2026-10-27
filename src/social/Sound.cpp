#include <medicon/social/Service.hpp>

#include <Windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace medicon::social {
namespace {

// Two soft rising notes, ~0.2 s, synthesized once into a RIFF/WAVE image.
// No asset file and no file I/O while the game runs.
std::vector<std::uint8_t> make_chime() {
    constexpr int rate = 44100;
    constexpr double tau = 6.28318530717958647692;
    struct Note { double frequency, start, length; };
    constexpr Note notes[]{{1046.5, 0.0, 0.16}, {1568.0, 0.07, 0.15}};
    const int samples = static_cast<int>(rate * 0.23);
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(samples));
    for (int i = 0; i < samples; ++i) {
        const double t = static_cast<double>(i) / rate;
        double value = 0.0;
        for (const auto& note : notes) {
            const double local = t - note.start;
            if (local < 0.0 || local > note.length) continue;
            // 5 ms attack, exponential decay, and a short fade so the note ends at zero.
            const double attack = std::min(1.0, local / 0.005);
            const double fade = std::min(1.0, (note.length - local) / 0.02);
            value += std::sin(tau * note.frequency * local) * attack * fade * std::exp(-local * 22.0);
        }
        pcm[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(std::lround(std::clamp(value * 0.22, -1.0, 1.0) * 32767.0));
    }
    const auto data_bytes = static_cast<std::uint32_t>(pcm.size() * sizeof(std::int16_t));
    std::vector<std::uint8_t> wave(44 + data_bytes);
    const auto put32 = [&](std::size_t at, std::uint32_t value) { std::memcpy(wave.data() + at, &value, 4); };
    const auto put16 = [&](std::size_t at, std::uint16_t value) { std::memcpy(wave.data() + at, &value, 2); };
    std::memcpy(wave.data(), "RIFF", 4);
    put32(4, 36 + data_bytes);
    std::memcpy(wave.data() + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1); // PCM
    put16(22, 1); // mono
    put32(24, rate);
    put32(28, rate * 2);
    put16(32, 2);
    put16(34, 16);
    std::memcpy(wave.data() + 36, "data", 4);
    put32(40, data_bytes);
    std::memcpy(wave.data() + 44, pcm.data(), data_bytes);
    return wave;
}

} // namespace

void play_notification_sound() noexcept {
    try {
        // SND_MEMORY with SND_ASYNC needs the image to outlive playback.
        static const std::vector<std::uint8_t> chime = make_chime();
        PlaySoundW(reinterpret_cast<LPCWSTR>(chime.data()), nullptr, SND_ASYNC | SND_MEMORY | SND_NODEFAULT);
    } catch (...) {
        // A sound is never worth an exception on the render thread.
    }
}

} // namespace medicon::social
