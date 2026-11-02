#include <medicon/sdk/BedrockChat.hpp>
#include <medicon/sdk/BedrockProfile.hpp>

#include <medicon/memory/module_view.hpp>
#include <medicon/memory/signature_catalog.hpp>
#include <medicon/memory/signature_registry.hpp>

#include <Windows.h>
#include <winver.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>

namespace medicon::sdk {
namespace {

// Layout/protocol reference only; this bridge does not import the upstream SDK:
// https://github.com/surukotonai/solstice/tree/cc6463dcadfeadb9049c235134c6b46efdc12b7e/src
// SDK/OffsetProvider.hpp and the independently inspected 1.26.60.5 image.
// Hook/Hooks/NetworkHooks/PacketSendHook.cpp: sender vtable[2], Text packet ID 9.
// SDK/Minecraft/Network/Packets/TextPacket.hpp: AuthorAndMessage variant.
// The primary-client path is read without calling the upstream getter: its
// output owns native shared_ptr references. Never synthesize those in this DLL.
namespace profile = bedrock_1_26_60_5;
constexpr std::size_t maximum_message = 1024;
constexpr std::size_t body_offset = 0xA0;
constexpr std::size_t message_offset = 0xC8;
constexpr std::size_t variant_index_offset = 0xE8;

void set_diagnostic(std::string& destination, const char* message) noexcept {
    try { destination = message; }
    catch (...) { destination.clear(); }
}

bool copy_bytes(std::uintptr_t address, void* destination, std::size_t length) noexcept {
    if (!address || !destination || !length ||
        length - 1 > std::numeric_limits<std::uintptr_t>::max() - address) return false;
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                             destination, length, &copied) != FALSE && copied == length;
}

template<class T>
bool read_at(std::uintptr_t base, std::size_t offset, T& result) noexcept {
    if (offset > std::numeric_limits<std::uintptr_t>::max() - base) return false;
    return copy_bytes(base + offset, &result, sizeof(result));
}

std::optional<std::string> supported_executable_version(std::string& error) {
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) {
        error = "Cannot inspect the game executable path.";
        return std::nullopt;
    }
    const std::wstring_view full_path(path.data(), length);
    const auto separator = full_path.find_last_of(L"\\/");
    const auto name = full_path.substr(separator == std::wstring_view::npos ? 0 : separator + 1);
    if (_wcsicmp(std::wstring(name).c_str(), L"Minecraft.Windows.exe") != 0) {
        error = "Chat SDK bridge is only supported in Minecraft.Windows.exe 1.26.60.5.";
        return std::nullopt;
    }
    DWORD ignored = 0;
    const DWORD bytes = GetFileVersionInfoSizeW(path.data(), &ignored);
    if (!bytes || bytes > 16 * 1024 * 1024) {
        error = "Cannot read Minecraft's executable file version; chat SDK bridge withheld.";
        return std::nullopt;
    }
    std::vector<std::byte> data(bytes);
    if (!GetFileVersionInfoW(path.data(), 0, bytes, data.data())) {
        error = "Cannot read Minecraft's executable file version; chat SDK bridge withheld.";
        return std::nullopt;
    }
    VS_FIXEDFILEINFO* info = nullptr;
    UINT info_bytes = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &info_bytes) ||
        !info || info_bytes < sizeof(*info) || info->dwSignature != 0xFEEF04BD) {
        error = "Minecraft's executable file version is invalid; chat SDK bridge withheld.";
        return std::nullopt;
    }
    const unsigned major = HIWORD(info->dwFileVersionMS);
    const unsigned minor = LOWORD(info->dwFileVersionMS);
    const unsigned build = HIWORD(info->dwFileVersionLS);
    const unsigned revision = LOWORD(info->dwFileVersionLS);
    const auto version = std::to_string(major) + "." + std::to_string(minor) + "." +
                         std::to_string(build) + "." + std::to_string(revision);
    if (major != 1 || minor != 26 || build != 60 || revision != 5) {
        error = "Chat SDK bridge does not support Minecraft " + version + "; expected 1.26.60.5.";
        return std::nullopt;
    }
    return version;
}

} // namespace

