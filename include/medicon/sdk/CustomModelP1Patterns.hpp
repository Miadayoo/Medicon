#pragma once

#include <string_view>

namespace medicon::sdk::custom_model::p1_native {
// Generated from bounded IDA entries and uniquely checked against the original
// 1.26.60.5 PE. Evidence and ABI: docs/custom_model_p1_report.md.
inline constexpr std::string_view world_render_pattern =
    "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 78 0C 00 00 48 8D AC 24 80 00 00 00 44 0F 29 85 E0 0B 00 00 0F 29 BD D0 0B 00 00 0F 29 B5 C0 0B 00 00 48 C7 85 B8 0B 00 00 FE FF FF FF 4C 89 8D 38";
inline constexpr std::string_view mesh_immediate_pattern =
    "55 41 57 41 56 41 54 56 57 53 48 81 EC C0 02 00 00 48 8D AC 24 80 00 00 00 48 C7 85 38 02 00 00 FE FF FF FF 80 BA 55 02 00 00 00 0F 85 ?? ?? ?? ?? 48 89 D6 48 89 CF 4C 8B A5 A0 02 00 00 4C 89";
inline constexpr std::string_view mesh_submit_pattern =
    "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 38 04 00 00 48 8D AC 24 80 00 00 00 0F 29 B5 A0 03 00 00 48 C7 85 98 03 00 00 FE FF FF FF 4D 89 CC 4D 89 C6 48 89 D3 48 89 CF 4C 8B BD 40 04 00 00";
inline constexpr std::string_view interpolated_position_pattern =
    "56 57 53 48 81 EC 90 00 00 00 44 0F 29 94 24 80 00 00 00 44 0F 29 4C 24 70 44 0F 29 44 24 60 0F 29 7C 24 50 0F 29 74 24 40 0F 28 F2 48 89 D6 48 89 CB E8 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ??";
inline constexpr std::string_view riding_offset_pattern =
    "48 89 C8 4C 8B 42 10 8B 4A 18 49 8B 50 48 4D 8B 48 50 49 29 D1 49 C1 E9 03 41 FF C9 41 81 E1 F2 5B 67 33 4E 8D 14 CA 49 8B 50 68 0F 1F 44 00 00 4D 8B 0A 49 83 F9 FF 0F 84 ?? ?? ?? ?? 49 C1 E1";
inline constexpr std::string_view body_yaw_pattern =
    "48 83 EC 58 44 0F 29 44 24 40 0F 29 7C 24 30 0F 29 74 24 20 0F 28 F1 48 8B 51 10 8B 41 18 48 8B 4A 48 4C 8B 42 50 49 29 C8 49 C1 E8 03 41 FF C8 41 81 E0 BA 4B F6 D7 4E 8D 0C C1 48 8B 4A 68 90";
inline constexpr std::string_view height_offset_pattern =
    "48 83 EC 28 48 8B 51 08 8B 41 10 48 8B 4A 48 4C 8B 42 50 49 29 C8 49 C1 E8 03 41 FF C8 41 81 E0 16 C3 21 DE 4E 8D 0C C1 48 8B 4A 68 0F 1F 40 00 4D 8B 01 49 83 F8 FF 0F 84 ?? ?? ?? ?? 49 C1 E0 05 4E 8D 0C 01 42 81 7C 01 08 16 C3 21 DE 75 ?? 4C 01 C1 48 39 4A 70 0F 84 ?? ?? ?? ?? 48 8B 49 10 48 85 C9 74 ?? 89 C2 81 E2 FF FF 03 00 41 89 D0 41 C1 E8 0B 4C 8B 49 08 4C 8B 51 10 4D 29 CA 49 C1 FA 03 4D 39 D0 73 ?? 4F 8B 04 C1 4D 85 C0 74 ?? 81 E2 FF 07 00 00 25 00 00 FC FF 41 8B 14 90 31 D0 3D FE FF 03 00 77 ?? 48 8B 41 50 89 D1 C1 E9 04 81 E1 F8 3F 00 00 48 8B 04 08 48 85 C0 74 ?? 81 E2 FF FF 03 00 83 E2 7F 89 D1 C1 E1 06 48 8D 0C 91 F3 0F 10 04 08 48 83 C4 28 C3 E8 ?? ?? ?? ?? CC";
inline constexpr std::string_view material_release_pattern =
    "56 48 83 EC 20 48 8B 71 08 48 85 F6 74 ?? F0 FF 4E 08 75 ?? 48 8B 06 48 8B 00 48 89 F1 FF 15 ?? ?? ?? ?? F0 FF 4E 0C 75 ?? 48 8B 06 48 8B 40 08 48 8B 15 ?? ?? ?? ?? 48 89 F1 48 83 C4 20 5E 48";
inline constexpr std::string_view material_anchor_pattern =
    "48 8D 15 ?? ?? ?? ?? 4C 8D 05 ?? ?? ?? ?? 48 8D 4D B0 E8 ?? ?? ?? ?? 90 48 89 74 24 28 66 C7 44 24 20 02 00";
inline constexpr std::string_view binding_copy_pattern =
    "56 57 53 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 63 0C 88 48 01 C1 FF E1 31 FF E9 ?? ?? ?? ?? 48 8B 02 0F 57 C0 0F 11 00 49 8B 48 08 48 85 C9 74 ?? F0 FF 41 08 49 8B 08 48 89 08 49 8B 48 08 48 89";
inline constexpr std::string_view binding_release_pattern =
    "56 57 48 83 EC 28 0F B6 41 20 48 83 F8 03 77 ?? 48 8D 15 ?? ?? ?? ?? 48 63 04 82 48 01 D0 FF E0 48 8B 71 08 48 85 F6 74 ?? F0 FF 4E 08 75 ?? 48 8B 06 48 8B 00 48 89 F1 FF 15 ?? ?? ?? ?? F0 FF";
inline constexpr std::string_view tess_begin_pattern =
    "56 57 55 53 48 83 EC 38 48 8B 05 ?? ?? ?? ?? 48 31 E0 48 89 44 24 30 80 B9 A0 02 00 00 00 0F 85 ?? ?? ?? ?? 48 89 CE 80 B9 55 02 00 00 00 0F 85 ?? ?? ?? ?? 44 89 CF 44 89 C3 0F B6 AC 24 80 00";
inline constexpr std::string_view tess_vertex_pattern =
    "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 78 44 0F 29 4C 24 60 44 0F 29 44 24 50 0F 29 7C 24 40 0F 29 74 24 30 8B 81 98 02 00 00 3B 81 9C 02 00 00 0F 84 ?? ?? ?? ?? 0F 28 F3 48 89 CE FF C0";
inline constexpr std::string_view material_lookup_pattern =
    "55 56 57 48 83 EC 50 48 8D 6C 24 50 0F 29 75 F0 48 C7 45 E8 FE FF FF FF 49 89 D1 0F 57 F6 48 89 4D E0 0F 11 31 48 8B 02 48 8B 40 08 48 8D 55 D0 4C 89 C9 FF 15 ?? ?? ?? ?? 90 0F 28 45 D0 0F 29";
inline constexpr std::string_view mesh_view_pattern =
    "55 41 57 41 56 41 54 56 57 53 48 81 EC C0 02 00 00 48 8D AC 24 80 00 00 00 48 C7 85 38 02 00 00 FE FF FF FF 80 BA 55 02 00 00 00 0F 85 ?? ?? ?? ?? 48 89 D6 48 89 CF 4C 8B A5 A8 02 00 00 0F B7";
}
