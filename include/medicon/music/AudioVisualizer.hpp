#pragma once

#include "AudioSpectrum.hpp"
#include <memory>

namespace medicon::music {

// Default-render-device WASAPI loopback. Only spectral levels leave the worker;
// audio is neither retained nor written to disk. Idle capture is suspended.
class AudioVisualizer {
public:
    AudioVisualizer();
    ~AudioVisualizer();
    AudioVisualizer(const AudioVisualizer&) = delete;
    AudioVisualizer& operator=(const AudioVisualizer&) = delete;
    void set_enabled(bool enabled);
    AudioLevels levels() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace medicon::music
