# Disconnect recovery (Minecraft 1.26.52.03)

Current 1.26.60.5 SDK identities and changed handler fields are recorded in
[the port report](bedrock-1.26.60.md). The addresses below are historical.

The disconnect screen has three buttons at the bottom left:

* **Reconnect** retries the destination retained by Minecraft.
* **Switch Account** opens AltManager. A successful Use closes it without connecting.
* **Switch Account and Reconnect** opens AltManager and retries only when that specific Use request succeeds.

An authentication failure keeps AltManager open with its error. Closing the account picker or leaving the disconnect screen cancels automatic reconnect, including a late authentication completion. The existing native multiplayer/NetherNet authorization bridge supplies the selected account on the new connection; the green In use marker still follows actual login serialization.

The overlay draws only the recovery controls. Native buttons outside their rectangle and Escape retain their input. With no saved destination, the two reconnect actions are disabled and an explanation is displayed; account selection remains available.

## Inspected native path

Evidence: local LeviLauncher `Minecraft.Windows.exe` 1.26.52.03, SHA-256 `47de440dc290bb5bd2a4ccd79e39c00219a155ea2c23bb961e27e323bd2fd942`. All addresses below are RVAs. Initialization checks the known PE timestamp/packaging, vtable entries and instruction bytes, and uses BedrockMenu's exact executable/version gate.

| RVA | Role |
| --- | --- |
| `5DB4440` | `ClientInstance::_handleDisconnectionScreenRequests`; native `this` is the client interface at primary client + `88`. Its prologue explicitly subtracts `88`. The hook preserves the original call, then observes/pumps on that callback thread. |
| `E9731B0` | Primary client vtable; slots 229, 238, 242, 398 identify the factory, SceneStack, router and disconnection handler getters. The disconnect update slot is at `E973F68`. |
| `5DD4550` | Gets the disconnection handler as a 24-byte NonOwnerPointer. For a primary client its object comes from client + `BA0`; the object contains its validity flag pointer at +8. |
| `BD38D0` | Native availability predicate used by the original disconnect update. If handler + `549` is 1, it tests whether the OreUI router contains `/disconnected` and returns the inverse. |
| `BBA02C0` | Native `tryReconnect` binding for `vanilla.connectionErrorInfoFacet`. Checks its handler NonOwnerPointer, invokes callback `5CDE6F0` to obtain the **SceneStack** owner through client vtable +`770`, then calls `BD68A0(handler, sceneFactory, sceneStackOwner)`. |
| `5DC6CA0` | Client vtable slot 238 (+`770`): obtains `NonOwnerPointer<SceneStack>` through MinecraftGame slot 121, `7F1A20`. The SceneStack object is at game +`1D70`. |
| `E8F7A40` | SceneStack vtable; slot 25 (+`C8`) is native pushScene at `4B23B00`, which native retry invokes. |
| `BD68A0` | Native saved-destination retry. Requires handler + `540` (engaged saved destination); copies the complete destination starting at +`1E0`, constructs native `reconnect_game` progress, invokes native scene navigation, and consumes/releases its NonOwnerPointer argument. |
| `758E0` | Native NonOwnerPointer cleanup, used only if the newly acquired native owner fails the final identity check. |

The official bundled `data/gui/dist/hbui/index-ded6287479b3aaccfe37.js` uses `vanilla.connectionErrorInfoFacet.tryReconnect()` for its standard disconnect route. Its C++ binding registration at `BB9FEA0` names `tryReconnect` and registers `BBA02C0`. This establishes the purpose and scalar ABI of the path above without manufacturing a foreign ScreenEvent, string, server descriptor or std::function.

JSON UI uses the existing bounded BedrockMenu reader, extended for exact raw `disconnect_screen` and `realms_disconnect_screen` names with a verified empty controller suffix. OreUI uses the native route predicate on the original callback thread. Rendering and input read only our copied state.

Requests contain a disconnect generation and expire after 1.5 seconds awaiting a native callback. Repeated requests are rejected while queued and for 750 ms after dispatch. Native targets are reacquired from the current primary client; they are never queued or retained as ownership. Native getter and native retry manage both ends of the temporary SceneStack NonOwnerPointer. The callback admission/drain guard and reconnect hook group participate in DLL shutdown.

## Crash correction (v2)

The first implementation incorrectly supplied a handler owner from client +`C70` as retry's third argument. It has the same 24-byte layout as a SceneStack owner, but native retry calls its object's vtable +`C8` as pushScene. The v2 dispatch uses +`770` and independently checks the object against the current game's SceneStack, its exact vtable, pushScene target and live validity flag. A wrong/dead/changed owner is released natively and never dispatched. Regression fixtures deliberately supply a valid handler owner and verify that it cannot reach retry.

Both account picker actions close their overlay after successful authentication. While the underlying disconnect screen owns its cursor, the mouse capture policy now discards capture restoration from earlier gameplay and invokes neither native release nor native grab. A regression fixture covers a pending gameplay restore followed by opening/closing the disconnect account picker. The switch-only flow remains separate from reconnect and is logged explicitly, as are request publication and v2 native dispatch. Live confirmation of the reported switch-only crash is still needed.

## Validation

The code and vtable guards were compared byte-for-byte with the supplied executable. Preview `disconnect_recovery` exercises all three buttons, successful selection, unrelated completion IDs, authentication failure, cancel, leaving the screen while authentication runs, and missing destinations. BedrockMenu fixtures cover both disconnect names, including heap string storage. All 12 targeted checks passed: bedrock_reconnect, mouse_capture, alt, bedrock_auth, bedrock_menu, vanilla_navigation, runtime, dll_lifecycle, overlay_d3d11, social_ui, preview_alt_manager and preview_disconnect_recovery. Recovery screenshots were checked at 1280×720 and 640×360.

Actual kick/rejoin on a live server requires the rebuilt DLL to be reloaded; static/Preview checks do not establish a successful live server join for these new buttons.
