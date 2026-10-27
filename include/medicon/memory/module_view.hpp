#pragma once

#include <medicon/memory/pattern.hpp>

namespace medicon::memory {

struct LoadedModule {
    AddressRange image;
    std::vector<MemoryRegion> executable_regions;
};

// Windows x64 only: inspect the executable loaded in the current process.
// No process opening, injection, patching, or protection changes are performed.
// Views remain valid only while the module remains loaded and readable.
[[nodiscard]] std::optional<LoadedModule> current_process_module(std::string* error = nullptr);

} // namespace medicon::memory
