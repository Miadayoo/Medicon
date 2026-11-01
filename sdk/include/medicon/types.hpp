#pragma once
#include <medicon/abi.h>
#include <cstdint>

namespace medicon { inline namespace script_v1 {
/// Small owned positions. RenderContext retains physical px; UIContext uses logical DIPs.
using Vec2 = MediconPoint;
struct Vec3 { float x = 0, y = 0, z = 0; };
struct Size { float width = 0, height = 0; };
struct Rect { float x = 0, y = 0, width = 0, height = 0; };
struct Insets { float left = 0, top = 0, right = 0, bottom = 0; };
/// Normalized RGBA. The byte factories convert [0,255] to [0,1].
struct Color {
    float r = 1, g = 1, b = 1, a = 1;
    static constexpr Color rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) { return rgba(r,g,b,255); }
    static constexpr Color rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) { return {r/255.F,g/255.F,b/255.F,a/255.F}; }
    static constexpr Color white() { return {}; }
    static constexpr Color transparent() { return {0,0,0,0}; }
};
enum class GlassQuality : std::uint32_t { Inherit, Off, Low, Balanced, High, Auto };
/** @brief Pure glass descriptor. Only regular/radius/refraction/opacity currently map to native optics.
 * Unsupported setters are preserved for source compatibility and produce a host diagnostic + plate fallback.
 * See SDK/docs/19-existing-liquid-glass-bridge.md. No graphics object is owned here. */
class Glass {
public:
    static Glass regular() { return Glass(0); }
    static Glass clear() { return Glass(1); }
    static Glass frosted() { return Glass(2); }
    static Glass prominent() { return Glass(3); }
    /// @param value Corner radius, clamped to half the pane's shorter edge. Units follow the context.
    Glass& radius(float value) { data_.fields |= MEDICON_GLASS_RADIUS; data_.radius = value; return *this; }
    /// @param value Proposed blur intensity; unsupported by the existing native renderer.
    Glass& blur(float value) { data_.fields |= MEDICON_GLASS_BLUR; data_.blur = value; return *this; }
    /// @param value Existing native lens in [0,1], controlling refraction AND absorption.
    Glass& refraction(float value) { data_.fields |= MEDICON_GLASS_REFRACTION; data_.refraction = value; return *this; }
    /// @param value Proposed tint; native optics have a fixed tint. Falls back to the host plate.
    Glass& tint(Color value) { data_.fields |= MEDICON_GLASS_TINT; data_.tint = {value.r,value.g,value.b,value.a}; return *this; }
    /// @param value Proposed saturation; unsupported, with a diagnostic.
    Glass& saturation(float value) { data_.fields |= MEDICON_GLASS_SATURATION; data_.saturation = value; return *this; }
    /// @param value Proposed highlight; unsupported, with a diagnostic.
    Glass& highlight(float value) { data_.fields |= MEDICON_GLASS_HIGHLIGHT; data_.highlight = value; return *this; }
    /// @param value Proposed border thickness; unsupported by native optics.
    Glass& border(float value) { data_.fields |= MEDICON_GLASS_BORDER; data_.border = value; return *this; }
    /// @param value Proposed shadow amount; unsupported by native optics.
    Glass& shadow(float value) { data_.fields |= MEDICON_GLASS_SHADOW; data_.shadow = value; return *this; }
    /// @param value Premultiplied composition opacity in [0,1].
    Glass& opacity(float value) { data_.fields |= MEDICON_GLASS_OPACITY; data_.opacity = value; return *this; }
    /// @param value Inherit is supported. Existing Medicon has no separate quality tiers.
    Glass& quality(GlassQuality value) { data_.fields |= MEDICON_GLASS_QUALITY; data_.quality = static_cast<uint32_t>(value); return *this; }
    /// ABI implementation accessor: returns copied values, never a device pointer.
    MediconGlassStyle descriptor() const { return data_; }
private:
    explicit Glass(uint32_t preset) { data_.size = sizeof(data_); data_.version = 1; data_.preset = preset; data_.opacity = data_.refraction = data_.saturation = 1; data_.tint = {1,1,1,1}; }
    MediconGlassStyle data_{};
};
enum class Easing { Linear, OutCubic, OutExpo, InOutCubic };
/// Stable Win32-compatible key values; no native handles or raw input API.
enum class Key : int { None = 0, Enter = 13, Escape = 27, Space = 32, Left = 37, Up = 38, Right = 39, Down = 40,
    A = 65, B = 66, C = 67, D = 68, E = 69, F = 70, G = 71, H = 72, I = 73, J = 74, K = 75, L = 76, M = 77,
    N = 78, O = 79, P = 80, Q = 81, R = 82, S = 83, T = 84, U = 85, V = 86, W = 87, X = 88, Y = 89, Z = 90,
    F1 = 112, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12 };
struct KeyPressed { Key key; bool repeat = false; };
struct GameJoined { std::uint64_t world_identity = 0; };
struct GameLeft { std::uint64_t world_identity = 0; };
struct ScreenResized { float width = 0, height = 0, scale = 1; };
}} // namespace medicon::script_v1
