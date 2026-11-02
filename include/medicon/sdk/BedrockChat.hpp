#pragma once

#include <medicon/memory/pattern.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace medicon::sdk {

// These readers copy foreign object bytes. They never create, destroy, or call
// methods on a game-owned STL object, and never treat it as our std::string.
[[nodiscard]] std::optional<std::string> read_msvc_string(const void* object,
                                                         std::size_t maximum = 1024);
[[nodiscard]] std::optional<std::string> read_text_message(const void* packet);

// The shared outgoing interception path. A trusted validator must approve both
// the foreign vtable and its scalar getId function before any game code runs.
// Returns true only for local dot commands, including when enqueueing fails.
using ChatCodeValidator = std::function<bool(std::uintptr_t vtable, std::uintptr_t get_id)>;
bool consume_outgoing_chat(void* packet, const ChatCodeValidator& validate,
                           const std::function<void(std::string)>& enqueue) noexcept;

enum class ClientMouseAction { grab, release };
namespace detail {
// Read the primary client's borrowed pointer without copying native shared_ptrs.
// The caller must verify the executable/profile before using this layout.
[[nodiscard]] std::uintptr_t read_primary_client(std::uintptr_t game) noexcept;
}
// Scalar native-call boundary shared with the isolated routing tests. The
// validator must approve the client vtable and selected native entry point.
bool invoke_client_mouse(void* client, ClientMouseAction action, const ChatCodeValidator& validate) noexcept;

// Non-owning Microsoft string ABI envelope for a native function proven to
// copy its input. Keep backing alive and unchanged until that function returns.
[[nodiscard]] std::array<std::byte, 32> borrowed_msvc_string(const std::string& backing) noexcept;

// A deliberately small ABI bridge for outgoing chat and local native feedback. The owner
// serializes locate() with hook installation; consume() then uses immutable data.
class BedrockChat {
public:
    enum class State { waiting, ready, unsupported };

    State locate(std::string& diagnostic);
    [[nodiscard]] bool feedback_available() const noexcept {
        return profile_ready_.load(std::memory_order_acquire) && display_target_ != 0;
    }
    [[nodiscard]] std::uintptr_t send_target() const noexcept { return send_target_; }
    // Native automatic latency echoes call sendToServer (slot 4) directly,
    // bypassing the chat/general send entry (slot 2).
    [[nodiscard]] std::uintptr_t server_send_target() const noexcept { return server_send_target_; }
    bool consume(void* packet, const std::function<void(std::string)>& enqueue) const noexcept;
    [[nodiscard]] std::uintptr_t tick_target(std::string& diagnostic, bool require_feedback = true) const noexcept;
    [[nodiscard]] bool is_local_player(void* actor) const noexcept;
    // Fresh native getter, for use only within a native render callback.
    // Borrowed until that callback returns; never retain it across frames.
    [[nodiscard]] void* local_player_for_render() const noexcept {
        return reinterpret_cast<void*>(local_player());
    }
    // Invoke only from the matching local-player baseTick callback, never from
    // Present or an arbitrary worker. Revalidates the current client/player/GUI.
    bool display_message(void* actor, std::string_view message, std::string& diagnostic) const noexcept;
    [[nodiscard]] std::uintptr_t grab_mouse_target(std::string& diagnostic) const noexcept;
    [[nodiscard]] void* mouse_client() const noexcept;
    [[nodiscard]] bool is_current_client(void* client) const noexcept;
    // Run mouse transitions on the Minecraft window's owner thread: the native
    // platform implementation directly uses ShowCursor/SetCapture, without a
    // dispatcher. No local player is required, so title screens are supported.
    bool release_mouse(std::string& diagnostic) const noexcept;
    bool grab_mouse(std::string& diagnostic) const noexcept;
    // Actual native capture state (true means grabbed), unlike the upstream
    // ClientInstance helper whose getMouseGrabbed() intentionally inverts it.
    [[nodiscard]] std::optional<bool> mouse_grabbed(std::string& diagnostic) const noexcept;

private:
    [[nodiscard]] bool executable(std::uintptr_t address) const noexcept;
    [[nodiscard]] bool object_vtable(std::uintptr_t object,
                                     std::array<std::uintptr_t, 5>& methods) const noexcept;
    [[nodiscard]] std::uintptr_t current_client() const noexcept;
    [[nodiscard]] std::uintptr_t primary_client(bool require_sender) const noexcept;
    [[nodiscard]] std::uintptr_t local_player() const noexcept;
    [[nodiscard]] std::uintptr_t mouse_game(std::string& diagnostic) const noexcept;
    [[nodiscard]] std::uintptr_t mouse_target(std::size_t slot, std::string& diagnostic) const noexcept;
    bool change_mouse(ClientMouseAction action, std::string& diagnostic) const noexcept;

    State state_ = State::waiting;
    std::atomic_bool profile_ready_{false};
    bool scanned_ = false;
    std::uintptr_t game_global_ = 0;
    std::uintptr_t send_target_ = 0;
    std::uintptr_t server_send_target_ = 0;
    std::uintptr_t display_target_ = 0;
    std::uintptr_t verified_client_ = 0;
    // mouse_grabbed() is polled while the ClickGUI is open. Re-reading the
    // capture byte is enough until the game's vtable identity changes.
    std::string profile_name_;
    mutable std::uintptr_t cached_mouse_game_ = 0;
    mutable std::uintptr_t cached_mouse_table_ = 0;
    memory::AddressRange image_;
    std::vector<memory::AddressRange> executable_regions_;
    std::string version_;
    std::string terminal_diagnostic_;
};

} // namespace medicon::sdk
