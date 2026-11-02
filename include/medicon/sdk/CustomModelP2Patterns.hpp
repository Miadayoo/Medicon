#pragma once
#include <string_view>
namespace medicon::sdk::custom_model::p2_native {
// Bounded IDA + unique exact profile scan. See docs/custom_model_p2_report.md.
inline constexpr std::string_view texture_get =
    "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 58 01 00 00 48 8D AC 24 80 00 00 00 48 C7 85 D0 00 00 00 FE FF FF FF 44 88 8D CF 00 00 00 4D 89 C7 48 89 D6 48 89 CF 48 89 8D 98 00 00 00 4C 8B B5";
inline constexpr std::string_view tess_end =
    "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 88 03 00 00 48 8D AC 24 80 00 00 00 44 0F 29 AD F0 02 00 00 66 44 0F 7F A5 E0 02 00 00 44 0F 29 9D D0 02 00 00 44 0F 29 95 C0 02 00 00 44 0F 29 8D";
inline constexpr std::string_view mesh_draw =
    "55 56 57 53 48 81 EC E8 01 00 00 48 8D AC 24 80 00 00 00 48 C7 85 60 01 00 00 FE FF FF FF 4C 89 C6 48 89 D7 48 89 CB C6 85 48 01 00 00 00 48 C7 85 08 01 00 00 00 00 00 00 C6 45 E8 FF 48 8D 45";
inline constexpr std::string_view mesh_destroy =
    "56 57 48 83 EC 28 48 89 CE E8 ?? ?? ?? ?? 48 8B 8E 48 02 00 00 48 85 C9 74 ?? 48 8B 96 58 02 00 00 48 29 CA 48 81 FA 00 10 00 00 72 ?? 48 8B 41 F8 48 83 C1 F8 48 29 C1 48 83 F9 20 0F 83 F9 00";
inline constexpr std::string_view builder_clear =
    "C6 01 00 48 8B 41 08 48 3B 41 10 74 ?? 48 89 41 10 48 8B 41 20 48 3B 41 28 74 ?? 48 89 41 28 48 8B 41 38 48 3B 41 40 74 ?? 48 89 41 40 48 8B 41 68 48 3B 41 70 74 ?? 48 89 41 70 48 8B 81 80 00";
inline constexpr std::string_view view_id =
    "55 56 57 48 83 EC 70 48 8D 6C 24 70 48 C7 45 F8 FE FF FF FF 48 8D 35 ?? ?? ?? ?? 48 89 F1 E8 ?? ?? ?? ?? F0 48 FF 05 ?? ?? ?? ?? 48 89 F1 E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 85 C0 74 ?? 80";
}
