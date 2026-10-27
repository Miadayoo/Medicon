#include <medicon/memory/module_view.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <limits>
#endif

namespace medicon::memory {

#ifdef _WIN32
namespace {

bool readable(DWORD protection) noexcept {
    if (protection & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    switch (protection & 0xFFU) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY: return true;
    default: return false;
    }
}

bool executable(DWORD protection) noexcept {
    switch (protection & 0xFFU) {
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY: return true;
    default: return false;
    }
}

bool copy_readable(std::uintptr_t address, void* destination, std::size_t length) {
    auto* output = static_cast<std::byte*>(destination);
    while (length > 0) {
        MEMORY_BASIC_INFORMATION information{};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &information, sizeof(information)) ||
            information.State != MEM_COMMIT || !readable(information.Protect)) return false;
        const auto region_start = reinterpret_cast<std::uintptr_t>(information.BaseAddress);
        if (address < region_start || address - region_start >= information.RegionSize) return false;
        const auto chunk = std::min(length, information.RegionSize - (address - region_start));
        if (chunk > std::numeric_limits<std::uintptr_t>::max() - address) return false;
        std::memcpy(output, reinterpret_cast<const void*>(address), chunk);
        output += chunk;
        address += chunk;
        length -= chunk;
    }
    return true;
}

} // namespace
#endif

std::optional<LoadedModule> current_process_module(std::string* error) {
    if (error) error->clear();
    auto fail = [error](const char* message) -> std::optional<LoadedModule> {
        if (error) *error = message;
        return std::nullopt;
    };
#ifdef _WIN32
    const auto handle = GetModuleHandleW(nullptr);
    if (!handle) return fail("GetModuleHandleW could not find the current executable");
    const auto base = reinterpret_cast<std::uintptr_t>(handle);
    IMAGE_DOS_HEADER dos{};
    if (!copy_readable(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < 0 || dos.e_lfanew > 1024 * 1024) return fail("Invalid DOS header");
    const auto nt_offset = static_cast<std::size_t>(dos.e_lfanew);
    if (nt_offset > std::numeric_limits<std::uintptr_t>::max() - base) return fail("PE header address overflow");
    IMAGE_NT_HEADERS64 nt{};
    if (!copy_readable(base + nt_offset, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return fail("Expected a Windows x64 PE image");
    LoadedModule module{{base, nt.OptionalHeader.SizeOfImage}, {}};
    if (module.image.size > std::numeric_limits<std::uintptr_t>::max() - base ||
        !module.image.contains(base, sizeof(IMAGE_DOS_HEADER)) ||
        !module.image.contains(base + nt_offset, sizeof(nt)) ||
        nt.FileHeader.NumberOfSections == 0 || nt.FileHeader.NumberOfSections > 96) {
        return fail("Invalid PE image bounds or section count");
    }
    const auto section_offset = nt_offset + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    const auto section_bytes = static_cast<std::size_t>(nt.FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
    if (section_offset > module.image.size || section_bytes > module.image.size - section_offset) {
        return fail("PE section table is outside the image");
    }
    for (std::size_t i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER section{};
        if (!copy_readable(base + section_offset + i * sizeof(section), &section, sizeof(section))) {
            return fail("PE section table is unreadable");
        }
        if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const auto section_size = section.Misc.VirtualSize ? section.Misc.VirtualSize : section.SizeOfRawData;
        if (!section_size) continue;
        if (section.VirtualAddress >= module.image.size || section_size > module.image.size - section.VirtualAddress) {
            return fail("Executable section is outside the image");
        }
        auto cursor = base + section.VirtualAddress;
        const auto end = cursor + section_size;
        const auto section_region_start = module.executable_regions.size();
        while (cursor < end) {
            MEMORY_BASIC_INFORMATION information{};
            if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &information, sizeof(information))) {
                return fail("VirtualQuery failed for executable section");
            }
            const auto region_start = reinterpret_cast<std::uintptr_t>(information.BaseAddress);
            if (cursor < region_start || cursor - region_start >= information.RegionSize) {
                return fail("VirtualQuery returned invalid bounds");
            }
            const auto length = std::min<std::size_t>(end - cursor, information.RegionSize - (cursor - region_start));
            if (length == 0) return fail("Empty memory region");
            if (information.State == MEM_COMMIT && readable(information.Protect) && executable(information.Protect)) {
                // Join contiguous readable pages within one section so a valid
                // pattern may cross a VirtualQuery protection-region boundary.
                if (module.executable_regions.size() > section_region_start &&
                    module.executable_regions.back().address + module.executable_regions.back().bytes.size() == cursor) {
                    auto& previous = module.executable_regions.back();
                    previous.bytes = {previous.bytes.data(), previous.bytes.size() + length};
                } else {
                    module.executable_regions.push_back({{reinterpret_cast<const std::byte*>(cursor), length}, cursor});
                }
            }
            cursor += length;
        }
    }
    if (module.executable_regions.empty()) return fail("No readable executable sections");
    return module;
#else
    return fail("Loaded-module inspection requires Windows x64");
#endif
}

} // namespace medicon::memory
