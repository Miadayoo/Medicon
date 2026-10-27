#pragma once

#include <medicon/gui/Textures.hpp>

#include <Windows.h>
#include <dxgi.h>
#include <d3d12.h>
#include <imgui.h>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace medicon::platform {

// Diagnostic timing (QueryPerformanceCounter ticks).
inline std::uint64_t perf_now() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}
inline double perf_ms(std::uint64_t ticks) noexcept {
    static const double frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    return static_cast<double>(ticks) * 1000.0 / frequency;
}

struct RenderCounters {
    std::uint64_t effects_ticks = 0, effects_max = 0;
    std::uint32_t effects_frames = 0;
    std::uint32_t skipped_frames = 0; // D3D12: previous overlay work still in flight
};

// The caller owns the ImGui context and serializes rendering/resizing.
class OverlayRenderer {
public:
    virtual ~OverlayRenderer() = default;
    virtual bool initialize(IDXGISwapChain* swap, ID3D12CommandQueue* queue, std::string& error) = 0;
    virtual bool rebuild_fonts(std::string& error) = 0;
    virtual bool render(ImDrawData* data, std::string& error) = 0;
    virtual bool wait_idle(std::string& error) = 0;
    // Extra GUI textures (Social). Valid after initialize() until destruction.
    virtual gui::TextureHost* textures() noexcept = 0;
    RenderCounters take_counters() noexcept { return std::exchange(counters_, {}); }

protected:
    void count_effects(std::uint64_t ticks) noexcept {
        counters_.effects_ticks += ticks;
        if (ticks > counters_.effects_max) counters_.effects_max = ticks;
        ++counters_.effects_frames;
    }
    RenderCounters counters_;
};

std::unique_ptr<OverlayRenderer> make_d3d11_renderer();
std::unique_ptr<OverlayRenderer> make_d3d12_renderer();

} // namespace medicon::platform
