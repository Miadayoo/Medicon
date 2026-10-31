#pragma once

#include "medicon/gui/LiquidGlass.hpp"

#include <imgui.h>

#include <memory>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11RenderTargetView;

namespace medicon::gui {

struct EdgeLitText {
    ImFont* font = nullptr;
    float size = 15.0F;
    ImVec2 pos{};
    std::string value;
    // Body color, including independent text opacity (ON has a delayed fade).
    ImVec4 color{1, 1, 1, 1};
};
struct EdgeLitRow {
    float left = 0, right = 0, top = 0, bottom = 0;
    float roundTopLeft = 0, roundBottomLeft = 0;
    float opacity = 1, lighting = 0;
    ImVec4 color{};
    EdgeLitText name, mode;
};
struct EdgeLitPulse {
    float centerY = 0;
    float opacity = 0;
    ImVec4 color{};
};
struct EdgeLitFrame {
    std::vector<EdgeLitRow> rows;
    std::vector<EdgeLitPulse> pulses;
    ImVec4 accentA{1, 79.0F / 255, 160.0F / 255, 1};
    ImVec4 accentB{139.0F / 255, 107.0F / 255, 1, 1};
    float screenWidth = 0, top = 8, height = 0, scale = 1;
    float glow = 1, shadow = 1;
};

// A frame belongs to the currently selected ImGui context and current frame.
// A hidden/disabled ArrayList publishes an empty frame, so stale geometry never draws.
void publish_edge_lit_frame(EdgeLitFrame frame);
const EdgeLitFrame* current_edge_lit_frame();
void forget_edge_lit_frame(ImGuiContext* context);

// Shared by the D3D11 host, Preview, and the D3D11On12 interop device.
// Effects use genuine separable Gaussian convolution and premultiplied source-over.
// render preserves every D3D11 state it changes and never retains the back buffer.
// It also draws the frame's Liquid Glass, so every host has one effects pass.
class EdgeLitD3D11 {
public:
    EdgeLitD3D11();
    ~EdgeLitD3D11();
    EdgeLitD3D11(const EdgeLitD3D11&) = delete;
    EdgeLitD3D11& operator=(const EdgeLitD3D11&) = delete;
    bool render(ID3D11Device* device, ID3D11DeviceContext* context,
                ID3D11Texture2D* backBuffer, ID3D11RenderTargetView* target,
                std::string& error);
    void reset();
    void invalidate_fonts();
    // False until the first render, which loads Liquid Glass even with nothing to show.
    [[nodiscard]] bool loaded() const noexcept { return glass_.loaded(); }
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    LiquidGlassD3D11 glass_;
};

} // namespace medicon::gui
