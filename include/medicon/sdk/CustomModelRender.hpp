#pragma once

#include <cstddef>
#include <string_view>

namespace medicon::sdk::custom_model::bedrock_1_26_52_3 {

// Minimal IDA-derived ABI for the P0 experiment; not a complete game SDK.
// Before use, validate the loaded image/version and a unique executable match
// through the existing signature registry. See docs/custom_model_ida_report.md
// and data/signatures/custom_model_candidates.json for callsite evidence.
inline constexpr std::string_view file_version = "1.26.52.3";
inline constexpr std::string_view investigated_sha256 =
    "47de440dc290bb5bd2a4ccd79e39c00219a155ea2c23bb961e27e323bd2fd942";

// 2026-10-28 runtime report: suppressing this entry hides first-person hands,
// while the third-person player remains visible. Unique signature/valid ABI
// did not establish the required world-render coverage as a sole P0 hook.
// The user now explicitly requests hiding first-person arms too, so this entry
// is used ONLY alongside the near/ECS world queues. Its camera/UI distinction
// is still unproven; never invent a camera offset to classify it.
inline constexpr bool legacy_render_candidate_eligible = false;

// Rejected replacement experiment: the user reports no suppression in any
// camera view. IDA follow-up shows this only adds distant player candidates;
// near actors use queueRenderEntities, including a separate ECS route.
// Keep the pattern/ABI as research evidence, but never register this live.
inline constexpr bool add_player_candidate_eligible = false;
// Callback table 0x14E8E3F30 +0x10 references
// 0x14480E6C0; +0x18 references the type thunk 0x14480EF60, which returns
// the ActorRenderCandidates::addPlayers lambda type descriptor 0x151917F60.
// Entry 0x14480E6F0/6F3 preserves incoming RDX/RCX. Normal exit 0x14480EE46
// sets AL=1, then returns at 0x14480EE68. The bounded whole-function decode
// found only this normal return. Keep its opaque, two-pointer machine ABI:
// do not invent an Actor layout or adjust the borrowed candidate pointer.
// See docs/custom_model_p0_addplayers_candidate.md for the registration and
// callback-table evidence and the failed runtime result.
struct AddPlayersCallback;
struct PlayerCandidate;
using AddPlayerCandidateFn = bool (*)(AddPlayersCallback*, PlayerCandidate*);
inline constexpr std::string_view add_player_candidate_pattern =
    "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? ? ? "
    "48 8D AC 24 ? ? ? ? 44 0F 29 45 ? 0F 29 7D ? 0F 29 75 ? "
    "48 C7 45 ? ? ? ? ? 48 89 D3 48 89 CF 48 8B 41 ? "
    "48 8B 8A ? ? ? ? F3 44 0F 10 01 F3 0F 10 79 ? "
    "F3 44 0F 5C 00 F3 0F 5C 78 ? 48 8B 77 ? "
    "F3 0F 10 71 ? F3 0F 5C 70 ? 48 8D 55 ? 48 89 D9 E8 ? ? ? ?";

// World-queue experiment. IDA callback table/type thunk links 0x144716E80
// to LevelRendererCamera::queueRenderEntities lambda_78. Its caller at
// 0x144692C89/C93 passes actor directly in RDX and ignores the return.
// The independent ECS helper at 0x1447172D0 reads the actor from [RDX+0]
// (0x1447172E9); callers E40/FD5/C2D in the same outer function ignore return.
// These aliases describe the observed two-pointer Windows x64 call boundary,
// using void because none of the evidenced callers consumes a return value.
// They are not recovered complete native C++ types. No sret/STL objects are
// synthesized. See docs/custom_model_p0_queue_candidates.md and its JSON.
struct NearActorQueueCallback;
struct EcsActorQueueContext;
struct ActorOwnerRef;
using NearActorQueueFn = void (*)(NearActorQueueCallback*, PlayerCandidate*);
using EcsActorQueueFn = void (*)(EcsActorQueueContext*, ActorOwnerRef*);
inline constexpr std::string_view near_actor_queue_pattern =
    "55 41 57 41 56 41 54 56 57 53 48 81 EC ? ? ? ? "
    "48 8D AC 24 ? ? ? ? 0F 29 75 ? 48 C7 45 ? ? ? ? ? "
    "48 89 D6 48 89 CF 48 8B 41 ? 48 8B 59 ? 80 78 ? ? 75 ? "
    "48 8B 47 ? 80 78 ? ? 75 ? 80 78 ? ? 75 ? 48 89 F1 E8 ? ? ? ?";
inline constexpr std::string_view ecs_actor_queue_pattern =
    "41 56 56 57 55 53 48 83 EC ? 48 8B 05 ? ? ? ? "
    "48 31 E0 48 89 44 24 ? 48 8B 3A 80 BF ? ? ? ? ? 0F 85 ? ? ? ? "
    "48 89 CB 48 8B 31 4C 8B 71 ? 41 80 7E ? ? 75 ? "
    "48 8D 4F ? E8 ? ? ? ?";

// Names and the void return/three pointer arguments are supported by the
// native source-location string at VA 0x14EF9CBB0, referenced via 0x14E942BE0
// from VA 0x14558E479. Entry 0x14558D354..0x14558D35A preserves R8/RDX/RCX.
// These are opaque borrowed objects, with no DLL-owned game STL objects.
struct DataDrivenRenderer;
struct BaseActorRenderContext;
struct ActorRenderData;
using DataDrivenRenderFn = void (*)(DataDrivenRenderer*, BaseActorRenderContext*, ActorRenderData*);

inline constexpr std::string_view data_driven_render_pattern =
    "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 98 01 00 00 "
    "48 8D AC 24 ? ? ? ? 44 0F 29 9D ? ? ? ? 44 0F 29 95 ? ? ? ? "
    "44 0F 29 8D ? ? ? ? 44 0F 29 85 ? ? ? ? 0F 29 BD ? ? ? ? "
    "0F 29 B5 ? ? ? ? 48 C7 85 ? ? ? ? ? ? ? ? 4C 89 C6";

// Read only, within the native render callback, using the existing checked
// foreign-memory reader. Do not reinterpret these as a complete object layout.
// ActorRenderData: VA 0x14558D4FF reads the actor; VA 0x14558DAD9..0x14558DAE3
// reads the three position floats. Their coordinate/interpolation semantics
// still need caller/runtime validation before P1 can use them as renderPos.
inline constexpr std::size_t render_data_actor = 0x00;
inline constexpr std::size_t render_data_position = 0x10;

// BaseActorRenderContext -> ScreenContext: VA 0x14558DA46 / 0x14558DA73.
// ScreenContext -> borrowed Tessellator: caller 0x145601A40, native expression
// *(context + 0x28) -> *(screen + 0xB8), before 0x14560212D.
inline constexpr std::size_t context_screen = 0x28;
inline constexpr std::size_t screen_tessellator = 0xB8;

} // namespace medicon::sdk::custom_model::bedrock_1_26_52_3
