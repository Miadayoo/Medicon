#include <medicon/music/AudioVisualizer.hpp>

#include <Windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace medicon::music {
namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

void require(HRESULT result) { if (FAILED(result)) throw result; }

struct MixFormat {
    WAVEFORMATEX* value = nullptr;
    ~MixFormat() { CoTaskMemFree(value); }
};

std::wstring default_id(IMMDeviceEnumerator* enumerator, ComPtr<IMMDevice>& device) {
    require(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, device.ReleaseAndGetAddressOf()));
    LPWSTR raw = nullptr;
    require(device->GetId(&raw));
    const std::wstring id(raw);
    CoTaskMemFree(raw);
    return id;
}

float sample_value(const BYTE* data, unsigned bits, bool floating) {
    if (floating) {
        if (bits == 32) { float value; std::memcpy(&value, data, 4); return value; }
        double value; std::memcpy(&value, data, 8); return static_cast<float>(value);
    }
    if (bits == 8) return (static_cast<float>(*data) - 128) / 128;
    if (bits == 16) { std::int16_t value; std::memcpy(&value, data, 2); return static_cast<float>(value) / 32768; }
    if (bits == 24) {
        const std::uint32_t packed = static_cast<std::uint32_t>(data[0]) |
            (static_cast<std::uint32_t>(data[1]) << 8) | (static_cast<std::uint32_t>(data[2]) << 16);
        const std::int32_t value = (packed & 0x800000U) ? static_cast<std::int32_t>(packed) - 0x1000000 :
            static_cast<std::int32_t>(packed);
        return static_cast<float>(value) / 8388608;
    }
    std::int32_t value; std::memcpy(&value, data, 4);
    return static_cast<float>(value) / 2147483648.0F;
}
} // namespace

struct AudioVisualizer::Impl {
    std::atomic<bool> enabled{false};
    mutable std::mutex mutex;
    std::condition_variable_any wake;
    AudioLevels latest{};
    Clock::time_point packet_time{};

    void publish(const AudioLevels& levels) {
        std::lock_guard lock(mutex);
        latest = levels;
        packet_time = Clock::now();
    }

    void capture(std::stop_token stop) {
        ComPtr<IMMDeviceEnumerator> enumerator;
        require(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)));
        ComPtr<IMMDevice> device;
        const auto id = default_id(enumerator.Get(), device);
        ComPtr<IAudioClient> client;
        require(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(client.GetAddressOf())));
        MixFormat format;
        require(client->GetMixFormat(&format.value));
        const auto& mix = *format.value;
        bool floating = mix.wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        bool pcm = mix.wFormatTag == WAVE_FORMAT_PCM;
        if (mix.wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            const auto& ext = *reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(&mix);
            constexpr GUID float_format{WAVE_FORMAT_IEEE_FLOAT, 0, 0x10, {0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
            constexpr GUID pcm_format{WAVE_FORMAT_PCM, 0, 0x10, {0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
            floating = IsEqualGUID(ext.SubFormat, float_format);
            pcm = IsEqualGUID(ext.SubFormat, pcm_format);
        }
        if ((!floating && !pcm) || !mix.nChannels || !mix.nSamplesPerSec ||
            (floating && mix.wBitsPerSample != 32 && mix.wBitsPerSample != 64) ||
            (pcm && mix.wBitsPerSample != 8 && mix.wBitsPerSample != 16 && mix.wBitsPerSample != 24 && mix.wBitsPerSample != 32) ||
            mix.nBlockAlign < mix.nChannels * (mix.wBitsPerSample / 8)) throw E_NOTIMPL;
        require(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
            1000000, 0, format.value, nullptr));
        ComPtr<IAudioCaptureClient> capture;
        require(client->GetService(IID_PPV_ARGS(&capture)));
        AudioSpectrum spectrum;
        spectrum.reset(static_cast<float>(mix.nSamplesPerSec));
        require(client->Start());
        struct StopClient { IAudioClient* client; ~StopClient() { client->Stop(); } } stop_client{client.Get()};
        auto checked = Clock::now();
        std::array<float, AudioSpectrum::fft_size> mono{};
        while (!stop.stop_requested() && enabled.load()) {
            UINT32 packets = 0;
            require(capture->GetNextPacketSize(&packets));
            while (packets && !stop.stop_requested()) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                require(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr));
                if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) spectrum.reset(static_cast<float>(mix.nSamplesPerSec));
                for (UINT32 offset = 0; offset < frames;) {
                    const auto count = std::min<std::size_t>(mono.size(), frames - offset);
                    for (std::size_t i = 0; i < count; ++i) {
                        float value = 0;
                        if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data) {
                            const auto* frame = data + (offset + i) * mix.nBlockAlign;
                            for (unsigned channel = 0; channel < mix.nChannels; ++channel)
                                value += sample_value(frame + channel * (mix.wBitsPerSample / 8), mix.wBitsPerSample, floating);
                        }
                        mono[i] = value / static_cast<float>(mix.nChannels);
                    }
                    spectrum.push(std::span(mono.data(), count));
                    offset += static_cast<UINT32>(count);
                }
                require(capture->ReleaseBuffer(frames));
                publish(spectrum.levels());
                require(capture->GetNextPacketSize(&packets));
            }
            // Default-output switches can leave the old device valid. Reopen
            // on identity changes as well as AUDCLNT_E_DEVICE_INVALIDATED.
            if (Clock::now() - checked > std::chrono::seconds(1)) {
                ComPtr<IMMDevice> current;
                if (default_id(enumerator.Get(), current) != id) return;
                checked = Clock::now();
            }
            std::unique_lock lock(mutex);
            wake.wait_for(lock, stop, std::chrono::milliseconds(8), [&] { return !enabled.load(); });
        }
    }

    std::jthread worker{[this](std::stop_token stop) {
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(apartment)) return;
        while (!stop.stop_requested()) {
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, stop, [&] { return enabled.load(); });
            }
            if (stop.stop_requested()) break;
            try { capture(stop); } catch (...) {}
            publish({});
            std::unique_lock lock(mutex);
            wake.wait_for(lock, stop, std::chrono::milliseconds(500), [&] { return !enabled.load(); });
        }
        CoUninitialize();
    }};
};

AudioVisualizer::AudioVisualizer() : impl_(std::make_unique<Impl>()) {}
AudioVisualizer::~AudioVisualizer() = default;
void AudioVisualizer::set_enabled(bool enabled) {
    if (impl_->enabled.exchange(enabled) != enabled) impl_->wake.notify_all();
}
AudioLevels AudioVisualizer::levels() const {
    std::lock_guard lock(impl_->mutex);
    // Some drivers send no silent packets at all. Never freeze the last FFT.
    return Clock::now() - impl_->packet_time > std::chrono::milliseconds(120) ? AudioLevels{} : impl_->latest;
}

} // namespace medicon::music
