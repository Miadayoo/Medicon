# Third-party sources

| Component | Pinned revision | License |
| --- | --- | --- |
| [Dear ImGui](https://github.com/ocornut/imgui) 1.91.9b | `f5befd2d29e66809cd1110a152e375a7f1981f06` | MIT; see fetched `imgui-src/LICENSE.txt` |
| [nlohmann/json](https://github.com/nlohmann/json) 3.11.3 | `9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03` | MIT; see fetched `json-src/LICENSE.MIT` |
| [MinHook](https://github.com/TsudaKageyu/minhook) 1.3.4 | `c3fcafdc10146beb5919319d0683e44e3c30d537` | BSD 2-Clause and bundled HDE notices; see fetched `minhook-src/LICENSE.txt` |
| [Microsoft C++/WinRT](https://github.com/microsoft/cppwinrt) | NuGet `2.0.240405.15` (SDK fallback generator) | MIT; see bundled `third_party/cppwinrt/LICENSE` |
| [Geist SemiBold 600 and Geist Mono Medium 500](https://github.com/vercel/geist-font) | `be21666703155b45d8bfb1e7fe5652fe48c842cb` | SIL Open Font License 1.1; see bundled `assets/fonts/OFL.txt` |
| [Solstice signature data](https://github.com/surukotonai/solstice/blob/1d7808f52ef4ba6e34a7fbcc65294907a7761611/src/SDK/SigManager.hpp) | `1d7808f52ef4ba6e34a7fbcc65294907a7761611` | No root license found at this revision; no license grant is asserted |

CMake fetches dependencies into the build's `_deps` directory. Preserve the corresponding license files when redistributing dependency code or binaries. The Solstice import contains signature definitions and source metadata; its scanner, SDK, authorization, and networking implementation are not imported, and AntiBot is the only module ported from it. See `data/signatures/solstice.json` and `docs/signatures.md` for provenance.

The user-supplied, unmodified DynaPuff Medium font is embedded in the client and
Preview. Its SHA-256 checksum is recorded in `assets/fonts/README.md`.
Copyright 2021 The DynaPuff Project Authors (https://github.com/googlefonts/dynapuff).
Its embedded metadata specifies SIL Open Font License 1.1. CMake copies
`assets/fonts/DynaPuff-OFL.txt` to `bin/licenses/DynaPuff-OFL.txt` for redistribution.
The previous Geist source assets retain their original OFL notice.

The Liquid Glass pass (`src/gui/LiquidGlass.cpp`) ports the optics of [YMM4-LiquidGlass](https://github.com/namakemono-san/YMM4-LiquidGlass) from C#/ComputeSharp to HLSL: the signed distance field with smoothed corners, the bezel height and displacement, the channel offsets, Beer-Lambert absorption and the Fresnel highlight. MIT, Copyright (c) 2026 namakemono-san; see bundled `third_party/ymm4-liquidglass/LICENSE`, and preserve it when redistributing. The spring constants of the block counter follow [AndroidLiquidGlass](https://github.com/Kyant0/AndroidLiquidGlass) (Apache-2.0); no source is imported from it.

The local ignored `.tools` directory contains development tools, not runtime client dependencies: LLVM MinGW (upstream LLVM/MinGW licenses), CMake, and Ninja. Their bundled notices remain in their distributions. None are downloaded or executed at client startup.

The bind-command syntax and minimal Bedrock chat bridge use Solstice's
`Features/Command/Commands/BindCommand.cpp`, `PacketSendHook.cpp`,
`MinecraftGame.cpp`, `OffsetProvider.hpp`, `TextPacket.hpp`, `GuiData.cpp`,
`BaseTickHook.cpp`, and `MemUtils.hpp` at the same pinned revision as
behavioral/layout/ABI references. These cover Minecraft's own chat input,
pre-send cancellation of local dot commands, and native chat feedback from the
local player's BaseTick. The command parser, foreign-memory reader and bridge
are independently implemented here; the upstream SDK and networking source are
not imported. Native feedback passes a borrowed POD string envelope rather than
importing game STL types or transferring the DLL's allocations to the game.
See `docs/commands.md` for the exact boundary and verification limits.

The gameplay bridge additionally uses the same pinned Solstice revision's
`OffsetProvider.hpp`, actor ECS component layouts, client camera layout,
`GameMode.hpp`, inventory/block layouts, and Scaffold placement behavior as
layout/ABI references. ChinaHat, Derp, Aimbot, Killaura, Scaffold, ESP, the
snapshot interface and ImGui glow renderer are independently implemented here.
No upstream module, EnTT implementation or game-owned STL type is linked into
the client, except that AntiBot is a direct port of Solstice's
`Features/Modules/Misc/AntiBot.cpp` checks, settings and hitbox limits. See
`docs/gameplay-modules.md` for behavior and validation scope.
