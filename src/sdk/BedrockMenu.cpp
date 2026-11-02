#include <medicon/sdk/BedrockMenu.hpp>

#include <medicon/memory/module_view.hpp>
#include <medicon/sdk/BedrockChat.hpp>
#include <medicon/sdk/BedrockProfile.hpp>

#include <Windows.h>
#include <winver.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string_view>

namespace medicon::sdk {
namespace {

// IDA MCP: 1.26.60.5 ClientInstance -> SceneStack -> UIScene -> ScreenView.
// The selector is now inlined in current_stack. See docs/bedrock-1.26.60.md.
// RVAs below are identity checks, never addresses invoked by this reader.
constexpr std::uintptr_t client_name_rva = 0x65B4850;
constexpr std::uintptr_t current_stack_rva = 0x65B4340;
constexpr std::uintptr_t primary_test_rva = 0x65B0040;
constexpr std::uintptr_t scene_factory_rva = 0x8BC9E0;
constexpr std::uintptr_t main_stack_rva = 0x65B4230;
constexpr std::uintptr_t game_stack_rva = 0x893930;
constexpr std::uintptr_t stack_name_rva = 0x56D000;
constexpr std::uintptr_t stack_visit_rva = 0x56C570;
constexpr std::uintptr_t scene_name_rva = 0x4CD2860;

constexpr std::uintptr_t empty_suffix_rva = 0x17DEE0;
constexpr std::uintptr_t title_empty_suffix_rva = 0x586570;
constexpr std::uintptr_t title_empty_storage_rva = 0xF07B2A0;
// slot30 counts entries with SAR 5 and walks back by 0x20. The native
// name getter's end-0x18 is the scene field at entry+8, not sizeof(entry).
constexpr std::size_t stack_entry_size = 32;

bool copy(std::uintptr_t address, void* output, std::size_t size) noexcept {
    if (!address || !output || !size || size - 1 > std::numeric_limits<std::uintptr_t>::max() - address)
        return false;
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), output,
                             size, &copied) != FALSE && copied == size;
}

template<class T>
bool read(std::uintptr_t address, std::size_t offset, T& value) noexcept {
    return offset <= std::numeric_limits<std::uintptr_t>::max() - address &&
           copy(address + offset, &value, sizeof(value));
}

bool bytes(std::uintptr_t address, std::initializer_list<std::uint8_t> expected) noexcept {
    std::array<std::uint8_t, 96> actual{};
    return expected.size() <= actual.size() && copy(address, actual.data(), expected.size()) &&
           std::equal(expected.begin(), expected.end(), actual.begin());
}

