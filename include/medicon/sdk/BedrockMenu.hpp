#pragma once

#include <medicon/memory/pattern.hpp>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace medicon::sdk {

enum class MenuScreenState { unknown, main_menu, other, disconnected };

// Read-only identity of one successfully inspected menu. These values are
// comparison tokens, never retained as native call targets or ownership.
struct MenuSessionIdentity {
    std::uintptr_t client{}, game{}, factory{}, factory_control{};
    std::uintptr_t stack{}, stack_control{}, scene{}, view{}, definition_owner{}, definition{}, controller{};
    bool operator==(const MenuSessionIdentity&) const = default;
};

// Read-only, fail-closed reader for the exact 1.26.52.3 image inspected for this
// feature. No foreign C++ method, string allocator or reference count is called.
class BedrockMenu {
public:
    [[nodiscard]] MenuScreenState poll(void* current_client, std::string& diagnostic,
                                      MenuSessionIdentity* session = nullptr) noexcept;
    // Version/PE inspection only. Call before the first Present so the first
    // overlay frame can already observe the title screen.
    bool prepare(std::string& diagnostic) noexcept;

private:
    // Tests supply an observed-code image and memory fixtures without weakening
    // the executable/version gate used by ordinary instances.
    friend struct BedrockMenuTestAccess;
    bool initialize(std::string& diagnostic);
    [[nodiscard]] bool code(std::uintptr_t address, std::size_t size) const noexcept;
    std::mutex mutex_;
    bool initialized_ = false;
    bool supported_ = false;
    bool static_code_ok_ = false;
    std::uintptr_t static_code_client_ = 0;
    std::uint64_t static_code_tick_ = 0;
    std::string terminal_diagnostic_;
    memory::AddressRange image_;
    std::vector<memory::AddressRange> executable_regions_;
};

} // namespace medicon::sdk
