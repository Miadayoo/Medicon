#pragma once
#include <string_view>
namespace medicon::sdk::custom_model::p3_native {
// P3 correction: read-only options profile; no additional native hooks/calls.
inline constexpr std::string_view view_options =
    "48 8B 81 00 0E 00 00 C3";
inline constexpr std::string_view view_option_ref =
    "56 48 83 EC 20 48 89 D6 44 89 C2 E8 ?? ?? ?? ?? 48 89 06 48 89 F0 48 83 C4 20 5E C3 CC CC CC CC 56 57 53 48 83 EC 20 44 89 C7 48 89 D6 48 89 CB 48 8B 01 48 8B 80 D0 00 00 00 44 89 C2 FF 15 AD";
inline constexpr std::string_view view_option_array =
    "55 56 57 48 81 EC 80 00 00 00 48 8D AC 24 80 00 00 00 48 C7 45 F8 FE FF FF FF 81 FA 38 03 00 00 77 ?? 89 D0 48 8B 44 C1 10 48 85 C0 75 ?? 8B 0D ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? 65 48 8B 14 25 58";
// Bounded native state-write evidence; scanned only, never hooked/called.
inline constexpr std::string_view swing_update =
    "56 48 83 EC 20 80 B9 5D 04 00 00 00 74 ?? FF 81 30 04 00 00 0F B6 91 5C 04 00 00 48 89 CE E8 ?? ?? ?? ?? 48 89 F1 8B 96 30 04 00 00 39 C2 7D ?? 85 C0 74 ?? F3 0F 2A C2 F3 0F 2A C8 F3 0F 5E C1 EB ?? C6 81 5D 04 00 00 00 C7 81 30 04 00 00 00 00 00 00 0F 57 C0 F3 0F 11 81 2C 04 00 00 48 83 C4 20 5E E9 ?? ?? ?? ?? CC CC CC CC CC CC CC CC 55 41 57 41";
inline constexpr std::string_view swing_previous =
    "55 56 57 48 81 EC 90 00 00 00 48 8D AC 24 80 00 00 00 48 C7 45 08 FE FF FF FF 48 89 CE F3 0F 10 81 2C 04 00 00 F3 0F 11 81 78 04 00 00 E8 ?? ?? ?? ?? F3 0F 10 86 D8 03 00 00 F3 0F 11 86 D4 03";
}
