#include <medicon/sdk/BedrockInput.hpp>
#include <medicon/memory/signature_registry.hpp>

#include <Windows.h>
#include <array>

namespace medicon::sdk::input {

Targets resolve_targets(std::uint32_t timestamp, std::uint32_t image_size,
                        std::span<const memory::MemoryRegion> regions, memory::AddressRange image,
                        std::string& diagnostic) {
    if (timestamp != 0x6AB54E37 || (image_size != 0x12C02000 && image_size != 0x12C01000)) {
        diagnostic = "Native input gate withheld: executable is not the inspected 1.26.52.3 PE profile.";
        return {};
    }
    constexpr std::array definitions{
        memory::SignatureDefinition{"MouseDevice::feed", mouse_feed_pattern, memory::SignatureResolution::direct, 0, 0, "1.26.52.3", false},
        memory::SignatureDefinition{"CameraDirectLookSystemUtil::_handleLookInput", look_input_pattern, memory::SignatureResolution::direct, 0, 0, "1.26.52.3", false}};
    memory::SignatureRegistry registry;
    registry.scan(definitions, regions, image);
    constexpr std::array<std::uintptr_t, 2> rvas{0x361AD0, 0x66EE3D0};
    std::array<std::uintptr_t, 2> addresses{};
    for (std::size_t i = 0; i < definitions.size(); ++i) {
        const auto* match = registry.find(definitions[i].name);
        if (!match || !match->address || !image.contains(image.address + rvas[i]) ||
            *match->address != image.address + rvas[i]) {
            diagnostic = std::string("Native input gate withheld: ") + std::string(definitions[i].name) +
                " must match uniquely at its inspected function entry.";
            return {};
        }
        addresses[i] = *match->address;
    }
    diagnostic = "Native mouse feed and camera look input uniquely resolved for 1.26.52.3.";
    return {addresses[0], addresses[1]};
}

Targets locate_targets(std::string& diagnostic) {
    const auto module = memory::current_process_module(&diagnostic);
    if (!module) return {};
    const auto read = [&](std::uintptr_t address, auto& value) {
        SIZE_T count = 0;
        return module->image.contains(address, sizeof(value)) &&
            ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                              &value, sizeof(value), &count) && count == sizeof(value);
    };
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!read(module->image.address, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
        !read(module->image.address + static_cast<std::uint32_t>(dos.e_lfanew), nt) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        diagnostic = "Native input gate withheld: unreadable x64 PE header.";
        return {};
    }
    return resolve_targets(nt.FileHeader.TimeDateStamp, nt.OptionalHeader.SizeOfImage,
                           module->executable_regions, module->image, diagnostic);
}

} // namespace medicon::sdk::input
