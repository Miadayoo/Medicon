#include <medicon/sdk/BedrockMenu.hpp>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <future>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace medicon::sdk {
// Only this executable defines the friend. Production's version gate remains
// unchanged; fixtures exercise poll() without invoking any foreign function.
struct BedrockMenuTestAccess {
    static void use_image(BedrockMenu& reader, memory::AddressRange image) {
        reader.initialized_ = reader.supported_ = true;
        reader.image_ = image;
        reader.executable_regions_ = {image};
        reader.static_code_ok_ = false;
        reader.static_code_client_ = 0;
        reader.static_code_tick_ = 0;
    }
    static void invalidate_profile(BedrockMenu& reader) {
        reader.static_code_ok_ = false;
        reader.static_code_client_ = 0;
        reader.static_code_tick_ = 0;
    }
};
}

namespace {
using medicon::sdk::BedrockMenu;
using medicon::sdk::MenuScreenState;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class T, std::size_t N>
void store(std::array<std::byte, N>& object, std::size_t offset, T value) {
    require(offset <= N && sizeof(value) <= N - offset, "Invalid test fixture offset.");
    std::memcpy(object.data() + offset, &value, sizeof(value));
}
template<class T, std::size_t N>
std::uintptr_t address(std::array<T, N>& object) {
    return reinterpret_cast<std::uintptr_t>(object.data());
}

// Existing inspected RVAs and bytes only. These fixtures prove traversal and
// fail-closed behavior; they cannot prove Minecraft's actual object layout.
constexpr std::uintptr_t client_name = 0x65B4850, current_stack = 0x65B4340;
constexpr std::uintptr_t primary_test = 0x65B0040, scene_factory = 0x8BC9E0;
constexpr std::uintptr_t main_stack = 0x65B4230, game_stack = 0x893930;
constexpr std::uintptr_t stack_name = 0x56D000, scene_name = 0x4CD2860;
constexpr std::uintptr_t empty_suffix = 0x17DEE0;
constexpr std::uintptr_t stack_count = 0x56C570, observed_start_suffix = 0x586570;
constexpr std::uintptr_t empty_suffix_literal = 0xF07B2A0;

struct MenuFixture {
    static constexpr std::size_t image_size = 0x1383B000;
    std::byte* image = static_cast<std::byte*>(VirtualAlloc(nullptr, image_size, MEM_RESERVE, PAGE_NOACCESS));
    std::array<std::byte, 0x700> client{};
    std::array<std::byte, 0x1F00> game{};
    std::array<std::byte, 0x80> factory{}, stack{}, scene{}, view{}, definition{}, controller{};
    std::array<std::byte, 0x20> definition_owner{};
    std::array<char,96> screen_storage{};
    // Observed live entry stride is 32 bytes. The final scene pointer lives at
    // end - 24, which is eight bytes into the last entry, not at its beginning.
    static constexpr std::size_t top_entry_offset = 4 * 32 - 24;
    std::array<std::byte, 4 * 32> entries{};
    std::array<std::byte, 8> factory_control{}, stack_control{};
    BedrockMenu reader;

    MenuFixture() {
        require(image != nullptr, "Cannot reserve the synthetic image.");
        try {
            observed_code();
            table(client, 0x10000, {{243, client_name}, {237, current_stack}, {160, primary_test}, {227, scene_factory}, {235, main_stack}});
            table(game, 0x11000, {{121, game_stack}});
            table(stack, 0x12000, {{30, stack_count}, {44, stack_name}});
            table(scene, 0x13000, {{55, scene_name}});
            table(controller, 0x14000, {{27, observed_start_suffix}});
            store(client, 0x228, address(factory));
            store(client, 0x1C8, address(game));
            store(factory, 8, address(factory_control));
            store(factory_control, 0, std::uint8_t{1});
            store(factory, 0x18, address(client));
            store(game, 0x1DF8, address(stack));
            store(stack, 8, address(stack_control));
            store(stack_control, 0, std::uint8_t{1});
            store(stack, 0x18, address(entries));
            store(stack, 0x20, address(entries) + entries.size());
            store(entries, 8, address(scene));
            store(entries, top_entry_offset, address(scene));
            store(scene, 0x40, address(view));
            store(view, 0x50, address(definition_owner));
            store(definition_owner, 8, address(definition));
            store(view, 0x40, address(controller));
            screen("start_screen");
            medicon::sdk::BedrockMenuTestAccess::use_image(reader, {reinterpret_cast<std::uintptr_t>(image), image_size});
        } catch (...) { VirtualFree(image, 0, MEM_RELEASE); throw; }
    }
    ~MenuFixture() { VirtualFree(image, 0, MEM_RELEASE); }
    MenuFixture(const MenuFixture&) = delete;
    MenuFixture& operator=(const MenuFixture&) = delete;

