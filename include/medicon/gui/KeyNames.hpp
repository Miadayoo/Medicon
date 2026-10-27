#pragma once

#include <string_view>

namespace medicon::gui {

// Win32 virtual-key codes used by GUI routing (kept here so shared GUI headers
// do not need <Windows.h>).
namespace vk {
inline constexpr int back = 0x08;
inline constexpr int escape = 0x1B;
inline constexpr int del = 0x2E;
inline constexpr int xbutton1 = 0x05;
inline constexpr int xbutton2 = 0x06;
} // namespace vk

// Display name for a virtual-key code ("None" for 0). Points at stable storage.
std::string_view key_name(int virtual_key);

} // namespace medicon::gui