// Image bytes do not change between frames. Re-reading them through
// ReadProcessMemory on every Present hitches the overlay and the game.
bool verify_static_code(std::uintptr_t base) noexcept {
    return bytes(base + 0x65B4864, {0x48,0x89,0xD6,0x48,0x8B,0x01,0x48,0x8B,0x80,0x68,0x07,0x00,0x00}) &&
           bytes(base + 0x65B4898, {0x48,0x8B,0x4D,0xF0,0x48,0x8B,0x01,0x48,0x8B,0x80,0x60,0x01,0x00,0x00}) &&
           bytes(base + 0x65B0040, {0x80,0xB9,0x90,0x06,0x00,0x00,0x00,0x0F,0x94,0xC0,0xC3}) &&
           bytes(base + 0x8BC9E0, {0x48,0x8B,0x81,0x28,0x02,0x00,0x00,0xC3}) &&
           bytes(base + 0x65B4364, {0x48,0x8B,0x07,0x48,0x8B,0x80,0x18,0x07,0x00,0x00,0x48,0x89,0xF9}) &&
           bytes(base + 0x65B437A, {0x80,0x78,0x40,0x00}) &&
           bytes(base + 0x65B43D8, {0xB8,0x58,0x07,0x00,0x00}) &&
           bytes(base + 0x65B4230, {0x48,0x8B,0x89,0xC8,0x01,0x00,0x00,0x48,0x8B,0x01,0x48,0x8B,0x80,0xC8,0x03,0x00,0x00}) &&
           bytes(base + 0x893937, {0x48,0x8B,0xB9,0xF8,0x1D,0x00,0x00}) &&
           bytes(base + 0x56D005, {0x48,0x8B,0x41,0x20,0x48,0x39,0x41,0x18,0x74,0x21,0x48,0x8B,0x48,0xE8,0x48}) &&
           bytes(base + 0x56D018, {0x48,0x8B,0x01,0x48,0x8B,0x80,0xB8,0x01,0x00,0x00}) &&
           bytes(base + 0x56C57E, {0x48,0x8B,0x41,0x18,0x4C,0x8B,0x71,0x20,0x4C,0x39,0xF0}) &&
           bytes(base + 0x56C594, {0x4C,0x89,0xF0,0x48,0xC1,0xF8,0x05}) &&
           bytes(base + 0x56C5CF, {0x49,0x83,0xC6,0xE0}) &&
           bytes(base + 0x4CD2860, {0x48,0x8B,0x49,0x40,0xE9,0xC7,0x4C,0xFF,0xFF}) &&
           bytes(base + 0x4CC7554, {0x48,0x8B,0x41,0x50,0x48,0x85,0xC0}) &&
           bytes(base + 0x4CC7577, {0x4C,0x8B,0x78,0x08}) &&
           bytes(base + 0x4CC7580, {0x49,0x83,0x7F,0x38,0x10}) &&
           bytes(base + 0x4CC7594, {0x49,0x8D,0x7F,0x20}) &&
           bytes(base + 0x4CC75DC, {0x48,0x8B,0x49,0x40,0x48,0x8B,0x01,0x48,0x8B,0x80,0xD8,0x00,0x00,0x00});
}

bool exact_version(std::string& diagnostic) {
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) { diagnostic = "Cannot inspect the Main Menu executable."; return false; }
    const std::wstring_view full(path.data(), length);
    const auto separator = full.find_last_of(L"\\/");
    if (_wcsicmp(std::wstring(full.substr(separator == std::wstring_view::npos ? 0 : separator + 1)).c_str(),
                 L"Minecraft.Windows.exe") != 0) {
        diagnostic = "Main Menu SDK withheld outside Minecraft.Windows.exe 1.26.60.5.";
        return false;
    }
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.data(), &ignored);
    if (!size || size > 16 * 1024 * 1024) { diagnostic = "Cannot read the Main Menu executable version."; return false; }
    std::vector<std::byte> data(size);
    VS_FIXEDFILEINFO* version = nullptr;
    UINT version_size = 0;
    if (!GetFileVersionInfoW(path.data(), 0, size, data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&version), &version_size) ||
        !version || version_size < sizeof(*version) || version->dwSignature != 0xFEEF04BD ||
        version->dwFileVersionMS != MAKELONG(26, 1) || version->dwFileVersionLS != MAKELONG(5, 60)) {
        diagnostic = "Main Menu SDK requires the inspected Minecraft 1.26.60.5 profile; feature disabled.";
        return false;
    }
    return true;
}

} // namespace

bool BedrockMenu::initialize(std::string& diagnostic) {
    if (initialized_) { diagnostic = terminal_diagnostic_; return supported_; }
    initialized_ = true;
    if (!exact_version(diagnostic)) { terminal_diagnostic_ = diagnostic; return false; }
    const auto module = memory::current_process_module(&diagnostic);
    if (!module) { terminal_diagnostic_ = diagnostic; return false; }
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!copy(module->image.address,&dos,sizeof dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<=0 ||
        !module->image.contains(module->image.address+dos.e_lfanew,sizeof nt) ||
        !copy(module->image.address+dos.e_lfanew,&nt,sizeof nt) || nt.Signature!=IMAGE_NT_SIGNATURE ||
        !bedrock_1_26_60_5::known_image(nt.FileHeader.TimeDateStamp,nt.OptionalHeader.SizeOfImage)) {
        diagnostic="Main Menu requires the inspected 1.26.60.5 PE profile.";
        terminal_diagnostic_=diagnostic;
        return false;
    }
    image_ = module->image;
    for (const auto& region : module->executable_regions)
        executable_regions_.push_back({region.address, region.bytes.size()});
    supported_ = true;
    return true;
}

