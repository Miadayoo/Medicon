#pragma once

#include <medicon/memory/module_view.hpp>
#include <medicon/sdk/CustomModelRender.hpp>

#include <atomic>
#include <array>
#include <cstdint>
#include <string>

namespace medicon::sdk::custom_model {

enum class QueuePath : std::size_t { near_actor, ecs_actor, data_driven, count };
inline constexpr auto queue_path_count = static_cast<std::size_t>(QueuePath::count);

struct ProbePathFrame {
    std::uint64_t calls{}, local{}, skipped{}, invalid{};
    std::uintptr_t actor{}, callback{};
};

struct ProbeFrame {
    std::uint64_t calls{}, local{}, skipped{}, invalid{}, overhead_ticks{};
    // Display identities only. Never dereference these or use them to select a target.
    std::uintptr_t actor{}, callback{};
    std::array<ProbePathFrame, queue_path_count> paths{};
};

struct ProbeTargets {
    std::uintptr_t near_actor{}, ecs_actor{}, data_driven{};
    explicit operator bool() const noexcept { return near_actor && ecs_actor && data_driven; }
};

// Testable profile/scan boundary. No address is returned for another image or
// a missing/ambiguous match. The live host additionally checks the file version.
[[nodiscard]] ProbeTargets resolve_probe_targets(std::uint32_t timestamp, std::uint32_t image_size,
    std::span<const memory::MemoryRegion> regions, memory::AddressRange image, std::string& diagnostic);

class Probe {
public:
    using LocalMatcher = bool (*)(void* actor, void* owner) noexcept;
    enum class Status { waiting, unavailable, ready };

    // Worker only, before enabling the hook. No I/O or scanning in callbacks.
    // All three entries must resolve. addPlayers is never used; the old render
    // entry is paired with both world queues for the Hide vanilla probe mode.
    [[nodiscard]] ProbeTargets locate(std::string& diagnostic);
    void set_ready(bool ready) noexcept;
    void set_enabled(bool enabled) noexcept { enabled_.store(enabled, std::memory_order_release); }
    // Hide native arms only after the custom first-person mesh is ready.
    // The two world queues also suppress the vanilla third-person body.
    void set_suppress_data_driven(bool value) noexcept { suppress_data_driven_.store(value, std::memory_order_release); }
    [[nodiscard]] bool enabled() const noexcept { return enabled_.load(std::memory_order_acquire); }
    [[nodiscard]] Status status() const noexcept { return status_.load(std::memory_order_acquire); }

    // Called only inside the native callbacks. Near RDX is the actor itself;
    // ECS RDX holds a borrowed actor pointer at +0, copied with a checked read.
    // OFF/unready never reads native arguments or calls the matcher. True
    // means omit this one queue callback; evidenced callers ignore its result.
    [[nodiscard]] bool inspect_near(bedrock_1_26_52_3::NearActorQueueCallback* callback,
        bedrock_1_26_52_3::PlayerCandidate* actor, LocalMatcher match, void* owner) noexcept;
    [[nodiscard]] bool inspect_ecs(bedrock_1_26_52_3::EcsActorQueueContext* context,
        bedrock_1_26_52_3::ActorOwnerRef* component, LocalMatcher match, void* owner) noexcept;
    [[nodiscard]] bool inspect_render(bedrock_1_26_52_3::DataDrivenRenderer* renderer,
        bedrock_1_26_52_3::BaseActorRenderContext* context,
        bedrock_1_26_52_3::ActorRenderData* data, LocalMatcher match, void* owner,
        bool allow_suppression = true) noexcept;
    [[nodiscard]] ProbeFrame take_frame() noexcept;
    // Only after callbacks have drained.
    void reset() noexcept;

private:
    [[nodiscard]] bool inspect(QueuePath path, void* callback, void* argument,
        LocalMatcher match, void* owner, bool allow_suppression = true) noexcept;
    struct PathCounters {
        std::atomic<std::uint64_t> calls{}, local{}, skipped{}, invalid{};
        std::atomic<std::uintptr_t> actor{}, callback{};
    };
    std::atomic_bool enabled_{false};
    std::atomic_bool suppress_data_driven_{true};
    std::atomic<Status> status_{Status::waiting};
    std::array<PathCounters, queue_path_count> paths_{};
    std::atomic<std::uint64_t> ticks_{};
};

} // namespace medicon::sdk::custom_model
