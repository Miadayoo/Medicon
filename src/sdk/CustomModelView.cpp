#include <medicon/sdk/CustomModelView.hpp>
#include <medicon/sdk/CustomModelP3Patterns.hpp>
#include <medicon/memory/module_view.hpp>
#include <medicon/memory/signature_registry.hpp>

#include <Windows.h>
#include <array>
#include <cstddef>
#include <atomic>

namespace medicon::sdk::custom_model {
namespace {
std::atomic_bool profile_ready{};
std::atomic<std::uintptr_t> client_options_getter{}, option_getter{};
template<class T> bool read_at(std::uintptr_t base, std::size_t offset, T& result) noexcept {
    SIZE_T count{};
    return base && offset <= UINTPTR_MAX - sizeof(T) && base <= UINTPTR_MAX - offset - sizeof(T) &&
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(base + offset),
                          &result, sizeof(T), &count) && count == sizeof(T);
}
}

bool initialize_view_reader(std::string& diagnostic) {
    profile_ready.store(false,std::memory_order_release);
    const auto module=memory::current_process_module(&diagnostic);
    if(!module)return false;
    using D=memory::SignatureDefinition;using R=memory::SignatureResolution;
    constexpr std::array definitions{
        D{"view_options",p3_native::view_options,R::direct,0,0,"1.26.60.5",false},
        D{"view_option_ref",p3_native::view_option_ref,R::direct,0,0,"1.26.60.5",false},
        D{"view_option_array",p3_native::view_option_array,R::direct,0,0,"1.26.60.5",false}};
    memory::SignatureRegistry registry;
    registry.scan(definitions,module->executable_regions,module->image);
    for(const auto& d:definitions){const auto* result=registry.find(d.name);
        if(!result||!result->address){diagnostic="CustomModel perspective profile unavailable; full model withheld.";return false;}}
    client_options_getter.store(*registry.find("view_options")->address,std::memory_order_relaxed);
    option_getter.store(*registry.find("view_option_ref")->address,std::memory_order_relaxed);
    profile_ready.store(true,std::memory_order_release);
    diagnostic="CustomModel read-only first/third-person option profile ready.";
    return true;
}

const char* view_mode_name(ViewMode view) noexcept {
    switch (view) {
    case ViewMode::first_person: return "first person (custom arms)";
    case ViewMode::third_person: return "third person";
    case ViewMode::third_person_front: return "third person front";
    default: return "unknown (custom body withheld)";
    }
}

ViewMode read_client_view(void* client, ViewProfile profile) noexcept {
    const auto c = reinterpret_cast<std::uintptr_t>(client);
    std::uintptr_t table{}, getter{}, options{}, option_table{}, option_getter{}, option{};
    std::array<std::byte, 8> code{};
    constexpr std::array<std::byte, 8> options_code{
        std::byte{0x48}, std::byte{0x8B}, std::byte{0x81}, std::byte{0x00},
        std::byte{0x0E}, std::byte{0x00}, std::byte{0x00}, std::byte{0xC3}};
    // IDA RVA 65B0590 / ClientInstance vtable[172]: mov rax,[rcx+E00]; ret.
    // Options vtable[1] 5AF8880 -> 5AF8640: options[16+id*8].
    // Native registration at 5B0CAB2..5B0CB01 retains perspective option ID 3.
    if (!profile.client_options_getter || !profile.option_getter ||
        !read_at(c, 0, table) || !read_at(table, 172 * 8, getter) ||
        getter != profile.client_options_getter || !read_at(getter, 0, code) || code != options_code ||
        !read_at(c, 0xE00, options) || !read_at(options, 0, option_table) ||
        !read_at(option_table, 8, option_getter) || option_getter != profile.option_getter ||
        !read_at(options, 0x10 + 3 * 8, option)) return ViewMode::unknown;

    // Native option reads follow Impl+238 overrides, then read IntOption+18.
    // Bound the chain so malformed/cyclic state fails without a native call.
    for (unsigned depth = 0; depth < 8; ++depth) {
        std::uintptr_t impl{}, override_option{};
        int kind{};
        if (!read_at(option, 8, impl) || !read_at(impl, 0x1D4, kind) || (kind != 5 && kind != 6) ||
            !read_at(impl, 0x238, override_option)) return ViewMode::unknown;
        if (override_option) { option = override_option; continue; }
        std::array<int, 4> values{}; // maximum, minimum, current, default
        if (!read_at(option, 0x10, values) || values[0] != 2 || values[1] != 0 ||
            values[2] < 0 || values[2] > 2) return ViewMode::unknown;
        return static_cast<ViewMode>(static_cast<unsigned>(ViewMode::first_person) + values[2]);
    }
    return ViewMode::unknown;
}

ViewMode read_local_view(void* client) noexcept {
    if(!profile_ready.load(std::memory_order_acquire))return ViewMode::unknown;
    return read_client_view(client, {client_options_getter.load(std::memory_order_relaxed),option_getter.load(std::memory_order_relaxed)});
}

} // namespace medicon::sdk::custom_model
