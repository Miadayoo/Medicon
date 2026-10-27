#pragma once

namespace medicon {
class ModuleManager;
}

namespace medicon::preview {

// Inert modules for the Preview scenarios (no game code). They give the GUI a
// realistic spread of categories, name widths, setting types and one Japanese
// name. Only registered with --demo or --scenario; never part of the DLL.
void register_demo_modules(ModuleManager& modules);

} // namespace medicon::preview
