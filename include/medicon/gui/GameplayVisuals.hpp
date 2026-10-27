#pragma once

namespace medicon {
class ModuleManager;
namespace sdk { struct GameplayFrame; }

namespace gui {

// Call inside the active ImGui frame, before client panels and HUD elements.
// Snapshots contain world-space bounds; no game pointers are dereferenced here.
void render_gameplay_visuals(ModuleManager& modules, const sdk::GameplayFrame& frame,
                             float seconds);

} // namespace gui
} // namespace medicon
