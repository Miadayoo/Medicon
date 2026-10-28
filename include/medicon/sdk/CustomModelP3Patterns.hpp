#pragma once
#include <string_view>
namespace medicon::sdk::custom_model::p3_native {
// P3 correction: read-only options profile; no additional native hooks/calls.
inline constexpr std::string_view view_options = "48 8B 81 10 0E 00 00 C3";
inline constexpr std::string_view view_option_ref = "56 48 83 EC 20 48 89 D6 44 89 C2 E8 B0 FD FF FF 48 89 06 48 89 F0 48 83 C4 20 5E C3";
inline constexpr std::string_view view_option_array = "55 56 57 48 81 EC 80 00 00 00 48 8D AC 24 80 00 00 00 48 C7 45 F8 FE FF FF FF 81 FA 35 03 00 00 77 0C 89 D0 48 8B 44 C1 10 48 85 C0 75 28";
// Bounded native state-write evidence; scanned only, never hooked/called.
inline constexpr std::string_view swing_update = "56 48 83 EC 20 80 B9 34 04 00 00 00 74 34 FF 81 10 04 00 00 48 89 CE E8 84 6A 90 FD 48 89 F1 8B 96 10 04 00 00 39 C2 7D 12 85 C0 74 1F F3 0F 2A C2 F3 0F 2A C8 F3 0F 5E C1 EB 14 C6 81 34 04 00";
inline constexpr std::string_view swing_previous = "55 56 57 48 81 EC 90 00 00 00 48 8D AC 24 80 00 00 00 48 C7 45 08 FE FF FF FF 48 89 CE F3 0F 10 81 0C 04 00 00 F3 0F 11 81 50 04 00 00 E8 8E 21 98 FF F3 0F 10 86 B8 03 00 00 F3 0F 11 86 B4 03";
}
