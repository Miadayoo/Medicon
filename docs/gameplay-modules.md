# Gameplay modules

All six modules are registered by `register_builtin_modules`, start disabled,
and use the existing ClickGUI, keybind commands, ArrayList and ConfigStore.

| ID / display name | Category | Behavior and settings |
| --- | --- | --- |
| `chinahat` / ChinaHat | Visual | A translucent conical hat above the local player's head in third person. Radius, height, head offset, color, rainbow and opacity. Hidden in first person. |
| `derp` / Derp | Player | Spins the local view. Speed in degrees per second and fixed pitch. |
| `aimbot` / Aimbot | Player | Turns toward an eligible target within range and the configured FOV. Smoothing, player filter, visibility filter, and require-click (on by default). |
| `killaura` / Killaura | Player | Attacks the nearest eligible actor at the configured CPS, at most once per game tick. Range, rotation, player and visibility filters. |
| `scaffold` / Scaffold | Movement | Finds replaceable cells one level below the feet (the highest cell whose top is at or under them, as Solstice's `y - 2.62`), then along horizontal movement. When the target has no face to click, a cell beside it is placed first as support. Extension, hotbar auto-selection and placement delay. |
| `esp` / ESP | Visual | Projects actor bounds as a screen box. Range, player filter, color, optional fill, line width, glow toggle, glow width and glow intensity. Glow fades outwards around the outline. |

Open ClickGUI with Insert (or its configured binding), toggle the module, and
expand its settings. For example, `.bind esp g` assigns G to ESP. The commands
and all module settings persist through the normal configuration file.

## Runtime boundary

`sdk::GameplayAccess` is a portable interface for copied actor/frame data,
rotation, attack, block queries and placement. The native `BedrockGameplay`
implementation is limited to Minecraft Windows `1.26.52.3`. It uses the pinned
Solstice revision already recorded in `docs/signatures.md` as a layout and
behavior reference; it does not link or import the upstream SDK.
The actual executable layout corrections and native call checks are recorded
in [gameplay-sdk-validation.md](gameplay-sdk-validation.md).

The existing local-player baseTick hook captures the current world after the
original update, runs enabled `GameplayModule::game_tick` callbacks, and closes
the native mutation window with `end_tick`. The host serializes this work with
settings edits and rendering. Calls from Present only animate the UI and draw
copied snapshots. No actor handle is dereferenced by the renderer.

Native actions require game focus and captured game mouse input, and stop while
ClickGUI, Social or the replacement main menu owns input. Modules discard missed
attack/placement intervals instead of catching up in a burst. A game tick waits
up to 4 ms for a Present in progress instead of being skipped.

Auto-selection keeps the chosen block slot selected, like Solstice's default
`Full` switch mode, and places from it on the next attempt, after the game has
reported the slot to the server on its own tick. Switching and restoring inside
one call meant the server saw a placement from a slot it never selected. The
original slot is selected again one second after the last placement from the
switched slot, also when Scaffold is turned off, unless the user picked another
slot meanwhile.

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