std::uintptr_t detail::read_primary_client(std::uintptr_t game) noexcept {
    std::uintptr_t head = 0, node = 0;
    std::uint8_t sentinel = 0;
    if (!read_at(game, profile::primary_clients, head) || !head ||
        !read_at(head, 0x19, sentinel) || sentinel != 1 || !read_at(head, 8, node)) return 0;
    // The getter at 0x8BCFC0 reads (game+0x30)+0x9A0, walks root->left,
    // selects key 0, then returns node+0x38 after checking client+8's flag.
    for (unsigned depth = 0; depth < 256; ++depth) {
        if (!node || node == head || !read_at(node, 0x19, sentinel)) return 0;
        if (sentinel != 0) return 0;
        std::uintptr_t left = 0;
        if (!read_at(node, 0, left) || !left || !read_at(left, 0x19, sentinel) || sentinel > 1) return 0;
        if (sentinel == 0) { node = left; continue; }
        std::uint8_t key = 255, alive = 0;
        std::uintptr_t client = 0, lifetime = 0;
        if (!read_at(node, 0x20, key) || key != 0 ||
            !read_at(node, profile::primary_client_node, client) || !client ||
            !read_at(client, 8, lifetime) || !read_at(lifetime, 0, alive) || alive != 1) return 0;
        return client;
    }
    return 0;
}

std::optional<std::string> read_msvc_string(const void* object, std::size_t maximum) {
    // Microsoft release x64 basic_string<char>: 16-byte buffer/pointer union,
    // uint64 size, uint64 capacity. Capacity 15 selects the inline buffer.
    // https://devblogs.microsoft.com/oldnewthing/20230803-00/?p=108532/
    // Only bytes are interpreted; libc++ std::string has a different layout.
    std::array<std::byte, 32> raw{};
    const auto address = reinterpret_cast<std::uintptr_t>(object);
    if (!copy_bytes(address, raw.data(), raw.size())) return std::nullopt;
    std::uint64_t size = 0, capacity = 0;
    std::memcpy(&size, raw.data() + 16, sizeof(size));
    std::memcpy(&capacity, raw.data() + 24, sizeof(capacity));
    maximum = std::min(maximum, maximum_message);
    if (size > maximum || size > capacity || capacity > maximum_message) return std::nullopt;
    if (capacity < 16) {
        if (capacity != 15 || size > 15 || raw[static_cast<std::size_t>(size)] != std::byte{})
            return std::nullopt;
        return std::string(reinterpret_cast<const char*>(raw.data()), static_cast<std::size_t>(size));
    }
    std::uintptr_t text = 0;
    std::memcpy(&text, raw.data(), sizeof(text));
    std::string copied(static_cast<std::size_t>(size) + 1, '\0');
    if (!copy_bytes(text, copied.data(), copied.size()) || copied.back() != '\0') return std::nullopt;
    copied.pop_back();
    return copied;
}

std::optional<std::string> read_text_message(const void* packet) {
    const auto address = reinterpret_cast<std::uintptr_t>(packet);
    std::uint8_t index = 0, type = 0;
    if (!address || !read_at(address, variant_index_offset, index) || index != 1 ||
        !read_at(address, body_offset, type) || type != 1 ||
        message_offset > std::numeric_limits<std::uintptr_t>::max() - address) return std::nullopt;
    return read_msvc_string(reinterpret_cast<const void*>(address + message_offset));
}

std::array<std::byte, 32> borrowed_msvc_string(const std::string& backing) noexcept {
    std::array<std::byte, 32> header{};
    const auto size = static_cast<std::uint64_t>(backing.size());
    const auto capacity = size <= 15 ? std::uint64_t{15} : size;
    if (size <= 15) std::memcpy(header.data(), backing.c_str(), backing.size());
    else {
        const auto data = reinterpret_cast<std::uintptr_t>(backing.c_str());
        std::memcpy(header.data(), &data, sizeof(data));
    }
    std::memcpy(header.data() + 16, &size, sizeof(size));
    std::memcpy(header.data() + 24, &capacity, sizeof(capacity));
    return header;
}

bool BedrockChat::executable(std::uintptr_t address) const noexcept {
    // The regions were verified committed+executable by VirtualQuery when the
    // profile was built. Do not query again here: on Minecraft's resident
    // ~300MB image VirtualQuery walks every PTE to the end of the region
    // (~1.5ms per call), and this runs many times on every Present.
    return image_.contains(address) && std::ranges::any_of(executable_regions_,
        [address](const auto& region) { return region.contains(address); });
}

bool BedrockChat::object_vtable(std::uintptr_t object,
                               std::array<std::uintptr_t, 5>& methods) const noexcept {
    std::uintptr_t vtable = 0;
    if (!object || object % alignof(std::uintptr_t) || !read_at(object, 0, vtable) ||
        !image_.contains(vtable, sizeof(methods)) || !copy_bytes(vtable, methods.data(), sizeof(methods))) return false;
    return std::ranges::all_of(methods, [this](auto address) { return executable(address); });
}