bool BedrockMenu::prepare(std::string& diagnostic) noexcept {
    try {
        std::lock_guard lock(mutex_);
        diagnostic.clear();
        return initialize(diagnostic);
    } catch (...) {
        return false;
    }
}

bool BedrockMenu::code(std::uintptr_t address, std::size_t size) const noexcept {
    return std::ranges::any_of(executable_regions_, [=](const auto& range) { return range.contains(address, size); });
}

MenuScreenState BedrockMenu::poll(void* current_client, std::string& diagnostic,
                                MenuSessionIdentity* session) noexcept {
    if (session) *session = {};
    try {
        std::lock_guard lock(mutex_);
        diagnostic.clear();
        if (!initialize(diagnostic)) return MenuScreenState::unknown;
        const auto fail = [&diagnostic](const char* text) { diagnostic = text; return MenuScreenState::unknown; };
        const auto client = reinterpret_cast<std::uintptr_t>(current_client);
        if (!client) return fail("Main Menu is waiting for the current validated client.");
        const auto method = [this](std::uintptr_t object, std::size_t slot, std::uintptr_t rva) {
            std::uintptr_t table = 0, function = 0;
            return read(object, 0, table) && image_.contains(table, (slot + 1) * sizeof(std::uintptr_t)) &&
                   read(table, slot * sizeof(std::uintptr_t), function) && function == image_.address + rva &&
                   code(function, 8);
        };
        const auto base = image_.address;
        const auto tick = GetTickCount64();
        // Instruction bytes live in the image and do not change per frame.
        // Re-reading them on every Present is a large ReadProcessMemory cost.
        if (!static_code_ok_ || static_code_client_ != client || tick - static_code_tick_ >= 250) {
            if (!verify_static_code(base)) {
                static_code_ok_ = false;
                return fail("Main Menu client/SceneStack code profile mismatch; feature disabled.");
            }
            static_code_ok_ = true;
            static_code_client_ = client;
            static_code_tick_ = tick;
        }
        // Inspect the actual instructions at the selected vtable entries. These
        // are exact observed byte checks, not generated/guessed scan signatures.
        if (!method(client, 243, client_name_rva) || !method(client, 237, current_stack_rva) ||
            !method(client, 160, primary_test_rva) || !method(client, 227, scene_factory_rva) ||
            !method(client, 235, main_stack_rva)) {
            static_code_ok_ = false;
            return fail("Main Menu client/SceneStack code profile mismatch; feature disabled.");
        }

        std::uintptr_t game = 0, factory = 0, factory_control = 0, factory_client = 0;
        std::uint8_t nonprimary = 1, alternate_route = 1, valid = 0;
        if (!read(client, 0x690, nonprimary) || nonprimary != 0 ||
            !read(client, 0x228, factory) || !read(factory, 8, factory_control) ||
            !read(factory_control, 0, valid) || valid != 1 ||
            !read(factory, 0x18, factory_client) || factory_client != client ||
            !read(factory, 0x40, alternate_route) || alternate_route != 0 ||
            !read(client, 0x1C8, game) || !method(game, 121, game_stack_rva))
            return fail("Main Menu current primary SceneStack route is unavailable; feature disabled.");

        std::uintptr_t stack = 0, stack_control = 0, begin = 0, end = 0, top = 0;
        if (!read(game, 0x1DF8, stack) || !read(stack, 8, stack_control) ||
            !read(stack_control, 0, valid) || valid != 1 || !method(stack, 44, stack_name_rva) ||
            !method(stack, 30, stack_visit_rva) ||
            !read(stack, 0x18, begin) || !read(stack, 0x20, end) || end < begin ||
            end - begin > stack_entry_size * 256 || (end - begin) % stack_entry_size != 0)
            return fail("Main Menu SceneStack is unreadable or has an unverified layout.");
        if (end == begin) return MenuScreenState::other;
        if (!read(end - 24, 0, top) || !top) return MenuScreenState::other;
        // Other scene implementations (including OreUI) are intentionally not
        // interpreted as UIScene. They can never enable the replacement menu.
        if (!method(top, 55, scene_name_rva)) return MenuScreenState::other;
        std::uintptr_t view = 0, definition_owner = 0, definition = 0, controller = 0;
        if (!read(top, 0x40, view) || !read(view, 0x50, definition_owner) ||
            !read(definition_owner, 8, definition) || !definition ||
            definition > std::numeric_limits<std::uintptr_t>::max() - 0x20)
            return fail("Main Menu top scene's screen definition is unavailable.");
        const auto name = read_msvc_string(reinterpret_cast<void*>(definition + 0x20), 96);
        if (!name || name->empty()) return fail("Main Menu screen name could not be read safely.");
        const bool disconnected = *name == "disconnect_screen" || *name == "realms_disconnect_screen";
        if (*name != "start_screen" && !disconnected) return MenuScreenState::other;
        // The effective native name is raw definition name + controller's
        // virtual-27 suffix (view_name_rva + 0xAC, concatenation at +0x1B9).
        // Accept only the two observed pure empty-string implementations. Stateful
        // suffixes/popups remain unknown; never mistake their raw start id for
        // the exact native start_screen name. This code is inspected, not run.
        if (!read(view, 0x40, controller))
            return fail("Main Menu controller name suffix is unavailable; feature disabled.");
        const bool empty_suffix = method(controller, 27, empty_suffix_rva) &&
            bytes(base + empty_suffix_rva,
                   {0x48,0x89,0xD0,0x0F,0x57,0xC0,0x0F,0x11,0x02,
                    0x48,0xC7,0x42,0x10,0x00,0x00,0x00,0x00,
                    0x48,0xC7,0x42,0x18,0x0F,0x00,0x00,0x00,0xC3});
        // The live title controller uses a load-from-zero-literal variant.
        // Check both the complete function and its RIP-relative data source;
        // no foreign string construction, allocator or callback is executed.
        const bool title_empty_suffix = method(controller, 27, title_empty_suffix_rva) &&
            bytes(base + title_empty_suffix_rva, {0x48,0x89,0xD0,0x48,0xC7,0x42,0x10,0x00,0x00,0x00,0x00,0x48,0xC7,0x42,0x18,0x0F,0x00,0x00,0x00,0x0F,0x10,0x05,0x16,0x4D,0xAF,0x0E,0x0F,0x11,0x02,0xC3}) &&
            bytes(base + title_empty_storage_rva, {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0});
        if (!empty_suffix && !title_empty_suffix)
            return fail("Main Menu controller name suffix is not proven empty; feature disabled.");
        // Re-read all borrowed links after the string snapshot. A pop, replace,
        // invalidated NonOwnerPointer or client replacement fails closed.
        std::uintptr_t again = 0;
        if (!read(stack_control, 0, valid) || valid != 1 || !read(factory_control, 0, valid) || valid != 1 ||
            !read(client, 0x228, again) || again != factory ||
            !read(factory, 8, again) || again != factory_control ||
            !read(factory, 0x18, again) || again != client ||
            !read(stack, 8, again) || again != stack_control ||
            !read(game, 0x1DF8, again) || again != stack || !read(stack, 0x18, again) || again != begin ||
            !read(stack, 0x20, again) || again != end || !read(end - 24, 0, again) || again != top ||
            !read(top, 0x40, again) || again != view || !read(view, 0x50, again) || again != definition_owner ||
            !read(definition_owner, 8, again) || again != definition ||
            !read(view, 0x40, again) || again != controller ||
            !read(client, 0x1C8, again) || again != game ||
            read_msvc_string(reinterpret_cast<void*>(definition + 0x20), 96) != name)
            return fail("Main Menu screen changed during inspection; frame withheld.");
        if (session) *session = {client, game, factory, factory_control, stack, stack_control,
                                 top, view, definition_owner, definition, controller};
        return disconnected ? MenuScreenState::disconnected : MenuScreenState::main_menu;
    } catch (...) {
        try { diagnostic = "Main Menu screen inspection failed; feature disabled for this frame."; } catch (...) {}
        return MenuScreenState::unknown;
    }
}

} // namespace medicon::sdk
