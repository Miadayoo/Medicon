#include <medicon/sdk/BedrockMenu.hpp>

#include <medicon/memory/module_view.hpp>
#include <medicon/sdk/BedrockChat.hpp>

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

// Evidence: solstice 1d7808f52ef4ba6e34a7fbcc65294907a7761611,
// ClientInstance.cpp getScreenName and OffsetProvider.hpp slot 246. That old
// slot was NOT assumed compatible: its complete read path was inspected in
// the actual Minecraft.Windows.exe file version 1.26.52.3, using the existing
// ScreenView_setupAndRender signature to identify the UIScene vtable.
// RVAs below are identity checks, never addresses invoked by this reader.
constexpr std::uintptr_t client_name_rva = 0x5DC7410;
constexpr std::uintptr_t current_stack_rva = 0x5DC6DC0;
constexpr std::uintptr_t primary_test_rva = 0x5DC2330;
constexpr std::uintptr_t scene_factory_rva = 0x5DC6A30;
constexpr std::uintptr_t main_stack_rva = 0x5DC6CA0;
constexpr std::uintptr_t game_stack_rva = 0x7F1A20;
constexpr std::uintptr_t stack_name_rva = 0x4B28730;
constexpr std::uintptr_t stack_visit_rva = 0x4B27BF0;
constexpr std::uintptr_t scene_name_rva = 0x485ABD0;
constexpr std::uintptr_t view_name_rva = 0x484F4A0;
constexpr std::uintptr_t stack_selector_rva = 0x4FFC70;
constexpr std::uintptr_t empty_suffix_rva = 0xC8BC0;
constexpr std::uintptr_t title_empty_suffix_rva = 0xD9BB60;
constexpr std::uintptr_t title_empty_storage_rva = 0xE7586B0;
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

bool relative_call(std::uintptr_t address, std::uintptr_t target) noexcept {
    std::array<std::uint8_t, 5> instruction{};
    if (!copy(address, instruction.data(), instruction.size()) || instruction[0] != 0xE8) return false;
    std::int32_t displacement = 0;
    std::memcpy(&displacement, instruction.data() + 1, sizeof(displacement));
    return static_cast<std::int64_t>(address) + 5 + displacement == static_cast<std::int64_t>(target);
}

// Image bytes do not change between frames. Re-reading them through
// ReadProcessMemory on every Present hitches the overlay and the game.
bool verify_static_code(std::uintptr_t base) noexcept {
    return bytes(base + client_name_rva + 0x14, {0x48,0x89,0xD6,0x48,0x8B,0x01,0x48,0x8B,0x80,0x80,0x07,0x00,0x00}) &&
           bytes(base + client_name_rva + 0xB1, {0x48,0x8B,0x4D,0xF0,0x48,0x8B,0x01,0x48,0x8B,0x80,0x60,0x01,0x00,0x00}) &&
           bytes(base + primary_test_rva, {0x80,0xB9,0x78,0x06,0x00,0x00,0x00,0x0F,0x94,0xC0,0xC3}) &&
           bytes(base + scene_factory_rva, {0x48,0x8B,0x81,0x08,0x02,0x00,0x00,0xC3}) &&
           bytes(base + current_stack_rva + 0x20, {0x48,0x8B,0x07,0x48,0x8B,0x80,0x28,0x07,0x00,0x00,0x48,0x89,0xF9}) &&
           relative_call(base + current_stack_rva + 0x3B, base + stack_selector_rva) &&
           bytes(base + stack_selector_rva + 0xD, {0x80,0x79,0x40,0x00}) &&
           bytes(base + stack_selector_rva + 0x103, {0xBB,0x70,0x07,0x00,0x00}) &&
           bytes(base + main_stack_rva + 8, {0x48,0x8B,0x89,0xA8,0x01,0x00,0x00,0x48,0x8B,0x01,0x48,0x8B,0x80,0xC8,0x03,0x00,0x00}) &&
           bytes(base + game_stack_rva + 7, {0x48,0x8B,0xB9,0x70,0x1D,0x00,0x00}) &&
           bytes(base + stack_name_rva + 5, {0x48,0x8B,0x41,0x20,0x48,0x39,0x41,0x18,0x74,0x21,0x48,0x8B,0x48,0xE8}) &&
           bytes(base + stack_name_rva + 0x18, {0x48,0x8B,0x01,0x48,0x8B,0x80,0xB8,0x01,0x00,0x00}) &&
           bytes(base + stack_visit_rva + 0xE, {0x48,0x8B,0x41,0x18,0x4C,0x8B,0x71,0x20,0x4C,0x39,0xF0}) &&
           bytes(base + stack_visit_rva + 0x24, {0x4C,0x89,0xF0,0x48,0xC1,0xF8,0x05}) &&
           bytes(base + stack_visit_rva + 0x5F, {0x49,0x83,0xC6,0xE0}) &&
           bytes(base + scene_name_rva, {0x56,0x48,0x83,0xEC,0x20,0x48,0x89,0xD6,0x48,0x8B,0x49,0x40}) &&
           relative_call(base + scene_name_rva + 0xC, base + view_name_rva) &&
           bytes(base + view_name_rva + 0x24, {0x48,0x8B,0x41,0x50,0x48,0x85,0xC0}) &&
           bytes(base + view_name_rva + 0x47, {0x4C,0x8B,0x78,0x08}) &&
           bytes(base + view_name_rva + 0x50, {0x49,0x83,0x7F,0x38,0x10}) &&
           bytes(base + view_name_rva + 0x64, {0x49,0x8D,0x5F,0x20}) &&
           bytes(base + view_name_rva + 0xAC, {0x48,0x8B,0x49,0x40,0x48,0x8B,0x01,0x48,0x8B,0x80,0xD8,0x00,0x00,0x00});
}

