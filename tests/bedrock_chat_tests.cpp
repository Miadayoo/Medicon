#include <medicon/sdk/BedrockChat.hpp>

#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using medicon::sdk::read_msvc_string;
using medicon::sdk::read_text_message;
using medicon::sdk::consume_outgoing_chat;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class T, std::size_t N>
void store(std::array<std::byte, N>& object, std::size_t offset, T value) {
    require(offset <= N && sizeof(value) <= N - offset, "invalid test fixture offset");
    std::memcpy(object.data() + offset, &value, sizeof(value));
}

std::array<std::byte, 32> inline_string(std::string_view value) {
    require(value.size() <= 15, "inline test fixture too large");
    std::array<std::byte, 32> result{};
    std::memcpy(result.data(), value.data(), value.size());
    store(result, 16, static_cast<std::uint64_t>(value.size()));
    store(result, 24, std::uint64_t{15});
    return result;
}

void strings() {
    auto raw = inline_string(".bind fly r");
    require(read_msvc_string(raw.data()) == ".bind fly r", "SSO command was not copied");
    require(read_msvc_string(inline_string("").data()) == "", "empty SSO string rejected");
    require(read_msvc_string(inline_string("123456789012345").data()) == "123456789012345",
            "15-byte SSO boundary rejected");
    require(!read_msvc_string(raw.data(), 5), "caller length bound ignored");
    store(raw, 16, std::uint64_t{16});
    require(!read_msvc_string(raw.data()), "SSO length over capacity accepted");
    raw = inline_string("x");
    store(raw, 24, std::uint64_t{14});
    require(!read_msvc_string(raw.data()), "invalid SSO capacity accepted");
    raw = inline_string("abc");
    raw[3] = std::byte{'!'};
    require(!read_msvc_string(raw.data()), "unterminated SSO accepted");

    std::string text = ".bind clickgui RightShift";
    raw = {};
    store(raw, 0, reinterpret_cast<std::uintptr_t>(text.c_str()));
    store(raw, 16, static_cast<std::uint64_t>(text.size()));
    store(raw, 24, std::uint64_t{63});
    require(read_msvc_string(raw.data()) == text, "heap string was not copied");
    const auto own_copy = read_msvc_string(raw.data());
    text[0] = '!';
    require(own_copy && own_copy->front() == '.', "returned string aliases foreign storage");
    store(raw, 24, std::uint64_t{16});
    require(!read_msvc_string(raw.data()), "heap length over capacity accepted");
    store(raw, 24, std::uint64_t{63});
    text.push_back('!');
    store(raw, 0, reinterpret_cast<std::uintptr_t>(text.c_str()));
    require(!read_msvc_string(raw.data()), "unterminated foreign heap string accepted");

    std::string boundary(1024, 'x');
    store(raw, 0, reinterpret_cast<std::uintptr_t>(boundary.c_str()));
    store(raw, 16, std::uint64_t{1024});
    store(raw, 24, std::uint64_t{1024});
    require(read_msvc_string(raw.data()) == boundary, "1024-byte message boundary rejected");
    store(raw, 24, std::uint64_t{1025});
    require(!read_msvc_string(raw.data()), "excessive foreign capacity accepted");
    store(raw, 24, std::uint64_t{1024});
    store(raw, 16, std::uint64_t{1025});
    require(!read_msvc_string(raw.data(), 2048), "hard message bound ignored");
    store(raw, 16, std::uint64_t{20});
    store(raw, 0, std::uintptr_t{1});
    require(!read_msvc_string(raw.data()), "unreadable heap pointer accepted");
    store(raw, 0, std::numeric_limits<std::uintptr_t>::max() - 3);
    require(!read_msvc_string(raw.data()), "overflowing heap range accepted");
    require(!read_msvc_string(nullptr) && !read_msvc_string(reinterpret_cast<const void*>(1)),
            "unreadable foreign string header accepted");
}

