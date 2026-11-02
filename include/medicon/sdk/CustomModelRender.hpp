#pragma once

#include <medicon/sdk/BedrockProfile.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace medicon::sdk::custom_model::bedrock_1_26_60_5 {

// Minimal IDA-derived ABI for the P0 experiment; not a complete game SDK.
// Before use, validate the loaded image/version and a unique executable match
// through the existing signature registry. Current instruction/field evidence:
// docs/bedrock-1.26.60.05-feature-evidence.json.
inline constexpr std::string_view file_version = "1.26.60.5";
inline constexpr std::string_view investigated_sha256 =
    "988f6a61403b4381aa41e6c66d64cce441b14e561a9b10f647c2758ef3f2672a";

// Stock and LeviLauncher share the inspected native code and tables.
namespace image_profile = ::medicon::sdk::bedrock_1_26_60_5;
inline constexpr std::uint32_t time_date_stamp = image_profile::timestamp;
inline constexpr std::uint32_t official_image_size = image_profile::image_size;
inline constexpr std::uint32_t launcher_image_size = image_profile::launcher_image_size;
[[nodiscard]] constexpr bool known_image(std::uint32_t timestamp, std::uint32_t image_size) noexcept {
    return image_profile::known_image(timestamp, image_size);
}
// Names the observed header, so a log from another PC shows what differs.
[[nodiscard]] inline std::string image_mismatch(std::string_view stage, std::uint32_t timestamp, std::uint32_t image_size) {
    char text[224]{};
    std::snprintf(text, sizeof text, " withheld: PE TimeDateStamp 0x%08X / SizeOfImage 0x%X is not the inspected "
        "1.26.60.5 build (0x%08X with stock 0x%X or LeviLauncher 0x%X).", static_cast<unsigned>(timestamp), static_cast<unsigned>(image_size),
        static_cast<unsigned>(time_date_stamp), static_cast<unsigned>(official_image_size), static_cast<unsigned>(launcher_image_size));
    return std::string(stage) + text;
}

// Native field changes independent of SIG entry addresses. IDA MCP evidence:
// docs/bedrock-1.26.60.05-custom-model-fields.json. Former offsets were +28/+8.
inline constexpr std::size_t camera_origin_offset = 0x20;
inline constexpr std::size_t texture_gpu_offset = 0;

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
// Historical rejected 1.26.52.3 candidate, excluded from every live resolver.
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

// New near/ECS queues at RVAs 53E9F80 / 53EA3B0 keep the two-pointer
// callback boundary. The near route receives Actor* in RDX; the ECS route
// reads the actor from [RDX]. Callers do not consume the return value.
struct NearActorQueueCallback;
struct EcsActorQueueContext;
struct ActorOwnerRef;
using NearActorQueueFn = void (*)(NearActorQueueCallback*, PlayerCandidate*);
using EcsActorQueueFn = void (*)(EcsActorQueueContext*, ActorOwnerRef*);
inline constexpr std::string_view near_actor_queue_pattern =
    "55 41 57 41 56 41 54 56 57 53 48 81 EC 80 00 00 00 48 8D AC 24 80 00 00 00 0F 29 75 F0 48 C7 45 E8 FE FF FF FF 48 89 D6 48 89 CF 48 8B 41 08 48 8B 59 20 80 78 4C 01 75 ?? 48 8B 47 10 80 78 10";
inline constexpr std::string_view ecs_actor_queue_pattern =
    "41 56 56 57 55 53 48 83 EC 60 48 8B 05 ?? ?? ?? ?? 48 31 E0 48 89 44 24 58 48 8B 3A 80 BF 89 02 00 00 00 0F 85 ?? ?? ?? ?? 48 89 CB 48 8B 31 4C 8B 71 08 41 80 7E 12 00 75 ?? 48 8D 4F 08 E8 2D";

// Native source location F836FF3 -> F26A860 -> 5985040 identifies DDR::render.
// Its entry preserves R8 (data), RDX (context), RCX (renderer). All borrowed.
struct DataDrivenRenderer;
struct BaseActorRenderContext;
struct ActorRenderData;
using DataDrivenRenderFn = void (*)(DataDrivenRenderer*, BaseActorRenderContext*, ActorRenderData*);

inline constexpr std::string_view data_driven_render_pattern =
    "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC C8 01 00 00 48 8D AC 24 80 00 00 00 44 0F 29 BD 30 01 00 00 44 0F 29 B5 20 01 00 00 44 0F 29 AD 10 01 00 00 44 0F 29 A5 00 01 00 00 44 0F 29 9D F0 00 00 00 44 0F 29 95 E0 00 00 00 44 0F 29 8D D0 00 00 00 44 0F 29 85 C0 00 00 00 0F 29 BD B0 00 00 00 0F 29 B5 A0 00 00 00 48 C7 85 98 00 00 00 FE FF FF FF 4C 89 C6 48 89 55 38 48 89 CF 8B 05";

// Read only, within the native render callback, using the existing checked
// foreign-memory reader. Do not reinterpret these as a complete object layout.
// DDR 5985244 reads the actor; 5985830..583D reads position data.
inline constexpr std::size_t render_data_actor = 0x00;
inline constexpr std::size_t render_data_position = 0x10;

// BaseActorRenderContext -> ScreenContext remains +28. Native actor draw at
// 5A1CB92..5A1CBA8 reads the borrowed Tessellator at ScreenContext+B0.
// B8 is an unrelated resource in 1.26.60, not the old Tessellator field.
inline constexpr std::size_t context_screen = 0x28;
inline constexpr std::size_t screen_tessellator = 0xB0;

} // namespace medicon::sdk::custom_model::bedrock_1_26_60_5
