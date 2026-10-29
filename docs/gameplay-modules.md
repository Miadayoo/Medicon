# Gameplay modules

All eight modules are registered by `register_builtin_modules`, start disabled,
and use the existing ClickGUI, keybind commands, ArrayList and ConfigStore.

| ID / display name | Category | Behavior and settings |
| --- | --- | --- |
| `chinahat` / ChinaHat | Visual | A translucent conical hat above the local player's head in third person. Radius, height, head offset, color, rainbow and opacity. Hidden in first person. |
| `derp` / Derp | Player | Spins the local head and body yaw. Speed in degrees per second; view and movement direction are unchanged. |
| `aimbot` / Aimbot | Player | Smoothly requests head/body yaw and model pitch toward an eligible target within range and the configured view FOV; camera yaw is unchanged. Smoothing, player filter, visibility filter, and require-click (on by default). |
| `killaura` / Killaura | Player | Attacks the nearest eligible actor at the configured CPS, at most once per game tick. Rotate offers `None` and `Simple`; `Simple` turns toward the target and retains it while eligible. Swing sends the native Attack swing after successful attacks. Range, requested yaw/pitch, player and visibility filters. Rotations owns third-person angle rendering. |
| `scaffold` / Scaffold | Movement | Finds replaceable cells one level below the feet (the highest cell whose top is at or under them, as Solstice's `y - 2.62`), then along horizontal movement. When the target has no face to click, a cell beside it is placed first as support. Extension, hotbar auto-selection and placement delay. |
| `sprint` / Sprint | Movement | Strike requests sprint for movement input in any direction; Legit requires forward input. It updates the native processed/raw SprintDown input and server-facing PlayerAuthInput sprint state. The game applies sprint acceleration and eligibility checks. Sneaking, locked movement, menus and module disable release the request. |
| `esp` / ESP | Visual | Projects actor bounds as a screen box. Range, player filter, color, optional fill, line width and glow toggle. Glow combines a tight core and a long exponential tail in one continuous exterior mesh, with a smooth fade at its outer limit. Spread increases smoothly by up to 40% for large, nearby boxes. Width/intensity are source-only constants in `src/gui/GameplayVisuals.cpp`; old config values are ignored. |
| `rotations` / Rotations | Misc | Owns the shared requested server yaw/pitch and their third-person render interpolation, including pitch-only requests from any source. Speed sets the maximum angular speed, reached at a fixed 180-degree distance. Always keeps rendering aligned to the current view whenever no gameplay module requests a target angle. |

Each built-in feature has its own header and implementation in
`include/medicon/modules/<Feature>Module.hpp` and `src/modules/<Feature>Module.cpp`
(for example, `KillauraModule.hpp` / `KillauraModule.cpp`). Shared gameplay
types and small helpers remain separate from the individual module logic.
`GameplayModules.hpp`, `VisualModules.hpp`, and `ClientModules.hpp` remain as
compatibility umbrella headers; new code can include only the module it uses.
To add a feature, create its module pair and register it in
`src/modules/BuiltinModules.cpp`. CMake discovers new `src/modules/*.cpp`
files automatically.

Open ClickGUI with Insert (or its configured binding), enable Rotations alongside
a gameplay module that requests yaw. For example, `.bind esp g` assigns G to ESP. The commands
and all module settings persist through the normal configuration file.

## Runtime boundary

`sdk::GameplayAccess` is a portable interface for copied actor/frame data,
view rotation, centrally controlled head/body-only rotation, attack, block queries and placement. The native `BedrockGameplay`
implementation is limited to Minecraft Windows `1.26.52.3`. It uses the pinned
Solstice revision already recorded in `docs/signatures.md` as a layout and
behavior reference; it does not link or import the upstream SDK.
The actual executable layout corrections and native call checks are recorded
in [gameplay-sdk-validation.md](gameplay-sdk-validation.md).

The existing local-player baseTick hook captures the current world after the
original update, runs enabled `GameplayModule::game_tick` callbacks, and
reapplies the held head/body yaw and view pitch before closing the native
mutation window with `end_tick`. The renderer's validated body-yaw interpolation
getter uses the held yaw on every render frame, while the held visual yaw and
pitch advance once per Present using elapsed time and the Rotations speed
settings. Rotations computes both rendered angles. The existing validated
DataDriven model callback temporarily applies its pitch to RenderRotation only
for the local third-person draw, then restores the native render value.
CustomModel receives the same Rotations pitch through its render adapter;
first-person arms continue to use the camera pitch. Successful gameplay angle
requests also update outgoing PlayerAuthInput and MovePlayer pitch, yaw and head
yaw, plus PlayerAuthInput's interact rotation (which servers use for attacks and
block use), through the existing packet-send hook. This uses the requested angles rather
than render interpolation and works with Rotations rendering disabled. The
request is cleared on target loss, input capture, module disable or world reset;
Rotations Always alone leaves native packet angles untouched. Sprint updates native movement
input and outgoing PlayerAuthInput sprint state from view-relative input rather
than velocity, so it can start from rest and ignores coasting or knockback.
The host serializes gameplay updates with settings edits. Calls
from Present only animate the UI and draw copied snapshots. No actor handle is
dereferenced by Present.

Native actions require game focus and captured game mouse input, and stop while
ClickGUI, Social or the replacement main menu owns input. Modules discard missed
attack/placement intervals instead of catching up in a burst. A game tick waits
up to 4 ms for a Present in progress instead of being skipped.

Auto-selection keeps the chosen block slot selected, like Solstice's default
`Full` switch mode, and places from it on the next attempt, after the game has
reported the slot to the server on its own tick. Switching and restoring inside
one call meant the server saw a placement from a slot it never selected. The
original slot is selected again one second after the last placement from the
switched slot while Scaffold runs, and on the next game tick once Scaffold is
turned off, unless the user picked another slot meanwhile. Restoration runs
after client/player validation, before the world snapshot's component checks.
An unavailable inventory or failed write keeps the original slot pending for
the next tick instead of losing it; a confirmed inventory change cancels it.

ESP and ChinaHat reread the camera (view matrix, render origin, FOV) every
Present from the fields the last tick validated: plain copies, no native call.
Actors are drawn between their previous and current tick positions, as the game
draws its models. ChinaHat instead sits on the local player's render position
(`RenderPositionComponent`), which the game moves every frame to where it draws
the model, also reread every Present: the snapshot is copied in baseTick, before
that tick's movement, so it trails the model by a tick. A render position more
than a couple of ticks' motion from the tick position is dropped and the hat
falls back to the tick positions. ChinaHat decides first person from the real camera position,
the render origin moved by the view matrix translation, so a third-person
offset carried by the matrix still shows the hat. Missing player data clears the
frame; rendering expires snapshots after 250 ms without a new game tick. Objects
crossing the camera near plane are conservatively omitted.

## Verification

The gameplay module tests use a mock game to check target filtering, yaw and
pitch, smoothing, attack cadence, placement positions and delays, invalid
frames, and state reset. The visual tests use synthetic camera and actor data
to check projection, filtering, ChinaHat geometry, and the additional outward
glow geometry. Runtime tests verify registration and that Present, disabled
modules, captured UI input, invalid worlds and stopped clients cannot issue
gameplay mutations.

These checks do not establish compatibility with an actual Minecraft world.
The native game ABI and server acceptance of attacks/placements still require
verification in the target `1.26.52.3` game build.
