# Local commands

`CommandManager` parses `.bind` and `.unbind` against registered module IDs or
display names, with case-insensitive exact matching and quoted names. Unknown
dot commands return `<command> was not found. Try using .help to see the
available commands.` Invalid or ambiguous arguments do not change binds.
`Client::execute_command` resets held-key state and persists each actual change
through the existing atomic `ConfigStore`. A malformed existing config is
retained; the result explicitly reports a session-only change.

`.help` lists the commands and `.help <command>` shows one command's usage.

`.config` manages named copies of the module state in
`%LOCALAPPDATA%\Medicon\configs\<name>.json`, separate from the active
`config.json`:

| Command | Effect |
| --- | --- |
| `.config` / `.config list` | List saved configs |
| `.config save <name>` / `.c s <name>` | Save the current state and make it the current config; replacing another config needs `overwrite` |
| `.config save` / `.c s` | Update the current config |
| `.config load <name>` / `.c l <name>` | Replace the live state, persist it to `config.json` and make it the current config |
| `.config delete <name>` | Delete a saved config |

The current config is the last one saved or loaded and lasts for the session.

Names are up to 64 ASCII letters, digits, `-` or `_`; Windows device names are
rejected. A rejected or malformed config changes nothing. Load uses the same
session rules as startup: ClickGUI closes and CustomModel stays OFF until F8.

Join a Minecraft world, close ClickGUI, and open Minecraft's own chat input.
Enter `.bind clickgui rshift` and press Enter to bind ClickGUI to Right Shift.
F8 is an ordinary assignable module key. The Preview does not provide a command
input.

Once the native chat hook is connected, leading-dot chat commands are cancelled
before the original packet send and copied to a bounded queue. The presentation
thread executes them on the client thread and saves actual bind changes
immediately. Ordinary chat continues through the original send function.
Results are queued for Minecraft's native chat display on the local player's
BaseTick. Command results and errors are never copied into ClickGUI status or
drawn in a Medicon window/toast. Failed native delivery retains the response for
a later valid player tick; it does not fall back to custom UI.
Connection, execution, persistence and delivery diagnostics are written
to `%LOCALAPPDATA%\Medicon\inject.log`.

## Minimal Bedrock bridge

The reference is [surukotonai/solstice, commit
1d7808f52ef4ba6e34a7fbcc65294907a7761611](https://github.com/surukotonai/solstice/tree/1d7808f52ef4ba6e34a7fbcc65294907a7761611).
Its `PacketSendHook.cpp` intercepts `LoopbackPacketSender::send` (vtable slot 2),
checks `Packet::getId` (slot 1, Text = 9), and cancels leading-dot messages.
`OffsetProvider.hpp`, `MinecraftGame.cpp` and `TextPacket.hpp` provide the layout
reference; their version comments are mixed and are not compatibility proof.

Medicon's independently written `sdk::BedrockChat` limits this layout to the
`Minecraft.Windows.exe` file-version family **1.26.52.\***. It scans the existing
`MinecraftGame_instance` definition once and requires a unique in-image result.
It checks the game map at +0x970, head/node sentinels and count, candidate primary
client slots +0x28 / +0x30, sender +0x1D0, and executable in-image vtable entries.
Exactly one validated candidate is required. No game functions are invoked
while discovering the sender. Unsupported/unready layouts do not install a chat
hook and write the reason to `inject.log`. A waiting bridge retries as the game
becomes ready.

For outgoing Text packets, the scalar Win64 `getId(void*)` call identifies the
packet. The MSVC release-x64 AuthorAndMessage variant is read as bytes:
body/type +0xA0, discriminator +0xE8, message string +0xC8. The 32-byte MSVC
string header has size +0x10 and capacity +0x18; inline capacity 15 selects SSO.
Reads use checked, bounded `ReadProcessMemory` against the current process. The
maximum command length is 1024 bytes. Only outgoing chat variant 1 / type 1 and
leading-dot messages are consumed. Ordinary packets/messages use the original
send function. No packet data or foreign string storage is modified.

## Native chat feedback

Feedback uses the catalog's `GuiData_displayClientMessage` signature and the
current client's GuiData pointer at +0x650. The local player getter is client
vtable slot 31; the player's BaseTick is slot 25. The BaseTick callback drains
results only for the current local player, revalidating the client, player and
GUI data before delivery. Minecraft display calls are not made from Present.

The display boundary receives a borrowed 32-byte POD MSVC string envelope, with
inline storage for up to 15 bytes or a pointer to caller-owned, NUL-terminated
bytes for longer messages. A zeroed metadata block supplies a disengaged
optional-string marker. Backing storage remains alive until the call returns.
The native function copies the text into its own storage; its copies use the
game's allocator. Medicon neither imports game STL types nor transfers ownership
of its bytes to the game.

The pinned Solstice `GuiData.cpp`, `BaseTickHook.cpp` and `MemUtils.hpp` provide
the behavioral and ABI references. The display signature resolves uniquely in
the installed 1.26.52.3 executable at RVA 0x155EEA0. Offline disassembly confirms
the source string is copied without being modified or freed, and the zeroed
metadata marker is accepted as disengaged.

## Validation boundary

The decoder tests cover inline/heap strings, copied ownership, invalid lengths,
variant tags, missing terminators, inaccessible pages and unsupported hosts.
Packet fixtures exercise the same ready-state interception path as production:
dot commands are copied and consumed, ordinary chat passes unchanged, untrusted
code is not called, and enqueue failures do not send recognized commands to the
server. Command parsing, client persistence and C API have separate tests. Native
output-envelope tests check the POD representation and borrowed storage boundary.

A read-only inspection of the actual 1.26.52.3 process validated primary-client
and packet-sender discovery. This did not type a chat command or exercise the
full command/response path in Minecraft. Live end-to-end command input and native
feedback remain unverified; the version checks, runtime validation and log are
still required for each game session.