    void put(std::uintptr_t rva, const void* data, std::size_t size) {
        require(rva < image_size && size <= image_size - rva, "Fixture image write overflow.");
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const auto page = static_cast<std::uintptr_t>(system.dwPageSize);
        const auto first = rva / page * page, last = (rva + size + page - 1) / page * page;
        require(VirtualAlloc(image + first, last - first, MEM_COMMIT, PAGE_READWRITE) != nullptr, "Cannot commit a synthetic image page.");
        std::memcpy(image + rva, data, size);
    }
    void bytes(std::uintptr_t rva, std::initializer_list<std::uint8_t> value) { put(rva, value.begin(), value.size()); }
    void call(std::uintptr_t at, std::uintptr_t target) {
        const auto displacement = static_cast<std::int32_t>(
            static_cast<std::int64_t>(target) - static_cast<std::int64_t>(at) - 5);
        bytes(at, {0xE8});
        put(at + 1, &displacement, sizeof(displacement));
    }
    template<std::size_t N>
    void table(std::array<std::byte, N>& object, std::uintptr_t rva,
               std::initializer_list<std::pair<std::size_t, std::uintptr_t>> methods) {
        store(object, 0, reinterpret_cast<std::uintptr_t>(image) + rva);
        for (const auto& [slot, method] : methods) {
            const auto function = reinterpret_cast<std::uintptr_t>(image) + method;
            put(rva + slot * sizeof(std::uintptr_t), &function, sizeof(function));
        }
    }
    void screen(std::string_view name) {
        require(name.size() < screen_storage.size(), "Screen fixture exceeds bounded string storage.");
        std::fill(definition.begin() + 0x20, definition.begin() + 0x40, std::byte{});
        if (name.size() <= 15) std::memcpy(definition.data() + 0x20, name.data(), name.size());
        else {
            std::fill(screen_storage.begin(),screen_storage.end(),0);
            std::memcpy(screen_storage.data(),name.data(),name.size());
            store(definition,0x20,reinterpret_cast<std::uintptr_t>(screen_storage.data()));
        }
        store(definition, 0x30, static_cast<std::uint64_t>(name.size()));
        store(definition, 0x38, name.size()<=15?std::uint64_t{15}:std::uint64_t{95});
    }
    void expect(MenuScreenState expected, const char* label, void* selected_client = nullptr) {
        medicon::sdk::BedrockMenuTestAccess::invalidate_profile(reader);
        std::string diagnostic;
        const auto actual = reader.poll(selected_client ? selected_client : client.data(), diagnostic);
        if (actual != expected || (actual == MenuScreenState::unknown && diagnostic.empty())) {
            std::cerr << label << ": state=" << static_cast<int>(actual) << ", diagnostic=" << diagnostic << '\n';
            throw std::runtime_error(label);
        }
    }
    void observed_code() {
        bytes(0x65B4864, {0x48,0x89,0xD6,0x48,0x8B,0x01,0x48,0x8B,0x80,0x68,0x07,0x00,0x00});
        bytes(0x65B4898, {0x48,0x8B,0x4D,0xF0,0x48,0x8B,0x01,0x48,0x8B,0x80,0x60,0x01,0x00,0x00});
        bytes(0x65B0040, {0x80,0xB9,0x90,0x06,0x00,0x00,0x00,0x0F,0x94,0xC0,0xC3});
        bytes(0x8BC9E0, {0x48,0x8B,0x81,0x28,0x02,0x00,0x00,0xC3});
        bytes(0x65B4364, {0x48,0x8B,0x07,0x48,0x8B,0x80,0x18,0x07,0x00,0x00,0x48,0x89,0xF9});
        bytes(0x65B437A, {0x80,0x78,0x40,0x00});
        bytes(0x65B43D8, {0xB8,0x58,0x07,0x00,0x00});
        bytes(0x65B4230, {0x48,0x8B,0x89,0xC8,0x01,0x00,0x00,0x48,0x8B,0x01,0x48,0x8B,0x80,0xC8,0x03,0x00,0x00});
        bytes(0x893937, {0x48,0x8B,0xB9,0xF8,0x1D,0x00,0x00});
        bytes(0x56D005, {0x48,0x8B,0x41,0x20,0x48,0x39,0x41,0x18,0x74,0x21,0x48,0x8B,0x48,0xE8,0x48});
        bytes(0x56D018, {0x48,0x8B,0x01,0x48,0x8B,0x80,0xB8,0x01,0x00,0x00});
        bytes(0x56C57E, {0x48,0x8B,0x41,0x18,0x4C,0x8B,0x71,0x20,0x4C,0x39,0xF0});
        bytes(0x56C594, {0x4C,0x89,0xF0,0x48,0xC1,0xF8,0x05});
        bytes(0x56C5CF, {0x49,0x83,0xC6,0xE0});
        bytes(0x4CD2860, {0x48,0x8B,0x49,0x40,0xE9,0xC7,0x4C,0xFF,0xFF});
        bytes(0x4CC7554, {0x48,0x8B,0x41,0x50,0x48,0x85,0xC0});
        bytes(0x4CC7577, {0x4C,0x8B,0x78,0x08});
        bytes(0x4CC7580, {0x49,0x83,0x7F,0x38,0x10});
        bytes(0x4CC7594, {0x49,0x8D,0x7F,0x20});
        bytes(0x4CC75DC, {0x48,0x8B,0x49,0x40,0x48,0x8B,0x01,0x48,0x8B,0x80,0xD8,0x00,0x00,0x00});
        bytes(empty_suffix, {0x48,0x89,0xD0,0x0F,0x57,0xC0,0x0F,0x11,0x02,0x48,0xC7,0x42,0x10,0,0,0,0,0x48,0xC7,0x42,0x18,0x0F,0,0,0,0xC3});
        bytes(observed_start_suffix, {0x48,0x89,0xD0,0x48,0xC7,0x42,0x10,0x00,0x00,0x00,0x00,0x48,0xC7,0x42,0x18,0x0F,0x00,0x00,0x00,0x0F,0x10,0x05,0x16,0x4D,0xAF,0x0E,0x0F,0x11,0x02,0xC3});
        const std::array<std::uint8_t, 16> zeros{};
        put(empty_suffix_literal, zeros.data(), zeros.size());
    }
};

void unsupported_host() {
    BedrockMenu reader;
    std::string reason;
    require(reader.poll(reinterpret_cast<void*>(std::numeric_limits<std::uintptr_t>::max()), reason) ==
            MenuScreenState::unknown && !reason.empty(), "Unsupported executable was not rejected safely.");
    const auto expected = reason;
    std::array<std::future<bool>, 4> polls;
    for (auto& task : polls) task = std::async(std::launch::async, [&reader, &expected] {
        for (unsigned i = 0; i != 32; ++i) {
            std::string diagnostic;
            if (reader.poll(reinterpret_cast<void*>(std::uintptr_t{1}), diagnostic) != MenuScreenState::unknown || diagnostic != expected) return false;
        }
        return true;
    });
    for (auto& task : polls) require(task.get(), "Concurrent unsupported polls did not remain disabled.");
}

void supported_image() {
    MenuFixture f;
    f.expect(MenuScreenState::main_menu, "Valid start_screen was not recognized.");
    store(f.stack, 0x20, address(f.entries) + 32);
    f.expect(MenuScreenState::main_menu, "A single 32-byte SceneStack entry was not recognized.");
    store(f.stack, 0x20, address(f.entries) + f.entries.size());
    f.table(f.stack, 0x12000, {{30, stack_count + 1}});
    f.expect(MenuScreenState::unknown, "An unverified SceneStack stride implementation was accepted.");
    f.table(f.stack, 0x12000, {{30, stack_count}});
    f.bytes(stack_count + 0x5F, {0x90});
    f.expect(MenuScreenState::unknown, "A changed SceneStack stride was accepted.");
    f.observed_code();
    f.screen("disconnect_screen");
    f.expect(MenuScreenState::disconnected, "Disconnect screen was not recognized.");
    f.screen("realms_disconnect_screen");
    f.expect(MenuScreenState::disconnected, "Realms disconnect screen was not recognized.");
    f.screen("start_screen");
    for (const auto name : {"play_screen", "settings_screen", "hud_screen", "pause_screen"}) {
        f.screen(name);
        f.expect(MenuScreenState::other, "A non-menu screen enabled the replacement menu.");
        f.screen("start_screen");
        f.expect(MenuScreenState::main_menu, "Returning to start_screen failed.");
    }
    store(f.stack, 0x20, address(f.entries));
    f.expect(MenuScreenState::other, "An empty SceneStack enabled the replacement menu.");
    store(f.stack, 0x20, address(f.entries) + f.entries.size());
    store(f.entries, MenuFixture::top_entry_offset, std::uintptr_t{});
    f.expect(MenuScreenState::other, "A null top scene enabled the replacement menu.");
    store(f.entries, MenuFixture::top_entry_offset, address(f.scene));
    store(f.scene, 0, std::uintptr_t{});
    f.expect(MenuScreenState::other, "An unrelated scene implementation was accepted as UIScene.");
    f.table(f.scene, 0x13000, {{55, scene_name}});
    f.expect(MenuScreenState::unknown, "An unreadable client was not rejected.", reinterpret_cast<void*>(1));
    f.expect(MenuScreenState::unknown, "An overflowing client was not rejected.", reinterpret_cast<void*>(std::numeric_limits<std::uintptr_t>::max()));
    std::string diagnostic;
    require(f.reader.poll(nullptr, diagnostic) == MenuScreenState::unknown && !diagnostic.empty(), "A null client was not rejected.");
    store(f.client, 0x228, std::uintptr_t{});
    f.expect(MenuScreenState::unknown, "A null SceneFactory was not rejected.");
    store(f.client, 0x228, address(f.factory));
    store(f.factory_control, 0, std::uint8_t{});
    f.expect(MenuScreenState::unknown, "An invalidated SceneFactory was not rejected.");
    store(f.factory_control, 0, std::uint8_t{1});
    store(f.stack_control, 0, std::uint8_t{});
    f.expect(MenuScreenState::unknown, "An invalidated stack was not rejected.");
    store(f.stack_control, 0, std::uint8_t{1});
    store(f.factory, 0x18, address(f.game));
    f.expect(MenuScreenState::unknown, "A factory belonging to another client was accepted.");
    store(f.factory, 0x18, address(f.client));
    for (const auto end : {address(f.entries) - 32, address(f.entries) + 1, address(f.entries) + 32 * 257}) {
        store(f.stack, 0x20, end);
        f.expect(MenuScreenState::unknown, "An invalid SceneStack range was accepted.");
    }
    store(f.stack, 0x20, address(f.entries) + f.entries.size());
    store(f.view, 0x50, reinterpret_cast<std::uintptr_t>(f.image) + 0x50000);
    f.expect(MenuScreenState::unknown, "An inaccessible screen definition owner was accepted.");
    store(f.view, 0x50, address(f.definition_owner));
    store(f.definition_owner, 8, std::numeric_limits<std::uintptr_t>::max() - 8);
    f.expect(MenuScreenState::unknown, "An overflowing screen definition was accepted.");
    store(f.definition_owner, 8, address(f.definition));
    store(f.definition, 0x30, std::uint64_t{97});
    f.expect(MenuScreenState::unknown, "An invalid screen string was accepted.");
    f.screen("start_screen");
    store(f.view, 0x40, std::uintptr_t{});
    f.expect(MenuScreenState::unknown, "A null start-screen controller was accepted.");
    store(f.view, 0x40, address(f.controller));
    f.table(f.controller, 0x14000, {{27, observed_start_suffix + 1}});
    f.expect(MenuScreenState::unknown, "An unverified controller suffix method was accepted.");
    f.screen("settings_screen");
    f.expect(MenuScreenState::other, "A non-menu screen unnecessarily required the start controller.");
    f.screen("start_screen");
    f.table(f.controller, 0x14000, {{27, empty_suffix}});
    f.expect(MenuScreenState::main_menu, "The other proven empty suffix implementation was rejected.");
    f.table(f.controller, 0x14000, {{27, observed_start_suffix}});
    f.bytes(empty_suffix_literal, {0x01});
    f.expect(MenuScreenState::unknown, "A nonempty suffix literal was accepted.");
    f.observed_code();
    f.bytes(observed_start_suffix, {0x90});
    f.expect(MenuScreenState::unknown, "A changed observed controller suffix was accepted.");
    f.observed_code();
    f.table(f.controller, 0x14000, {{27, empty_suffix}});
    f.bytes(empty_suffix, {0x90});
    f.expect(MenuScreenState::unknown, "A controller with unverified suffix code was accepted.");
    f.observed_code();
    f.table(f.controller, 0x14000, {{27, observed_start_suffix}});
    f.table(f.client, 0x10000, {{243, client_name + 1}});
    f.expect(MenuScreenState::unknown, "A mismatched client method was accepted.");
    f.table(f.client, 0x10000, {{243, client_name}});
    f.bytes(scene_factory, {0x90});
    f.expect(MenuScreenState::unknown, "A changed instruction profile was accepted.");
    f.observed_code();
    f.expect(MenuScreenState::main_menu, "A transient failure permanently disabled the supported reader.");
}

void session_identity() {
    MenuFixture f;
    std::string diagnostic;
    medicon::sdk::MenuSessionIdentity initial, observed;
    const auto inspect = [&](medicon::sdk::MenuSessionIdentity& session) {
        return f.reader.poll(f.client.data(), diagnostic, &session);
    };
    require(inspect(initial) == MenuScreenState::main_menu && initial.client == address(f.client) &&
            initial.scene == address(f.scene) && initial.controller == address(f.controller),
            "A verified menu did not provide its owning session identity.");
    require(inspect(observed) == MenuScreenState::main_menu && observed == initial,
            "An unchanged menu changed its session identity.");

    // Replacing a controller or scene can leave the raw start_screen name
    // unchanged. A queued click must not acquire the new owner's identity.
    auto replacement_controller = f.controller;
    store(f.view, 0x40, address(replacement_controller));
    require(inspect(observed) == MenuScreenState::main_menu && observed != initial &&
            observed.controller == address(replacement_controller),
            "Replacing start_screen's controller was invisible to session tracking.");
    store(f.view, 0x40, address(f.controller));
    auto replacement_scene = f.scene;
    store(f.entries, MenuFixture::top_entry_offset, address(replacement_scene));
    require(inspect(observed) == MenuScreenState::main_menu && observed != initial &&
            observed.scene == address(replacement_scene),
            "Replacing start_screen's scene was invisible to session tracking.");
    store(f.entries, MenuFixture::top_entry_offset, address(f.scene));
    require(inspect(observed) == MenuScreenState::main_menu && observed == initial,
            "Restoring the original session did not restore its identity.");

    f.screen("settings_screen");
    require(inspect(observed) == MenuScreenState::other && observed == medicon::sdk::MenuSessionIdentity{},
            "A non-menu observation retained the previous menu identity.");
    f.screen("start_screen");
    require(inspect(observed) == MenuScreenState::main_menu, "Main Menu reentry failed.");
    store(f.stack_control, 0, std::uint8_t{});
    require(inspect(observed) == MenuScreenState::unknown && observed == medicon::sdk::MenuSessionIdentity{},
            "An invalidated menu retained callable session identity.");
}
}

int main() {
    try {
        unsupported_host();
        supported_image();
        session_identity();
        std::cout << "Unsupported-host, concurrent fail-closed, and supported-image SceneStack fixture checks passed.\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
