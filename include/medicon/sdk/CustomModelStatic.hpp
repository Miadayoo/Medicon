#pragma once
#include <medicon/memory/module_view.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
namespace medicon::sdk::custom_model {
// Physical slots from bounded native bodies, not DLL STL/class ABI.
using TextureGetFn = void* (*)(void*,void*,const void*,bool,std::uint64_t,std::uint8_t);
using TessEndFn = void (*)(void*,void*,int,const void*,std::uint16_t);
using BuiltMeshDrawFn = void (*)(void*,void*,const void*,const void*,std::uintptr_t,std::uintptr_t,const void*);
using MeshDestroyFn = void (*)(void*);
using BuilderClearFn = void (*)(void*);
using ViewIdFn = std::uint16_t (*)();
struct StaticTargets {
    TextureGetFn texture_get{}; TessEndFn end{}; BuiltMeshDrawFn draw{};
    MeshDestroyFn destroy{}; BuilderClearFn clear{}; ViewIdFn view{};
    explicit operator bool() const noexcept { return texture_get&&end&&draw&&destroy&&clear&&view; }
};
[[nodiscard]] StaticTargets locate_static_targets(std::string& diagnostic);
// Read-only ResourceLocation input envelope. The backing UTF-8 string must
// outlive the call; native getTexture copies it with the game's allocator.
// Current 56-byte layout/hash formula: 1402709A0 / 141D0CCE0 / 144B0F000.
// Raw filesystem kind 2: .reference/solstice/src/SDK/Minecraft/mce.hpp.
[[nodiscard]] std::array<std::byte,56> raw_texture_location(std::string_view path) noexcept;
struct StaticProgress { unsigned meshes{},textures{}; bool ready{}; std::uint64_t requests{},ticks{}; };
}
