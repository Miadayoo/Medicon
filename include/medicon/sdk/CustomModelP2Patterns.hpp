#pragma once
#include <string_view>
namespace medicon::sdk::custom_model::p2_native {
// Bounded IDA + unique exact profile scan. See docs/custom_model_p2_report.md.
inline constexpr std::string_view texture_get = "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 58 01 00 00 48 8D AC 24 80 00 00 00 48 C7 85 D0 00 00 00 FE FF FF FF 44 88 8D CF 00 00 00 4D 89 C7 48 89 D6 48 89 CF 4C 8B B5 40 01 00 00 48 81 C1";
inline constexpr std::string_view tess_end = "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 98 03 00 00 48 8D AC 24 80 00 00 00 44 0F 29 A5 00 03 00 00 44 0F 29 9D F0 02 00 00 44 0F 29 95 E0 02 00 00 44 0F 29 8D D0 02 00 00 44 0F 29 85 C0";
inline constexpr std::string_view mesh_draw = "55 56 57 53 48 81 EC F8 01 00 00 48 8D AC 24 80 00 00 00 48 C7 85 70 01 00 00 FE FF FF FF 4C 89 C6 48 89 D7 48 89 CB C6 85 58 01 00 00 00 48 C7 85 10 01 00 00 00 00 00 00 C6 45 F0 FF 48 8D 45";
inline constexpr std::string_view mesh_destroy = "55 56 57 53 48 83 EC 38 48 8D 6C 24 30 48 C7 45 00 FE FF FF FF 48 89 CE E8 63 F1 FF FF 48 8B 8E 60 02 00 00 48 85 C9 74 49 48 8B 96 70 02 00 00 48 29 CA 48 81 FA 00 10 00 00 72 1C 48 8B 41 F8";
inline constexpr std::string_view builder_clear = "C6 01 00 48 8B 41 08 48 3B 41 10 74 04 48 89 41 10 48 8B 41 20 48 3B 41 28 74 04 48 89 41 28 48 8B 41 38 48 3B 41 40 74 04 48 89 41 40 48 8B 41 68 48 3B 41 70 74 04 48 89 41 70 48 8B 81 80 00";
inline constexpr std::string_view view_id = "55 56 57 48 83 EC 60 48 8D 6C 24 60 48 C7 45 F8 FE FF FF FF 48 8D 35 75 01 63 0D 48 89 F1 E8 05 06 94 09 F0 48 FF 05 C5 01 63 0D 48 89 F1 E8 19 06 94 09 48 8B 05 BE 01 63 0D 48 85 C0 74 25 80";
}
