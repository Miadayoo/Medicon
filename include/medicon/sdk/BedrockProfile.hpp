#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace medicon::sdk::bedrock_1_26_60_5 {

// Solstice cc6463d, independently checked in the supplied 1.26.60.05 IDB.
// See docs/bedrock-1.26.60.md. These values do not apply to 1.26.52.
inline constexpr std::string_view file_version = "1.26.60.5";
inline constexpr std::uint32_t timestamp = 0x6AD79EE4;
inline constexpr std::uint32_t image_size = 0x1383B000;
inline constexpr std::uint32_t launcher_image_size = 0x1383C000;
struct Packaging { std::uint32_t image_size, checksum; };
// The launcher adds .levildr; all eight original sections are byte-identical.
// See docs/bedrock-1.26.60.05-packaging-evidence.json (IDA MCP comparison).
inline constexpr std::array packagings{
    Packaging{image_size, 0x134D2DEF},
    Packaging{launcher_image_size, 0x134D3CBA}
};
[[nodiscard]] constexpr bool known_image(std::uint32_t stamp, std::uint32_t size) noexcept {
    if (stamp != timestamp) return false;
    for (const auto& packaging : packagings)
        if (size == packaging.image_size) return true;
    return false;
}
[[nodiscard]] constexpr bool known_packaging(std::uint32_t stamp, std::uint32_t size,
                                             std::uint32_t checksum) noexcept {
    if (stamp != timestamp) return false;
    for (const auto& packaging : packagings)
        if (size == packaging.image_size && checksum == packaging.checksum) return true;
    return false;
}

inline constexpr std::uintptr_t client_vtable = 0xF2B9330;
inline constexpr std::uintptr_t game_vtable = 0xF091B70;
inline constexpr std::uintptr_t primary_client_getter = 0x8BCFC0;
inline constexpr std::uintptr_t local_player_getter = 0x659E470;
inline constexpr std::uintptr_t primary_test = 0x65B0040;
inline constexpr std::uintptr_t grab_mouse = 0x65BB0A0;
inline constexpr std::uintptr_t release_mouse = 0x65BB0F0;
inline constexpr std::uintptr_t game_grab_mouse = 0x8BF910;
inline constexpr std::uintptr_t game_release_mouse = 0x8BFB00;
inline constexpr std::uintptr_t mouse_feed = 0x427650;
inline constexpr std::uintptr_t look_input = 0x6EEBA30;

inline constexpr std::size_t primary_clients = 0x9D0;
inline constexpr std::size_t primary_client_node = 0x38;
inline constexpr std::size_t client_game = 0x1C8;
inline constexpr std::size_t client_sender = 0x1F0;
inline constexpr std::size_t client_gui = 0x668;
inline constexpr std::size_t mouse_grabbed = 0x1F8;
inline constexpr std::size_t local_player_slot = 30;
inline constexpr std::size_t primary_test_slot = 160;
inline constexpr std::size_t grab_mouse_slot = 306;
inline constexpr std::size_t release_mouse_slot = 307;

} // namespace medicon::sdk::bedrock_1_26_60_5