BedrockChat::State BedrockChat::locate(std::string& diagnostic) {
    diagnostic.clear();
    if (state_ != State::waiting) {
        diagnostic = terminal_diagnostic_;
        return state_;
    }
    const auto unsupported = [&](std::string message) {
        state_ = State::unsupported;
        terminal_diagnostic_ = std::move(message);
        diagnostic = terminal_diagnostic_;
        return state_;
    };
    if (!scanned_) {
        scanned_ = true;
        auto version = supported_executable_version(diagnostic);
        if (!version) return unsupported(diagnostic);
        version_ = std::move(*version);
        auto module = memory::current_process_module(&diagnostic);
        if (!module) return unsupported(diagnostic);
        image_ = module->image;
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        if (!copy_bytes(image_.address, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
            dos.e_lfanew <= 0 || !image_.contains(image_.address + dos.e_lfanew, sizeof(nt)) ||
            !copy_bytes(image_.address + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            !profile::known_image(nt.FileHeader.TimeDateStamp, nt.OptionalHeader.SizeOfImage))
            return unsupported("Chat SDK bridge requires the inspected 1.26.60.5 PE profile.");
        for (const auto& region : module->executable_regions)
            executable_regions_.push_back({region.address, region.bytes.size()});
        const auto definitions = memory::solstice_signatures();
        const auto found = std::ranges::find_if(definitions,
            [](const auto& entry) { return entry.name == "MinecraftGame_instance"; });
        if (found == definitions.end()) return unsupported("MinecraftGame_instance signature is unavailable.");
        std::vector<memory::SignatureDefinition> required{*found};
        const auto display = std::ranges::find_if(definitions,
            [](const auto& entry) { return entry.name == "GuiData_displayClientMessage"; });
        if (display != definitions.end()) required.push_back(*display);
        memory::SignatureRegistry signatures;
        signatures.scan(required,
                        module->executable_regions, image_);
        const auto resolved = signatures.address("MinecraftGame_instance");
        if (!resolved || !image_.contains(*resolved, sizeof(std::uintptr_t))) {
            const auto* detail = signatures.find("MinecraftGame_instance");
            return unsupported("MinecraftGame_instance signature did not resolve uniquely: " +
                               std::string(detail ? memory::to_string(detail->status) : "missing"));
        }
        game_global_ = *resolved;
        const auto display_address = signatures.address("GuiData_displayClientMessage");
        if (display_address && executable(*display_address)) display_target_ = *display_address;
        // The mouse bridge can work at the title screen while chat discovery is
        // still waiting for a sender. Publish only the now-immutable profile;
        // mouse calls never read the subsequently changing chat state fields.
        profile_ready_.store(true, std::memory_order_release);
    }
    const auto instance = primary_client(false);
    if (!instance) {
        diagnostic = "Waiting for a valid Minecraft primary client instance.";
        return State::waiting;
    }
    std::uintptr_t sender = 0;
    std::array<std::uintptr_t, 5> sender_methods{};
    if (!read_at(instance, profile::client_sender, sender) || !object_vtable(sender, sender_methods)) {
        diagnostic = "Waiting for a validated client/packet-sender vtable for Minecraft " + version_ + ".";
        return State::waiting;
    }
    send_target_ = sender_methods[2];
    server_send_target_ = sender_methods[4];
    verified_client_ = instance;
    state_ = State::ready;
    terminal_diagnostic_ = "Chat SDK bridge ready for Minecraft " + version_ + " (validated primary client node +0x38).";
    diagnostic = terminal_diagnostic_;
    return state_;
}

bool consume_outgoing_chat(void* packet, const ChatCodeValidator& validate,
                           const std::function<void(std::string)>& enqueue) noexcept {
    bool recognized_command = false;
    try {
        if (!packet || !validate) return false;
        const auto address = reinterpret_cast<std::uintptr_t>(packet);
        std::uintptr_t vtable = 0, get_id = 0;
        if (!read_at(address, 0, vtable) || !vtable ||
            !read_at(vtable, sizeof(std::uintptr_t), get_id) || !validate(vtable, get_id)) return false;
        // Scalar/pointer Win64 ABI only. Do not use SDK C++ virtual classes or
        // game std::string/std::variant methods across the MinGW/MSVC boundary.
        using GetId = int(__fastcall*)(void*);
        if (reinterpret_cast<GetId>(get_id)(packet) != 9) return false;
        auto message = read_text_message(packet);
        if (!message || message->empty() || message->front() != '.') return false;
        recognized_command = true;
        if (enqueue) enqueue(std::move(*message));
        return true;
    } catch (...) {
        // Once a valid dot command is recognized, keep it local even if the
        // command queue cannot allocate. Never send it to the server on failure.
        // Unrecognized/malformed packets still retain the host's original send.
        return recognized_command;
    }
}

bool BedrockChat::consume(void* packet, const std::function<void(std::string)>& enqueue) const noexcept {
    if (state_ != State::ready) return false;
    try {
        return consume_outgoing_chat(packet, [this](auto vtable, auto get_id) {
            return image_.contains(vtable, 2 * sizeof(std::uintptr_t)) && executable(get_id);
        }, enqueue);
    } catch (...) {
        return false;
    }
}

bool invoke_client_mouse(void* client, ClientMouseAction action, const ChatCodeValidator& validate) noexcept {
    try {
        if (!client || !validate || (action != ClientMouseAction::grab && action != ClientMouseAction::release))
            return false;
        const std::size_t slot = action == ClientMouseAction::grab ? profile::grab_mouse_slot : profile::release_mouse_slot;
        std::uintptr_t table = 0, function = 0;
        if (!read_at(reinterpret_cast<std::uintptr_t>(client), 0, table) || !table ||
            !read_at(table, slot * sizeof(std::uintptr_t), function) || !validate(table, function)) return false;
        using NativeMouse = void(__fastcall*)(void*);
        reinterpret_cast<NativeMouse>(function)(client);
        return true;
    } catch (...) { return false; }
}

std::uintptr_t BedrockChat::current_client() const noexcept {
    if (state_ != State::ready || !verified_client_) return 0;
    return primary_client(true);
}

std::uintptr_t BedrockChat::primary_client(bool require_sender) const noexcept {
    if (!profile_ready_.load(std::memory_order_acquire)) return 0;
    std::uintptr_t game = 0, game_table = 0;
    if (!read_at(game_global_, 0, game) || !read_at(game, 0, game_table) ||
        game_table != image_.address + profile::game_vtable) return 0;
    const auto candidate = detail::read_primary_client(game);
    std::uintptr_t table = 0, associated_game = 0, grab = 0, release = 0;
    if (!candidate || !read_at(candidate, 0, table) || table != image_.address + profile::client_vtable ||
        !image_.contains(table, (profile::release_mouse_slot + 1) * sizeof(std::uintptr_t)) ||
        !read_at(candidate, profile::client_game, associated_game) || associated_game != game ||
        !read_at(table, profile::grab_mouse_slot * sizeof(std::uintptr_t), grab) ||
        grab != image_.address + profile::grab_mouse || !executable(grab) ||
        !read_at(table, profile::release_mouse_slot * sizeof(std::uintptr_t), release) ||
        release != image_.address + profile::release_mouse || !executable(release)) return 0;
    if (require_sender) {
        std::uintptr_t sender = 0;
        std::array<std::uintptr_t, 5> methods{};
        if (!read_at(candidate, profile::client_sender, sender) || !object_vtable(sender, methods) ||
            methods[2] != send_target_ || methods[4] != server_send_target_) return 0;
    }
    return candidate;
}

std::uintptr_t BedrockChat::local_player() const noexcept {
    try {
        const auto client = current_client();
        std::uintptr_t table = 0, getter = 0;
        if (!client || !read_at(client, 0, table) || !image_.contains(table, 32 * sizeof(std::uintptr_t)) ||
            !read_at(table, profile::local_player_slot * sizeof(std::uintptr_t), getter) ||
            getter != image_.address + profile::local_player_getter || !executable(getter)) return 0;
        // 1.26.60.5 getter30 RVA 0x659E470 uses the client's internal
        // weak-handle +0x268 and returns a raw pointer in RAX. All temporary
        // handle ownership remains inside the native function.
        using Getter = void*(__fastcall*)(void*);
        return reinterpret_cast<std::uintptr_t>(reinterpret_cast<Getter>(getter)(reinterpret_cast<void*>(client)));
    } catch (...) { return 0; }
}

std::uintptr_t BedrockChat::tick_target(std::string& diagnostic, bool require_feedback) const noexcept {
    diagnostic.clear();
    try {
        if (require_feedback && (!display_target_ || !executable(display_target_))) {
            set_diagnostic(diagnostic, "Native chat feedback signature is unavailable.");
            return 0;
        }
        const auto player = local_player();
        std::uintptr_t table = 0, tick = 0;
        if (!player || !read_at(player, 0, table) || !image_.contains(table, 26 * sizeof(std::uintptr_t)) ||
            !read_at(table, 25 * sizeof(std::uintptr_t), tick) || !executable(tick)) {
            set_diagnostic(diagnostic, "Waiting for the local player's native chat feedback tick.");
            return 0;
        }
        return tick;
    } catch (...) {
        set_diagnostic(diagnostic, "Could not validate the native chat feedback tick.");
        return 0;
    }
}

bool BedrockChat::is_local_player(void* actor) const noexcept {
    return actor && reinterpret_cast<std::uintptr_t>(actor) == local_player();
}

bool BedrockChat::display_message(void* actor, std::string_view message, std::string& diagnostic) const noexcept {
    diagnostic.clear();
    try {
        if (!is_local_player(actor)) {
            set_diagnostic(diagnostic, "Native feedback requires the current local-player tick.");
            return false;
        }
        const auto client = current_client();
        std::uintptr_t gui = 0;
        std::array<std::byte, 8> gui_probe{};
        if (!display_target_ || !executable(display_target_) || !client ||
            !read_at(client, profile::client_gui, gui) || !gui || !copy_bytes(gui, gui_probe.data(), gui_probe.size())) {
            set_diagnostic(diagnostic, "The current Minecraft GUI data is unavailable for native feedback.");
            return false;
        }
        if (message.empty() || message.size() > 16384) {
            set_diagnostic(diagnostic, "Native chat feedback must contain between 1 and 16384 bytes.");
            return false;
        }
        const std::string backing(message);
        alignas(8) const auto header = borrowed_msvc_string(backing);
        alignas(8) const std::array<std::byte, 0x120> metadata{};
        // Inspected in 1.26.60.5 at display RVA 0xE927B0: the message
        // envelope is read and copied; never modified or freed. Metadata has a
        // disengaged optional<string> marker at +0x20. Native copies allocate
        // and release using the game's allocator, not the DLL's libc++ runtime.
        using Display = void(__fastcall*)(void*, const void*, const void*, bool);
        reinterpret_cast<Display>(display_target_)(reinterpret_cast<void*>(gui), header.data(), metadata.data(), false);
        return true;
    } catch (...) {
        set_diagnostic(diagnostic, "Native chat feedback could not be delivered.");
        return false;
    }
}

std::uintptr_t BedrockChat::mouse_game(std::string& diagnostic) const noexcept {
    try {
        const auto client = primary_client(false);
        std::uintptr_t game = 0, associated_game = 0, table = 0, grab = 0, release = 0;
        if (!client || !read_at(game_global_, 0, game) || !game ||
            !read_at(client, profile::client_game, associated_game) || associated_game != game ||
            !read_at(game, 0, table) || !image_.contains(table, 137 * sizeof(std::uintptr_t)) ||
            !read_at(table, 135 * sizeof(std::uintptr_t), grab) || !executable(grab) ||
            !read_at(table, 136 * sizeof(std::uintptr_t), release) || !executable(release)) {
            set_diagnostic(diagnostic, "The current Minecraft mouse context is unavailable.");
            return 0;
        }
        // 1.26.60.5 game grab135 (RVA 0x8BF910) tests capture byte +0x1F8.
        // Release136 (0x8BFB00) tests ==1 and clears it before the platform call.
        // Check those field accesses before reading the byte, rather than trust
        // the upstream's formerly uncertain offset or manually writing a flag.
        std::array<std::byte, 64> grab_code{}, release_code{};
        constexpr std::array<std::byte, 7> grab_check{
            std::byte{0x80}, std::byte{0xB9}, std::byte{0xF8}, std::byte{0x01},
            std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
        constexpr std::array<std::byte, 7> release_check{
            std::byte{0x80}, std::byte{0xB9}, std::byte{0xF8}, std::byte{0x01},
            std::byte{0x00}, std::byte{0x00}, std::byte{0x01}};
        constexpr std::array<std::byte, 7> release_write{
            std::byte{0xC6}, std::byte{0x81}, std::byte{0xF8}, std::byte{0x01},
            std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
        const auto contains = [](const auto& code, const auto& pattern) {
            return std::search(code.begin(), code.end(), pattern.begin(), pattern.end()) != code.end();
        };
        if (!copy_bytes(grab, grab_code.data(), grab_code.size()) ||
            !copy_bytes(release, release_code.data(), release_code.size()) ||
            !contains(grab_code, grab_check) || !contains(release_code, release_check) ||
            !contains(release_code, release_write)) {
            set_diagnostic(diagnostic, "Minecraft's native mouse-state layout could not be verified.");
            return 0;
        }
        return game;
    } catch (...) {
        set_diagnostic(diagnostic, "Could not validate Minecraft's native mouse context.");
        return 0;
    }
}

std::uintptr_t BedrockChat::mouse_target(std::size_t slot, std::string& diagnostic) const noexcept {
    diagnostic.clear();
    if (slot != profile::grab_mouse_slot && slot != profile::release_mouse_slot) return 0;
    try {
        if (!mouse_game(diagnostic)) return 0;
        const auto client = primary_client(false);
        std::uintptr_t table = 0, target = 0, primary_check = 0;
        if (!client || !read_at(client, 0, table) || !image_.contains(table, (slot + 1) * sizeof(std::uintptr_t)) ||
            !read_at(table, slot * sizeof(std::uintptr_t), target) || !executable(target) ||
            !read_at(table, profile::primary_test_slot * sizeof(std::uintptr_t), primary_check) ||
            primary_check != image_.address + profile::primary_test || !executable(primary_check)) {
            set_diagnostic(diagnostic, "Minecraft's native mouse method is unavailable.");
            return 0;
        }
        // The native wrappers themselves perform the primary-client check
        // (vtable160) and dispatch to MinecraftGame. No guessed state is written.
        return target;
    } catch (...) {
        set_diagnostic(diagnostic, "Could not resolve Minecraft's native mouse method.");
        return 0;
    }
}

std::uintptr_t BedrockChat::grab_mouse_target(std::string& diagnostic) const noexcept {
    return mouse_target(profile::grab_mouse_slot, diagnostic);
}

bool BedrockChat::is_current_client(void* client) const noexcept {
    return client && client == mouse_client();
}

void* BedrockChat::mouse_client() const noexcept {
    return reinterpret_cast<void*>(primary_client(false));
}

std::optional<bool> BedrockChat::mouse_grabbed(std::string& diagnostic) const noexcept {
    diagnostic.clear();
    if (cached_mouse_game_) {
        std::uintptr_t table = 0;
        std::uint8_t state = 0;
        if (read_at(cached_mouse_game_, 0, table) && table == cached_mouse_table_ &&
            read_at(cached_mouse_game_, profile::mouse_grabbed, state) && state <= 1) return state == 1;
        cached_mouse_game_ = cached_mouse_table_ = 0;
    }
    const auto game = mouse_game(diagnostic);
    std::uint8_t state = 0;
    if (!game || !read_at(game, profile::mouse_grabbed, state) || state > 1) {
        if (diagnostic.empty()) diagnostic = "Native mouse capture state is not readable.";
        return std::nullopt;
    }
    std::uintptr_t table = 0;
    if (read_at(game, 0, table)) {
        cached_mouse_game_ = game;
        cached_mouse_table_ = table;
    }
    return state == 1;
}

bool BedrockChat::change_mouse(ClientMouseAction action, std::string& diagnostic) const noexcept {
    diagnostic.clear();
    try {
        const auto slot = action == ClientMouseAction::grab ? profile::grab_mouse_slot : profile::release_mouse_slot;
        const auto target = mouse_target(slot, diagnostic);
        const auto client = primary_client(false);
        if (!target || !client) return false;
        if (!invoke_client_mouse(reinterpret_cast<void*>(client), action, [this, target, slot](auto table, auto function) {
                return image_.contains(table, (slot + 1) * sizeof(std::uintptr_t)) &&
                       function == target && executable(function);
            })) {
            set_diagnostic(diagnostic, "Minecraft's native mouse transition could not be called.");
            return false;
        }
        const auto state = mouse_grabbed(diagnostic);
        if (!state || *state != (action == ClientMouseAction::grab)) {
            if (diagnostic.empty()) set_diagnostic(diagnostic, "Minecraft did not accept the native mouse capture transition.");
            return false;
        }
        return true;
    } catch (...) {
        set_diagnostic(diagnostic, "Minecraft's native mouse transition failed.");
        return false;
    }
}

bool BedrockChat::release_mouse(std::string& diagnostic) const noexcept {
    return change_mouse(ClientMouseAction::release, diagnostic);
}

bool BedrockChat::grab_mouse(std::string& diagnostic) const noexcept {
    return change_mouse(ClientMouseAction::grab, diagnostic);
}

} // namespace medicon::sdk
