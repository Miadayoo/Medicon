#include <medicon/sdk/BedrockChat.hpp>

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
// https://github.com/surukotonai/solstice/tree/1d7808f52ef4ba6e34a7fbcc65294907a7761611/src
// SDK/OffsetProvider.hpp: MinecraftGame +0x970, ClientInstance sender +0x1D0.
// Hook/Hooks/NetworkHooks/PacketSendHook.cpp: sender vtable[2], Text packet ID 9.
// SDK/Minecraft/Network/Packets/TextPacket.hpp: AuthorAndMessage variant.
// The upstream hints target 1.26.51; this bridge additionally limits versions,
// checks both candidate map layouts, and withholds unverified/ambiguous targets.
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
        error = "Chat SDK bridge is only supported in Minecraft.Windows.exe 1.26.52.*.";
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
    if (major != 1 || minor != 26 || build != 52) {
        error = "Chat SDK bridge does not support Minecraft " + version + "; expected 1.26.52.*.";
        return std::nullopt;
    }
    return version;
}

} // namespace

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
    std::uintptr_t game = 0, head = 0, count = 0, node = 0;
    std::uint8_t head_sentinel = 0, node_sentinel = 1;
    if (!read_at(game_global_, 0, game) || !game || !read_at(game, 0x970, head) ||
        !read_at(game, 0x978, count) || !head || !count || count > 256 ||
        !read_at(head, 0x19, head_sentinel) || head_sentinel != 1 ||
        !read_at(head, 0, node) || !node || node == head ||
        !read_at(node, 0x19, node_sentinel) || node_sentinel != 0) {
        diagnostic = "Waiting for a valid Minecraft primary client instance.";
        return State::waiting;
    }
    std::uintptr_t verified_target = 0;
    std::uintptr_t verified_client = 0;
    std::size_t verified_offset = 0;
    unsigned valid_candidates = 0;
    for (const std::size_t offset : {std::size_t{0x28}, std::size_t{0x30}}) {
        std::uintptr_t instance = 0, sender = 0;
        std::array<std::uintptr_t, 5> instance_methods{}, sender_methods{};
        if (read_at(node, offset, instance) && object_vtable(instance, instance_methods) &&
            read_at(instance, 0x1D0, sender) && object_vtable(sender, sender_methods)) {
            ++valid_candidates;
            verified_target = sender_methods[2];
            verified_client = instance;
            verified_offset = offset;
        }
    }
    if (valid_candidates > 1)
        return unsupported("Primary-client map layouts +0x28 and +0x30 are ambiguous; chat hook withheld.");
    if (!valid_candidates) {
        diagnostic = "Waiting for a validated client/packet-sender vtable for Minecraft " + version_ + ".";
        return State::waiting;
    }
    send_target_ = verified_target;
    verified_client_ = verified_client;
    state_ = State::ready;
    terminal_diagnostic_ = "Chat SDK bridge ready for Minecraft " + version_ +
        (verified_offset == 0x28 ? " (validated client node +0x28)." : " (validated client node +0x30).");
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
        const std::size_t slot = action == ClientMouseAction::grab ? 310 : 311;
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
    std::uintptr_t game = 0, head = 0, count = 0, node = 0;
    std::uint8_t sentinel = 0;
    if (!read_at(game_global_, 0, game) || !game || !read_at(game, 0x970, head) || !head ||
        !read_at(game, 0x978, count) || !count || count > 256 ||
        !read_at(head, 0x19, sentinel) || sentinel != 1 || !read_at(head, 0, node) ||
        !node || node == head || !read_at(node, 0x19, sentinel) || sentinel != 0) return 0;
    std::uintptr_t selected = 0;
    unsigned matches = 0;
    for (const std::size_t offset : {std::size_t{0x28}, std::size_t{0x30}}) {
        std::uintptr_t candidate = 0, sender = 0, associated_game = 0;
        std::array<std::uintptr_t, 5> client_methods{}, sender_methods{};
        if (!read_at(node, offset, candidate) || !object_vtable(candidate, client_methods) ||
            !read_at(candidate, 0x1A8, associated_game) || associated_game != game) continue;
        if (require_sender) {
            if (!read_at(candidate, 0x1D0, sender) || !object_vtable(sender, sender_methods) ||
                sender_methods[2] != send_target_) continue;
        } else {
            std::uintptr_t table = 0, grab = 0, release = 0;
            if (!read_at(candidate, 0, table) || !image_.contains(table, 312 * sizeof(std::uintptr_t)) ||
                !read_at(table, 310 * sizeof(std::uintptr_t), grab) || !executable(grab) ||
                !read_at(table, 311 * sizeof(std::uintptr_t), release) || !executable(release)) continue;
        }
        selected = candidate;
        ++matches;
    }
    return matches == 1 ? selected : 0;
}