void packets() {
    std::array<std::byte, 0xF0> packet{};
    auto text = inline_string(".bind fly f");
    std::memcpy(packet.data() + 0xC8, text.data(), text.size());
    packet[0xA0] = std::byte{1};
    packet[0xE8] = std::byte{1};
    require(read_text_message(packet.data()) == ".bind fly f", "outgoing chat variant rejected");
    packet[0xE8] = std::byte{0};
    require(!read_text_message(packet.data()), "MessageOnly variant accepted as outgoing chat");
    packet[0xE8] = std::byte{2};
    require(!read_text_message(packet.data()), "MessageAndParams variant accepted as outgoing chat");
    packet[0xE8] = std::byte{0xFF};
    require(!read_text_message(packet.data()), "valueless variant accepted");
    packet[0xE8] = std::byte{1};
    packet[0xA0] = std::byte{7};
    require(!read_text_message(packet.data()), "non-chat packet type accepted");
    require(!read_text_message(nullptr) && !read_text_message(reinterpret_cast<const void*>(1)),
            "unreadable packet accepted");
    require(!read_text_message(reinterpret_cast<const void*>(std::numeric_limits<std::uintptr_t>::max() - 5)),
            "overflowing packet offsets accepted");

    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    auto* page = static_cast<std::byte*>(VirtualAlloc(nullptr, info.dwPageSize,
                                                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    require(page != nullptr, "VirtualAlloc failed");
    std::memcpy(page, packet.data(), packet.size());
    DWORD previous = 0;
    require(VirtualProtect(page, info.dwPageSize, PAGE_NOACCESS, &previous) != FALSE,
            "VirtualProtect failed");
    const bool rejected = !read_text_message(page) && !read_msvc_string(page);
    VirtualFree(page, 0, MEM_RELEASE);
    require(rejected, "inaccessible page was read");

    auto* adjacent = static_cast<std::byte*>(VirtualAlloc(nullptr, 2 * info.dwPageSize,
                                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    require(adjacent != nullptr, "boundary VirtualAlloc failed");
    require(VirtualProtect(adjacent + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &previous) != FALSE,
            "boundary VirtualProtect failed");
    const bool truncated_rejected = !read_msvc_string(adjacent + info.dwPageSize - 16);
    VirtualFree(adjacent, 0, MEM_RELEASE);
    require(truncated_rejected, "foreign header crossing inaccessible page accepted");
}

int packet_id_calls = 0;
int __fastcall fixture_packet_id(void* packet) {
    ++packet_id_calls;
    int id = 0;
    std::memcpy(&id, static_cast<std::byte*>(packet) + sizeof(std::uintptr_t), sizeof(id));
    return id;
}

void outgoing_interception() {
    // This runs the exact interception path called by BedrockChat::consume.
    // The only substitute is code ownership validation: fixtures belong to this
    // isolated host executable, while production requires the Minecraft image.
    std::array<std::uintptr_t, 2> vtable{0, reinterpret_cast<std::uintptr_t>(&fixture_packet_id)};
    std::array<std::byte, 0xF0> packet{};
    store(packet, 0, reinterpret_cast<std::uintptr_t>(vtable.data()));
    store(packet, sizeof(std::uintptr_t), int{9});
    packet[0xA0] = std::byte{1};
    packet[0xE8] = std::byte{1};
    const auto validate = [&](std::uintptr_t table, std::uintptr_t getter) {
        return table == reinterpret_cast<std::uintptr_t>(vtable.data()) && getter == vtable[1];
    };
    const auto set_message = [&](std::string_view message) {
        const auto raw = inline_string(message);
        std::memcpy(packet.data() + 0xC8, raw.data(), raw.size());
    };
    std::string queued;
    unsigned enqueues = 0;
    const auto enqueue = [&](std::string message) { queued = std::move(message); ++enqueues; };
    set_message("normal chat");
    const auto normal_before = packet;
    require(!consume_outgoing_chat(packet.data(), validate, enqueue) && enqueues == 0 && packet == normal_before,
            "ordinary native chat did not pass through unchanged");
    set_message(".bind fly r");
    const auto command_before = packet;
    require(consume_outgoing_chat(packet.data(), validate, enqueue) && enqueues == 1 && queued == ".bind fly r" &&
            packet == command_before, "dot command was not copied and consumed without packet mutation");
    set_message("changed");
    require(queued == ".bind fly r", "queued command aliases game packet storage");
    set_message(".unknown");
    require(consume_outgoing_chat(packet.data(), validate, enqueue) && enqueues == 2 && queued == ".unknown",
            "unknown local command escaped to the server");
    require(consume_outgoing_chat(packet.data(), validate, [](std::string) { throw std::bad_alloc(); }),
            "enqueue failure allowed a recognized command to reach the server");
    require(consume_outgoing_chat(packet.data(), validate, {}), "missing enqueue allowed a command to reach the server");
    store(packet, sizeof(std::uintptr_t), int{1});
    require(!consume_outgoing_chat(packet.data(), validate, enqueue) && enqueues == 2,
            "non-Text packet was consumed");
    store(packet, sizeof(std::uintptr_t), int{9});
    packet[0xE8] = std::byte{2};
    require(!consume_outgoing_chat(packet.data(), validate, enqueue), "malformed outgoing variant was consumed");
    packet[0xE8] = std::byte{1};
    const auto calls = packet_id_calls;
    require(!consume_outgoing_chat(packet.data(), [](auto, auto) { return false; }, enqueue) &&
            packet_id_calls == calls, "untrusted game function was called");
    require(!consume_outgoing_chat(reinterpret_cast<void*>(1), validate, enqueue), "unreadable packet was consumed");
}

void borrowed_feedback_strings() {
    // The native display entry reads these three ABI fields, then copies the
    // bytes into strings allocated by Minecraft. The envelope itself owns none.
    for (const std::size_t length : {std::size_t{0}, std::size_t{15}, std::size_t{16},
                                    std::size_t{22}, std::size_t{23}, std::size_t{256}}) {
        std::string backing(length, 'x');
        alignas(8) const auto header = medicon::sdk::borrowed_msvc_string(backing);
        std::uint64_t size = 0, capacity = 0;
        std::memcpy(&size, header.data() + 16, sizeof(size));
        std::memcpy(&capacity, header.data() + 24, sizeof(capacity));
        require(size == length, "native feedback header length is wrong");
        if (length <= 15) {
            require(capacity == 15 && std::memcmp(header.data(), backing.data(), length) == 0 &&
                    header[length] == std::byte{}, "native feedback SSO envelope is malformed");
        } else {
            std::uintptr_t data = 0;
            std::memcpy(&data, header.data(), sizeof(data));
            require(capacity == length && data == reinterpret_cast<std::uintptr_t>(backing.c_str()),
                    "native feedback heap envelope does not borrow caller-owned NUL-backed storage");
            backing[0] = 'y';
        }
        require(read_msvc_string(header.data()) == backing, "native feedback envelope cannot be read by MSVC layout");
    }
}

struct MouseFixture {
    std::uintptr_t table = 0;
    bool grabbed = false;
    unsigned grabs = 0;
    unsigned releases = 0;
};

void __fastcall fixture_grab_mouse(void* object) {
    auto& client = *static_cast<MouseFixture*>(object);
    client.grabbed = true;
    ++client.grabs;
}

void __fastcall fixture_release_mouse(void* object) {
    auto& client = *static_cast<MouseFixture*>(object);
    client.grabbed = false;
    ++client.releases;
}

void native_mouse_routing() {
    using medicon::sdk::ClientMouseAction;
    using medicon::sdk::invoke_client_mouse;
    std::array<std::uintptr_t, 312> table{};
    table[310] = reinterpret_cast<std::uintptr_t>(&fixture_grab_mouse);
    table[311] = reinterpret_cast<std::uintptr_t>(&fixture_release_mouse);
    MouseFixture client{reinterpret_cast<std::uintptr_t>(table.data())};
    const auto validate = [&](auto candidate_table, auto function) {
        return candidate_table == client.table && (function == table[310] || function == table[311]);
    };
    require(invoke_client_mouse(&client, ClientMouseAction::grab, validate) &&
            client.grabbed && client.grabs == 1 && client.releases == 0,
            "native grab did not route to ClientInstance vtable310 with its original this pointer");
    require(invoke_client_mouse(&client, ClientMouseAction::release, validate) &&
            !client.grabbed && client.grabs == 1 && client.releases == 1,
            "native release did not route to ClientInstance vtable311");
    require(!invoke_client_mouse(&client, ClientMouseAction::grab, [](auto, auto) { return false; }) &&
            client.grabs == 1, "untrusted native mouse function was called");
    require(!invoke_client_mouse(&client, ClientMouseAction::grab,
            [](auto, auto) -> bool { throw std::runtime_error("validation failed"); }) && client.grabs == 1,
            "mouse validator exception crossed the native boundary");
    require(!invoke_client_mouse(&client, static_cast<ClientMouseAction>(99), validate) &&
            !invoke_client_mouse(&client, ClientMouseAction::release, {}) &&
            client.grabs == 1 && client.releases == 1, "invalid mouse route called game code");
    require(!invoke_client_mouse(nullptr, ClientMouseAction::grab, validate) &&
            !invoke_client_mouse(reinterpret_cast<void*>(1), ClientMouseAction::release, validate),
            "unreadable native mouse client was accepted");
    client.table = 1;
    require(!invoke_client_mouse(&client, ClientMouseAction::grab, validate), "unreadable mouse vtable was accepted");
}

void unsupported_host() {
    medicon::sdk::BedrockChat bridge;
    std::string diagnostic;
    require(bridge.locate(diagnostic) == medicon::sdk::BedrockChat::State::unsupported &&
            !diagnostic.empty() && bridge.send_target() == 0, "non-game process was not rejected");
    bool enqueued = false;
    require(!bridge.consume(reinterpret_cast<void*>(1), [&](std::string) { enqueued = true; }) && !enqueued,
            "unsupported bridge dispatched a command");
    require(bridge.tick_target(diagnostic) == 0 && !diagnostic.empty() && !bridge.is_local_player(nullptr) &&
            !bridge.is_local_player(reinterpret_cast<void*>(1)), "unsupported bridge resolved a native feedback tick");
    require(!bridge.display_message(reinterpret_cast<void*>(1), "feedback", diagnostic) && !diagnostic.empty(),
            "unsupported bridge called native feedback");
    require(bridge.grab_mouse_target(diagnostic) == 0 && !diagnostic.empty() &&
            bridge.mouse_client() == nullptr && !bridge.is_current_client(nullptr) &&
            !bridge.is_current_client(reinterpret_cast<void*>(1)),
            "unsupported host resolved a native mouse client");
    require(!bridge.mouse_grabbed(diagnostic).has_value() && !diagnostic.empty(),
            "unsupported host reported a native mouse capture state");
    require(!bridge.release_mouse(diagnostic) && !diagnostic.empty() &&
            !bridge.grab_mouse(diagnostic) && !diagnostic.empty(),
            "unsupported host invoked a native mouse transition");
}
} // namespace

int main() {
    try {
        strings();
        packets();
        outgoing_interception();
        borrowed_feedback_strings();
        native_mouse_routing();
        unsupported_host();
        std::cout << "Bedrock chat foreign-layout decoding passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Bedrock chat: " << error.what() << '\n';
        return 1;
    }
}