bool exact_version(std::string& diagnostic) {
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) { diagnostic = "Cannot inspect the Main Menu executable."; return false; }
    const std::wstring_view full(path.data(), length);
    const auto separator = full.find_last_of(L"\\/");
    if (_wcsicmp(std::wstring(full.substr(separator == std::wstring_view::npos ? 0 : separator + 1)).c_str(),
                 L"Minecraft.Windows.exe") != 0) {
        diagnostic = "Main Menu SDK withheld outside Minecraft.Windows.exe 1.26.52.3.";
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
        version->dwFileVersionMS != MAKELONG(26, 1) || version->dwFileVersionLS != MAKELONG(3, 52)) {
        diagnostic = "Main Menu SDK requires the inspected Minecraft 1.26.52.3 profile; feature disabled.";
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
        if (!method(client, 246, client_name_rva) || !method(client, 240, current_stack_rva) ||
            !method(client, 162, primary_test_rva) || !method(client, 229, scene_factory_rva) ||
            !method(client, 238, main_stack_rva)) {
            static_code_ok_ = false;
            return fail("Main Menu client/SceneStack code profile mismatch; feature disabled.");
        }

        std::uintptr_t game = 0, factory = 0, factory_control = 0, factory_client = 0;
        std::uint8_t nonprimary = 1, alternate_route = 1, valid = 0;
        if (!read(client, 0x678, nonprimary) || nonprimary != 0 ||
            !read(client, 0x208, factory) || !read(factory, 8, factory_control) ||
            !read(factory_control, 0, valid) || valid != 1 ||
            !read(factory, 0x18, factory_client) || factory_client != client ||
            !read(factory, 0x40, alternate_route) || alternate_route != 0 ||
            !read(client, 0x1A8, game) || !method(game, 121, game_stack_rva))
            return fail("Main Menu current primary SceneStack route is unavailable; feature disabled.");

        std::uintptr_t stack = 0, stack_control = 0, begin = 0, end = 0, top = 0;
        if (!read(game, 0x1D70, stack) || !read(stack, 8, stack_control) ||
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
            bytes(base + title_empty_suffix_rva,
                   {0x48,0x89,0xD0,0x48,0xC7,0x42,0x10,0x00,0x00,0x00,0x00,
                    0x48,0xC7,0x42,0x18,0x0F,0x00,0x00,0x00,
                    0x0F,0x10,0x05,0x36,0xCB,0x9B,0x0D,0x0F,0x11,0x02,0xC3}) &&
            bytes(base + title_empty_storage_rva, {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0});
        if (!empty_suffix && !title_empty_suffix)
            return fail("Main Menu controller name suffix is not proven empty; feature disabled.");
        // Re-read all borrowed links after the string snapshot. A pop, replace,
        // invalidated NonOwnerPointer or client replacement fails closed.
        std::uintptr_t again = 0;
        if (!read(stack_control, 0, valid) || valid != 1 || !read(factory_control, 0, valid) || valid != 1 ||
            !read(client, 0x208, again) || again != factory ||
            !read(factory, 8, again) || again != factory_control ||
            !read(factory, 0x18, again) || again != client ||
            !read(stack, 8, again) || again != stack_control ||
            !read(game, 0x1D70, again) || again != stack || !read(stack, 0x18, again) || again != begin ||
            !read(stack, 0x20, again) || again != end || !read(end - 24, 0, again) || again != top ||
            !read(top, 0x40, again) || again != view || !read(view, 0x50, again) || again != definition_owner ||
            !read(definition_owner, 8, again) || again != definition ||
            !read(view, 0x40, again) || again != controller ||
            !read(client, 0x1A8, again) || again != game ||
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
