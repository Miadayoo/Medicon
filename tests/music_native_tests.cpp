// Native integration probe: in-memory PNG -> WinRT streams/decoder, desktop
// SMTC -> production session watcher, and actual WASAPI output -> loopback FFT.
// A short -46 dB test tone is emitted only with --loopback. No user media is
// started/stopped, and every fixture resource belongs to this process.
#include <medicon/music/MusicSessionManager.hpp>
#include <medicon/music/AudioVisualizer.hpp>

#include <Windows.h>
#include <systemmediatransportcontrolsinterop.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include "../src/music/AlbumArtProvider.hpp"
#include "../src/music/SessionState.hpp"
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <numbers>
#include <thread>

namespace {
using namespace medicon::music;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Media;
using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;

void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void wait_for(const std::function<bool()>& predicate, const char* message) {
    const auto deadline = Clock::now() + std::chrono::seconds(8);
    while (!predicate()) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (Clock::now() > deadline) throw std::runtime_error(message);
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
}

// Exercise the real WinRT projection: S_OK with a null playback-info object is
// different from a failed HRESULT. A C++ catch does not catch its null vtable.
struct EmptyPlaybackSession : winrt::implements<EmptyPlaybackSession,
    winrt::Windows::Media::Control::IGlobalSystemMediaTransportControlsSession> {
    using Info = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackInfo;
    using Properties = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionMediaProperties;
    using Timeline = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionTimelineProperties;
    template<class T> using Async = winrt::Windows::Foundation::IAsyncOperation<T>;
    bool disconnected = false;
    Info GetPlaybackInfo() {
        if (disconnected) throw winrt::hresult_error(RPC_E_DISCONNECTED);
        return nullptr;
    }
    winrt::hstring SourceAppUserModelId() { return L"Medicon.EmptyPlaybackFixture"; }
    Async<Properties> TryGetMediaPropertiesAsync() { return nullptr; }
    Timeline GetTimelineProperties() { return nullptr; }
    Async<bool> TryPlayAsync() { return nullptr; }
    Async<bool> TryPauseAsync() { return nullptr; }
    Async<bool> TryStopAsync() { return nullptr; }
    Async<bool> TryRecordAsync() { return nullptr; }
    Async<bool> TryFastForwardAsync() { return nullptr; }
    Async<bool> TryRewindAsync() { return nullptr; }
    Async<bool> TrySkipNextAsync() { return nullptr; }
    Async<bool> TrySkipPreviousAsync() { return nullptr; }
    Async<bool> TryChangeChannelUpAsync() { return nullptr; }
    Async<bool> TryChangeChannelDownAsync() { return nullptr; }
    Async<bool> TryTogglePlayPauseAsync() { return nullptr; }
    Async<bool> TryChangeAutoRepeatModeAsync(MediaPlaybackAutoRepeatMode) { return nullptr; }
    Async<bool> TryChangePlaybackRateAsync(double) { return nullptr; }
    Async<bool> TryChangeShuffleActiveAsync(bool) { return nullptr; }
    Async<bool> TryChangePlaybackPositionAsync(std::int64_t) { return nullptr; }
    template<class Handler> winrt::event_token TimelinePropertiesChanged(const Handler&) { return {}; }
    void TimelinePropertiesChanged(const winrt::event_token&) noexcept {}
    template<class Handler> winrt::event_token PlaybackInfoChanged(const Handler&) { return {}; }
    void PlaybackInfoChanged(const winrt::event_token&) noexcept {}
    template<class Handler> winrt::event_token MediaPropertiesChanged(const Handler&) { return {}; }
    void MediaPropertiesChanged(const winrt::event_token&) noexcept {}
};

void session_responses() {
    using namespace winrt::Windows::Media::Control;
    using Status = GlobalSystemMediaTransportControlsSessionPlaybackStatus;
    auto fixture = winrt::make_self<EmptyPlaybackSession>();
    const auto session = fixture.as<GlobalSystemMediaTransportControlsSession>();
    check(detail::playback_status(session) == Status::Closed, "null playback info was not treated as closed");
    fixture->disconnected = true;
    check(detail::playback_status(session) == Status::Closed, "disconnected session was not treated as closed");
    check(detail::playback_status(nullptr) == Status::Closed, "missing session was not treated as closed");
    bool rejected = false;
    try { await_result(fixture->TryGetMediaPropertiesAsync(), {}); }
    catch (const winrt::hresult_error&) { rejected = true; }
    check(rejected, "missing async operation was not rejected safely");
    std::cout << "native session responses: null playback info / disconnected session / missing session / missing operation passed\n";
}

RandomAccessStreamReference make_art() {
    InMemoryRandomAccessStream stream;
    const auto encoder = await_result(BitmapEncoder::CreateAsync(BitmapEncoder::PngEncoderId(), stream), {});
    const std::array<std::uint8_t,8> rgba{250,20,30,255,40,220,60,255};
    encoder.SetPixelData(BitmapPixelFormat::Rgba8, BitmapAlphaMode::Straight, 2, 1, 96, 96, rgba);
    await_result(encoder.FlushAsync(), {});
    stream.Seek(0);
    return RandomAccessStreamReference::CreateFromStream(stream);
}

struct MissingThumbnailStream : winrt::implements<MissingThumbnailStream, IRandomAccessStreamReference> {
    winrt::Windows::Foundation::IAsyncOperation<IRandomAccessStreamWithContentType> OpenReadAsync() {
        co_return nullptr;
    }
};

void album_art() {
    AlbumArtProvider provider;
    auto image = provider.load(make_art(), {});
    check(image && image->width == 2 && image->height == 1 && image->rgba[0] == 250 && image->rgba[5] == 220,
        "WinRT PNG decode did not preserve RGBA pixels");
    check(provider.load(make_art(), {}) == image, "identical art did not reuse cache");
    InMemoryRandomAccessStream invalid;
    check(!provider.load(RandomAccessStreamReference::CreateFromStream(invalid), {}), "invalid art did not fall back");
    check(!provider.load(winrt::make<MissingThumbnailStream>(), {}), "missing thumbnail stream did not fall back");
    std::cout << "native album art: decoded RGBA / cache / malformed-stream / missing-stream fallback passed\n";
}

void media_session() {
    struct Window {
        HWND handle = CreateWindowExW(0, L"STATIC", L"Medicon music integration fixture", WS_OVERLAPPEDWINDOW,
            0,0,200,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        ~Window() { if (handle) DestroyWindow(handle); }
    } window;
    check(window.handle != nullptr, "SMTC fixture window creation failed");
    const auto factory = winrt::get_activation_factory<SystemMediaTransportControls, ISystemMediaTransportControlsInterop>();
    SystemMediaTransportControls controls{nullptr};
    winrt::check_hresult(factory->GetForWindow(window.handle, winrt::guid_of<SystemMediaTransportControls>(), winrt::put_abi(controls)));
    struct Disable { SystemMediaTransportControls controls; ~Disable() { try { controls.IsEnabled(false); } catch (...) {} } } disable{controls};
    controls.IsPlayEnabled(true);
    controls.IsPauseEnabled(true);
    controls.IsEnabled(true);
    const auto updater = controls.DisplayUpdater();
    updater.Type(MediaPlaybackType::Music);
    updater.MusicProperties().Title(L"Medicon native test — 曲名");
    updater.MusicProperties().Artist(L"Test artist");
    updater.Thumbnail(make_art());
    updater.Update();
    controls.PlaybackStatus(MediaPlaybackStatus::Playing);
    // Register a fixture that has played before Medicon starts, then leave it
    // paused. A stale session must not cause a startup widget to appear.
    const auto discovery = await_result(winrt::Windows::Media::Control::
        GlobalSystemMediaTransportControlsSessionManager::RequestAsync(), {});
    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession fixture{nullptr};
    wait_for([&] {
        for (const auto& session : discovery.GetSessions()) {
            if (await_result(session.TryGetMediaPropertiesAsync(), {}).Title() == L"Medicon native test — 曲名") {
                fixture = session;
                return true;
            }
        }
        return false;
    }, "fixture was not registered with SMTC");
    controls.PlaybackStatus(MediaPlaybackStatus::Paused);
    wait_for([&] { return fixture.GetPlaybackInfo().PlaybackStatus() ==
        winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused; },
        "fixture did not enter paused state before watcher startup");
    MusicSessionManager manager;
    const auto observe_until = Clock::now() + std::chrono::milliseconds(1200);
    wait_for([&] {
        const auto s = manager.snapshot();
        check(!s->available || s->title != "Medicon native test — 曲名", "startup displayed a stale paused session");
        return Clock::now() >= observe_until;
    }, "startup visibility check timed out");
    controls.PlaybackStatus(MediaPlaybackStatus::Playing);
    wait_for([&] { const auto s = manager.snapshot(); return s->playing && s->title == "Medicon native test — 曲名" && s->art; },
        "production manager did not observe fixture playing/title/art");
    check(manager.snapshot()->artist == "Test artist" && !manager.snapshot()->source_app.empty(), "missing artist/app identity");
    controls.PlaybackStatus(MediaPlaybackStatus::Paused);
    try {
        wait_for([&] { const auto s = manager.snapshot();
            return !s->available && !s->playing && s->title == "Medicon native test — 曲名";
        }, "pause did not hide the fixture");
    } catch (...) {
        const auto s = manager.snapshot();
        std::cerr << "pause snapshot: fixture=" << (s->title == "Medicon native test — 曲名")
                  << " available=" << s->available << " playing=" << s->playing << '\n';
        throw;
    }
    updater.MusicProperties().Title(L"Medicon second track");
    updater.Update();
    wait_for([&] { return manager.snapshot()->title == "Medicon second track"; }, "track-change event not observed");
    controls.PlaybackStatus(MediaPlaybackStatus::Playing);
    wait_for([&] { const auto s = manager.snapshot();
        return s->available && s->playing && s->title == "Medicon second track" && s->art;
    }, "resume did not show the fixture with cached art");
    for (int cycle = 0; cycle < 8; ++cycle) {
        controls.PlaybackStatus(MediaPlaybackStatus::Paused);
        wait_for([&] { const auto s = manager.snapshot(); return !s->available && !s->playing; },
            "repeated pause did not hide the fixture");
        controls.PlaybackStatus(MediaPlaybackStatus::Playing);
        wait_for([&] { const auto s = manager.snapshot();
            return s->available && s->playing && s->title == "Medicon second track" && s->art;
        }, "repeated resume after pause did not recover");
        controls.PlaybackStatus(MediaPlaybackStatus::Stopped);
        wait_for([&] { const auto s = manager.snapshot(); return !s->available && !s->playing; },
            "repeated stop did not hide the fixture");
        controls.PlaybackStatus(MediaPlaybackStatus::Playing);
        wait_for([&] { const auto s = manager.snapshot();
            return s->available && s->playing && s->title == "Medicon second track" && s->art;
        }, "repeated resume after stop did not recover");
    }
    controls.PlaybackStatus(MediaPlaybackStatus::Stopped);
    controls.IsEnabled(false);
    wait_for([&] { const auto s = manager.snapshot(); return !s->available || s->title != "Medicon second track"; },
        "session end not observed");
    std::cout << "native SMTC: hidden while paused / show on play / Japanese title / artist / app / art / track change / 8 pause-stop-resume cycles / end passed\n";
}

void loopback() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    winrt::check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)));
    ComPtr<IMMDevice> device;
    winrt::check_hresult(enumerator->GetDefaultAudioEndpoint(eRender,eMultimedia,&device));
    ComPtr<IAudioClient> client;
    winrt::check_hresult(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(client.GetAddressOf())));
    WAVEFORMATEX* raw = nullptr;
    winrt::check_hresult(client->GetMixFormat(&raw));
    std::unique_ptr<WAVEFORMATEX,decltype(&CoTaskMemFree)> format(raw,CoTaskMemFree);
    const auto& mix = *raw;
    constexpr GUID float_format{WAVE_FORMAT_IEEE_FLOAT,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
    bool floating = mix.wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (mix.wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix.cbSize >= sizeof(WAVEFORMATEXTENSIBLE)-sizeof(WAVEFORMATEX))
        floating = IsEqualGUID(reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(&mix)->SubFormat,float_format);
    check(floating && mix.wBitsPerSample == 32 && mix.nBlockAlign == mix.nChannels*4,
        "loopback test tone requires packed float32 mix format");
    winrt::check_hresult(client->Initialize(AUDCLNT_SHAREMODE_SHARED,0,1000000,0,raw,nullptr));
    ComPtr<IAudioRenderClient> render;
    winrt::check_hresult(client->GetService(IID_PPV_ARGS(&render)));
    UINT32 capacity = 0;
    winrt::check_hresult(client->GetBufferSize(&capacity));
    AudioVisualizer visualizer;
    visualizer.set_enabled(true);
    winrt::check_hresult(client->Start());
    struct Stop { IAudioClient* client; ~Stop() { client->Stop(); } } stop{client.Get()};
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    float phase = 0, maximum = 0;
    while (Clock::now() < deadline) {
        UINT32 padding = 0;
        winrt::check_hresult(client->GetCurrentPadding(&padding));
        const UINT32 frames = capacity-padding;
        if (frames) {
            BYTE* output = nullptr;
            winrt::check_hresult(render->GetBuffer(frames,&output));
            for (UINT32 i = 0; i < frames; ++i) {
                const float sample = 0.005F*std::sin(phase);
                phase = std::fmod(phase + 2*std::numbers::pi_v<float>*1000/static_cast<float>(mix.nSamplesPerSec),
                    2*std::numbers::pi_v<float>);
                for (unsigned channel = 0; channel < mix.nChannels; ++channel)
                    std::memcpy(output + i*mix.nBlockAlign + channel*4,&sample,4);
            }
            winrt::check_hresult(render->ReleaseBuffer(frames,0));
        }
        const auto levels = visualizer.levels();
        maximum = std::max(maximum,*std::max_element(levels.begin(),levels.end()));
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    check(maximum > 0.03F, "WASAPI loopback FFT did not react to actual output");
    client->Stop();
    visualizer.set_enabled(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto silent = visualizer.levels();
    check(*std::max_element(silent.begin(),silent.end()) == 0, "capture disable retained stale spectrum");
    std::cout << "native WASAPI: real 1 kHz output -> loopback FFT / shutdown silence passed (peak " << maximum << ")\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        session_responses();
        album_art();
        if (argc > 1 && std::string_view(argv[1]) == "--session") media_session();
        if (argc > 1 && std::string_view(argv[1]) == "--loopback") loopback();
        winrt::uninit_apartment();
        return 0;
    } catch (const winrt::hresult_error& error) {
        std::cerr << "native music FAILED: 0x" << std::hex << static_cast<unsigned>(error.code()) << ' '
            << winrt::to_string(error.message()) << '\n'; return 1;
    } catch (const std::exception& error) {
        std::cerr << "native music FAILED: " << error.what() << '\n'; return 1;
    }
}
