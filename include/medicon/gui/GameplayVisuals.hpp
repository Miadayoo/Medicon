#pragma once

namespace medicon {
class ModuleManager;
namespace sdk { struct GameplayFrame; }

namespace gui {

// Call inside the active ImGui frame, before client panels and HUD elements.
// Snapshots contain world-space bounds; no game pointers are dereferenced here.
// partial_tick: 0 just after a game tick, 1 when the next is due. Actors with a
// previous position are drawn between the two, as the game draws them.
void render_gameplay_visuals(ModuleManager& modules, const sdk::GameplayFrame& frame,
                             float seconds, float partial_tick = 1.0F);

} // namespace gui
} // namespace medicon