std::uintptr_t BedrockChat::local_player() const noexcept {
    try {
        const auto client = current_client();
        std::uintptr_t table = 0, getter = 0;
        if (!client || !read_at(client, 0, table) || !image_.contains(table, 32 * sizeof(std::uintptr_t)) ||
            !read_at(table, 31 * sizeof(std::uintptr_t), getter) || !executable(getter)) return 0;
        // Actual 1.26.52.3 getter31 RVA 0x5DAED30 uses the client's internal
        // weak-handle +0x248 and returns a raw pointer in RAX. All temporary
        // handle ownership remains inside the native function.
        using Getter = void*(__fastcall*)(void*);
        return reinterpret_cast<std::uintptr_t>(reinterpret_cast<Getter>(getter)(reinterpret_cast<void*>(client)));
    } catch (...) { return 0; }
}

std::uintptr_t BedrockChat::tick_target(std::string& diagnostic) const noexcept {
    diagnostic.clear();
    try {
        if (!display_target_ || !executable(display_target_)) {
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
            !read_at(client, 0x650, gui) || !gui || !copy_bytes(gui, gui_probe.data(), gui_probe.size())) {
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
        // Verified offline in 1.26.52.3 at display RVA 0x155EEA0: the message
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
            !read_at(client, 0x1A8, associated_game) || associated_game != game ||
            !read_at(game, 0, table) || !image_.contains(table, 137 * sizeof(std::uintptr_t)) ||
            !read_at(table, 135 * sizeof(std::uintptr_t), grab) || !executable(grab) ||
            !read_at(table, 136 * sizeof(std::uintptr_t), release) || !executable(release)) {
            set_diagnostic(diagnostic, "The current Minecraft mouse context is unavailable.");
            return 0;
        }
        // Proven in the actual 1.26.52.3 image: game grab135 (RVA 0x821230)
        // tests capture byte +0x1E8 then writes 1 after it can acquire. Release136
        // (RVA 0x821700) tests ==1 and writes 0 before the native platform call.
        // Check those field accesses before reading the byte, rather than trust
        // the upstream's formerly uncertain offset or manually writing a flag.
        std::array<std::byte, 64> grab_code{}, release_code{};
        constexpr std::array<std::byte, 7> grab_check{
            std::byte{0x80}, std::byte{0xB9}, std::byte{0xE8}, std::byte{0x01},
            std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
        constexpr std::array<std::byte, 7> release_check{
            std::byte{0x80}, std::byte{0xB9}, std::byte{0xE8}, std::byte{0x01},
            std::byte{0x00}, std::byte{0x00}, std::byte{0x01}};
        constexpr std::array<std::byte, 7> release_write{
            std::byte{0xC6}, std::byte{0x81}, std::byte{0xE8}, std::byte{0x01},
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
    if (slot != 310 && slot != 311) return 0;
    try {
        if (!mouse_game(diagnostic)) return 0;
        const auto client = primary_client(false);
        std::uintptr_t table = 0, target = 0, primary_check = 0;
        if (!client || !read_at(client, 0, table) || !image_.contains(table, (slot + 1) * sizeof(std::uintptr_t)) ||
            !read_at(table, slot * sizeof(std::uintptr_t), target) || !executable(target) ||
            !read_at(table, 162 * sizeof(std::uintptr_t), primary_check) || !executable(primary_check)) {
            set_diagnostic(diagnostic, "Minecraft's native mouse method is unavailable.");
            return 0;
        }
        // The native wrappers themselves perform the primary-client check
        // (vtable162) and dispatch to MinecraftGame. No guessed state is written.
        return target;
    } catch (...) {
        set_diagnostic(diagnostic, "Could not resolve Minecraft's native mouse method.");
        return 0;
    }
}

std::uintptr_t BedrockChat::grab_mouse_target(std::string& diagnostic) const noexcept {
    return mouse_target(310, diagnostic);
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
            read_at(cached_mouse_game_, 0x1E8, state) && state <= 1) return state == 1;
        cached_mouse_game_ = cached_mouse_table_ = 0;
    }
    const auto game = mouse_game(diagnostic);
    std::uint8_t state = 0;
    if (!game || !read_at(game, 0x1E8, state) || state > 1) {
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
        const auto slot = action == ClientMouseAction::grab ? std::size_t{310} : std::size_t{311};
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
