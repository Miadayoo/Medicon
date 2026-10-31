# Gameplay modules

All nine modules are registered by `register_builtin_modules`, start disabled,
and use the existing ClickGUI, keybind commands, ArrayList and ConfigStore.

| ID / display name | Category | Behavior and settings |
| --- | --- | --- |
| `chinahat` / ChinaHat | Visual | A translucent conical hat above the local player's head in third person. Radius, height, head offset, color, rainbow and opacity. Hidden in first person. |
| `derp` / Derp | Player | Spins the local head and body yaw. Speed in degrees per second; view and movement direction are unchanged. |
| `aimbot` / Aimbot | Player | Smoothly requests head/body yaw and model pitch toward an eligible target within range and the configured view FOV; camera yaw is unchanged. Smoothing, player filter, visibility filter, and require-click (on by default). |
| `killaura` / Killaura | Player | Attacks the nearest eligible actor at the configured CPS, at most once per game tick. Rotate offers `None` and `Simple`; `Simple` turns toward the target and retains it while eligible. Swing sends the native Attack swing after successful attacks. Range, requested yaw/pitch, player and visibility filters. Rotations owns third-person angle rendering. |
| `scaffold` / Scaffold | Movement | Finds replaceable cells one level below the feet (the highest cell whose top is at or under them, as Solstice's `y - 2.62`), then along horizontal movement. When the target has no face to click, a cell beside it is placed first as support. Extension, hotbar auto-selection and placement delay. |
| `sprint` / Sprint | Movement | Rage requests sprint for movement input in any direction; Legit requires forward input. It updates the native processed/raw SprintDown input and server-facing PlayerAuthInput sprint state. The game applies sprint acceleration and eligibility checks. Sneaking, locked movement, menus and module disable release the request. |
| `velocity` / Velocity | Movement | Full cancels incoming SetActorMotion packets for the local player. Percent exposes separate Horizontal (X/Z) and Vertical (Y) sliders, each 0–100 in steps of 1. 0% removes that component, 100% preserves it. Delay holds copied knockback vectors and applies them on a later game tick; Delay (ms) is 0-10,000, default 8,000, slider step 50. Zero passes packets through immediately. Defaults to Full with both percentages at 0. |
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
view rotation, centrally controlled head/body-only rotation, attack, block queries and placement. Velocity intercepts the native incoming motion dispatcher; its Delay mode later applies the copied vector through the validated game-thread velocity setter. The native `BedrockGameplay`
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

Velocity Delay uses a monotonic clock, with a separate deadline for each hit.
Later hits cannot postpone earlier ones; shortening the setting preserves FIFO
order. At most one overdue hit enters the movement pipeline per game tick. The
queue holds up to 256 copied vectors and native arrival timestamps; overflow
passes the new packet through unchanged. A zero-motion SetActorMotion is a
server velocity stop, not a knockback: it is applied immediately through the
native dispatcher and retires older delayed impulses. Disable,
leaving Delay mode, captured UI input, focus loss, death, actor/world changes,
or clearing the gameplay snapshot discard pending hits. Native packet pointers
and network/session callbacks are never retained. When native movement prediction
is active, release calls its motion enqueue and update functions, matching the
normal SetActorMotion branch. The arrival timestamp advances by the actual
time held, using the original native clock epoch. When prediction is bypassed
or absent, release writes velocity directly. The native adapter currently delays
only tick-zero packets with a positive arrival timestamp; other packets pass
through normally. It does not replay the original packet's rewind tick.

Sync Replies (on by default, shown only in Delay mode) also holds outgoing
NetworkStackLatency replies while KB is pending. Each reply waits for the KB
preceding it to enter the movement pipeline, then is sent in the same update. There is
no extra simulation tick between application and confirmation. Later replies
stay behind that queue, including failed sends after the last KB write,
so a later acknowledgment cannot overtake an earlier one. Up to 512 copied
replies are retained. Disable, input capture and leaving Delay cancel queued
KB and flush replies on the next valid game tick; world/actor/snapshot changes
discard replies from the old context. Failed sends retry. Cleanup keeps the
game tick active even with Velocity OFF until replies are returned.

NetworkStackLatency's timestamp/acknowledgment value is opaque server data and
is returned bit-for-bit unchanged. Before any KB is queued, Delay leaves replies
on their native send path. With Sync Replies OFF and no older held replies,
replies also pass through normally. Turning Sync Replies OFF while replies are
pending preserves FIFO until the next game tick flushes the queue. The v10
timestamp spoof and its unit guessing were removed after two sessions showed
rewritten replies followed by disconnects with no KB queued or applied.

Local MovePlayer Respawn/Teleport (modes 1/2) also retires all older held KB,
even when the actor and world identifiers stay the same. Its pending confirmations
remain FIFO and become eligible on the next valid game tick. A new hit received
after the reset keeps its own delay and confirmation barrier. Normal/head-only
movement and other players' resets do not retire local KB. Receive callbacks
retain scalar reset notices if rendering is busy and consume them before the next
module update, motion admission, or latency admission.

The native automatic reply uses the sender's `sendToServer` entry (vtable
slot 4), bypassing general/chat `send` (slot 2). Both entries are intercepted;
replay calls the original slot 4. Startup logs identify this as `NSL-route-v2`.

This is selective KB/confirmation delay, not a full network-latency emulator.
It does not delay PlayerAuthInput, world updates, or RakNet ping/pong. A server
may use different confirmation mechanisms or enforce its own timeouts.
Every two seconds while Delay is active, `inject.log` records `Velocity Delay:`
with pending counts, queued/applied KB, latency seen/held/sent, and failures.
`nsl_hook_hits` counts visits to the two hooks, `nsl_rejected` counts sender or
packet validation failures, and `nsl_busy` counts lock timeouts that pass through.
General send can forward to sendToServer, so an unheld reply may visit both hooks.
Zero `latency_seen` means no eligible validated latency reply was observed; it
does not establish which synchronization mechanism a server uses.
`Velocity queue:` records the received motion, input tick and native arrival
timestamp. `Velocity release:` records admission/send totals, failures,
`native_predicted`/`direct` counts, position and velocity immediately
when a release or retry changes those totals. Velocity lines include milliseconds.
Native prediction admission can leave the current velocity unchanged until
simulation consumes the queued impulse. Incoming CorrectPlayerMovePrediction
and Disconnect messages are observed and logged without changing their processing.
MovePlayer is also logged; its local reset modes additionally retire stale queued
KB while preserving the original native handler. `position_resets` and
`invalidated_kb` record this invalidation. `zero_motion` counts server velocity
stops that were passed through immediately instead of being delayed. The log
does not cover every possible position correction mechanism.
In Minecraft, the DLL also attempts to append these lines to `velocity.log`
beside the loaded DLL; if that directory is not writable, `inject.log` remains
available. The current session marker is `opaque-nsl-v11`.
Server correction and anti-cheat acceptance, including Hive/Flareon, remain unverified. See
[the investigation](velocity-delay-investigation.md).

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
