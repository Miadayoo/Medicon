#pragma once

#include <imgui.h>

#include <memory>
#include <string>
#include <string_view>
#include <span>
#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11RenderTargetView;

namespace medicon::gui {

// One pane of glass for the current frame, in physical pixels.
struct LiquidGlassFrame {
    ImVec2 center{};
    ImVec2 size{};
    float scale = 1.0F;   // HUD scale; the optics are authored at 1080p
    float waist = 0.0F;   // how far the middle swells past the ends, negative pinches it
    float lens = 1.0F;    // 0..1, how strongly it bends light (it materializes by this)
    float energy = 0.0F;  // 0..1, light caught inside the glass
    float light = 0.0F;   // direction of the light on screen, radians
    float opacity = 1.0F;
    float radius = -1.0F; // negative preserves the native capsule
    std::string owner;
    std::uint64_t generation = 0;
};

// Same lifetime as an Edge-lit frame: it belongs to the current ImGui context
// and frame, so a widget that stops publishing simply stops drawing.
bool publish_liquid_glass(const LiquidGlassFrame& frame);
const LiquidGlassFrame* current_liquid_glass();
std::span<const LiquidGlassFrame> current_liquid_glass_frames();
void remove_liquid_glass(std::string_view owner, std::uint64_t generation);
// The pass has nothing left to load. Hosts use this to skip it on idle frames.
bool liquid_glass_warm();
// Whether scripts may use glass: warm, and the user's Windows settings allow it.
bool liquid_glass_ready();
void forget_liquid_glass(ImGuiContext* context);

// Refracts the finished scene through the pane. Runs before the ImGui pass, so
// text drawn at the same place sits on the glass. Preserves every D3D11 state
// it changes and never retains the back buffer.
class LiquidGlassD3D11 {
public:
    LiquidGlassD3D11();
    ~LiquidGlassD3D11();
    LiquidGlassD3D11(const LiquidGlassD3D11&) = delete;
    LiquidGlassD3D11& operator=(const LiquidGlassD3D11&) = delete;
    bool render(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* backBuffer,
                ID3D11RenderTargetView* target, const LiquidGlassFrame& frame, std::string& error);
    bool render(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* backBuffer,
                ID3D11RenderTargetView* target, std::span<const LiquidGlassFrame> frames, std::string& error);
    void reset();
    // Its shaders are compiled and have been through the driver once.
    [[nodiscard]] bool loaded() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    ImGuiContext* context_ = nullptr;
};

} // namespace medicon::gui
