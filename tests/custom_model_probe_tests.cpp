#include <medicon/sdk/CustomModelProbe.hpp>
#include <medicon/memory/pattern.hpp>
#include <Windows.h>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using namespace medicon::sdk::custom_model;
namespace native = bedrock_1_26_52_3;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
std::size_t append_pattern(std::vector<std::byte>& code, std::string_view text) {
    const auto pattern = medicon::memory::Pattern::parse(text);
    require(pattern.has_value(), "P0 signature parses");
    code.insert(code.end(), 16, std::byte{0x90});
    const auto offset = code.size();
    for (const auto& byte : pattern->bytes())
        code.push_back(std::byte{byte.wildcard ? std::uint8_t{0xA5} : byte.value});
    return offset;
}
void resolution_test() {
    static_assert(!native::legacy_render_candidate_eligible && !native::add_player_candidate_eligible);
    std::vector<std::byte> code;
    const auto near_offset = append_pattern(code, native::near_actor_queue_pattern);
    const auto ecs = append_pattern(code, native::ecs_actor_queue_pattern);
    const auto render_offset = append_pattern(code, native::data_driven_render_pattern);
    const std::array regions{medicon::memory::MemoryRegion{code, 0x1000}};
    std::string error;
    const auto targets = resolve_probe_targets(0x6AB54E37, 0x12C02000, regions, {0x1000, code.size()}, error);
    require(targets && targets.near_actor == 0x1000 + near_offset && targets.ecs_actor == 0x1000 + ecs &&
            targets.data_driven == 0x1000 + render_offset, "All three combined entries resolve through the registry");
    // The official install of the same link is one page smaller than the LeviLauncher copy.
    require(static_cast<bool>(resolve_probe_targets(0x6AB54E37, 0x12C01000, regions, {0x1000, code.size()}, error)),
            "The official install's packaging resolves the same entries");
    require(!resolve_probe_targets(0, 0x12C02000, regions, {0x1000, code.size()}, error), "Reject different timestamp");
    require(!resolve_probe_targets(0, 0x12C01000, regions, {0x1000, code.size()}, error),
            "Reject the official size with a different timestamp");
    require(!resolve_probe_targets(0x6AB54E37, 1, regions, {0x1000, code.size()}, error), "Reject different PE size");
    require(error.find("SizeOfImage 0x1 ") != std::string::npos, "A rejected image names its observed header");
    require(!resolve_probe_targets(0x6AB54E37, 0x12C03000, regions, {0x1000, code.size()}, error) &&
            error.find("TimeDateStamp 0x6AB54E37 / SizeOfImage 0x12C03000 ") != std::string::npos,
            "Reject a third packaging and name its observed header");
    require(!resolve_probe_targets(0x6AB54E37, 0x12C02000, regions, {0x1000, ecs}, error), "No partial target set");
    for (const auto offset : {near_offset, ecs, render_offset}) {
        const auto saved = code[offset];
        code[offset] = std::byte{0xCC};
        require(!resolve_probe_targets(0x6AB54E37, 0x12C02000, regions, {0x1000, code.size()}, error),
                "Any missing entry withholds all three hooks");
        code[offset] = saved;
    }
    std::vector<std::byte> duplicate;
    append_pattern(duplicate, native::ecs_actor_queue_pattern);
    const std::array duplicates{regions[0], medicon::memory::MemoryRegion{duplicate, 0x2000}};
    require(!resolve_probe_targets(0x6AB54E37, 0x12C02000, duplicates, {0x1000, 0x2000}, error),
            "Ambiguous ECS entry withholds all three hooks");
    std::vector<std::byte> rejected;
    append_pattern(rejected, native::data_driven_render_pattern);
    append_pattern(rejected, native::add_player_candidate_pattern);
    const std::array old_regions{medicon::memory::MemoryRegion{rejected, 0x1000}};
    require(!resolve_probe_targets(0x6AB54E37, 0x12C02000, old_regions, {0x1000, rejected.size()}, error),
            "Never install the old render entry without the world queues");
}
struct Matcher {
    std::uintptr_t local{0x12340000};
    unsigned calls{};
    Probe* disable_during_match{};
    Probe* invalidate_during_match{};
    static bool run(void* actor, void* owner) noexcept {
        auto& self = *static_cast<Matcher*>(owner);
        ++self.calls;
        if (self.disable_during_match) self.disable_during_match->set_enabled(false);
        if (self.invalidate_during_match) self.invalidate_during_match->set_ready(false);
        return reinterpret_cast<std::uintptr_t>(actor) == self.local;
    }
};
void selection_and_restore_test() {
    for (const auto path : {QueuePath::near_actor, QueuePath::ecs_actor, QueuePath::data_driven}) {
        Probe probe;
        Matcher matcher;
        auto* callback = reinterpret_cast<void*>(0x56780000);
        auto* actor = reinterpret_cast<native::PlayerCandidate*>(matcher.local);
        const auto inspect = [&] {
            if (path == QueuePath::near_actor)
                return probe.inspect_near(static_cast<native::NearActorQueueCallback*>(callback), actor, Matcher::run, &matcher);
            if (path == QueuePath::ecs_actor)
                return probe.inspect_ecs(static_cast<native::EcsActorQueueContext*>(callback),
                    reinterpret_cast<native::ActorOwnerRef*>(&actor), Matcher::run, &matcher);
            return probe.inspect_render(reinterpret_cast<native::DataDrivenRenderer*>(0x1111),
                static_cast<native::BaseActorRenderContext*>(callback),
                reinterpret_cast<native::ActorRenderData*>(&actor), Matcher::run, &matcher);
        };
        require(!inspect() && matcher.calls == 0, "Default OFF never queries actor");
        probe.set_enabled(true);
        require(!inspect() && matcher.calls == 0, "Unready bridge passes through");
        probe.set_ready(true);
        require(inspect(), "Enabled local target skips original");
        const auto sample = probe.take_frame();
        const auto index = static_cast<std::size_t>(path);
        require(sample.calls == 3 && sample.local == 1 && sample.skipped == 1 && sample.actor == matcher.local &&
                sample.callback == reinterpret_cast<std::uintptr_t>(callback) && sample.paths[index].calls == 3 &&
                sample.paths[index].skipped == 1, "Counters and per-path display identity");
        require(probe.take_frame().calls == 0 && probe.take_frame().actor == 0, "Drain retains no target identity");
        actor = reinterpret_cast<native::PlayerCandidate*>(0x99990000);
        require(!inspect(), "Other actors pass through");
        ++matcher.local;
        actor = reinterpret_cast<native::PlayerCandidate*>(matcher.local);
        require(inspect(), "Resolve current local identity on each call");
        probe.set_enabled(false);
        const auto calls = matcher.calls;
        require(!inspect() && matcher.calls == calls, "OFF restores original routing");
        probe.set_enabled(true);
        matcher.disable_during_match = &probe;
        require(!inspect(), "OFF during local lookup wins");
        matcher.disable_during_match = nullptr;
        matcher.invalidate_during_match = &probe;
        probe.set_enabled(true);
        require(!inspect(), "Lost readiness prevents suppression");
        probe.reset();
        require(!probe.enabled() && probe.status() == Probe::Status::waiting && probe.take_frame().calls == 0,
                "Drained stop resets all paths");
        std::string error;
        require(!probe.locate(error) && !error.empty(), "Non-Minecraft host cannot install queue hooks");
    }
}
void native_argument_test() {
    Probe probe;
    probe.set_ready(true);
    Matcher matcher;
    auto* near_context = reinterpret_cast<native::NearActorQueueCallback*>(0x1000);
    auto* ecs_context = reinterpret_cast<native::EcsActorQueueContext*>(0x1000);
    auto* renderer = reinterpret_cast<native::DataDrivenRenderer*>(0x2000);
    auto* render_context = reinterpret_cast<native::BaseActorRenderContext*>(0x3000);
    auto* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    require(page != nullptr, "Allocate unreadable native argument fixture");
    struct Free { void* page; ~Free() { VirtualFree(page, 0, MEM_RELEASE); } } free_page{page};
    auto* component = static_cast<native::ActorOwnerRef*>(page);
    require(!probe.inspect_ecs(ecs_context, component, Matcher::run, &matcher), "OFF ignores unreadable ECS component");
    require(!probe.inspect_render(renderer, render_context, static_cast<native::ActorRenderData*>(page), Matcher::run, &matcher),
            "OFF ignores unreadable render data");
    require(probe.take_frame().invalid == 0 && matcher.calls == 0, "OFF never reads ECS memory or matches actor");
    probe.set_enabled(true);
    require(!probe.inspect_render(renderer, render_context, static_cast<native::ActorRenderData*>(page), Matcher::run, &matcher),
            "Unreadable render actor field passes through");
    require(probe.take_frame().paths[2].invalid == 1 && matcher.calls == 0, "Failed render read never queries local player");
    for (auto* actor : {static_cast<native::PlayerCandidate*>(page),
                       reinterpret_cast<native::PlayerCandidate*>(UINTPTR_MAX - 1)})
        require(!probe.inspect_near(near_context, actor, Matcher::run, &matcher), "Near actor is never dereferenced");
    require(matcher.calls == 2, "Near path compares direct actor pointers");
    require(!probe.inspect_ecs(ecs_context, component, Matcher::run, &matcher), "Unreadable ECS component passes through");
    require(!probe.inspect_ecs(ecs_context, reinterpret_cast<native::ActorOwnerRef*>(UINTPTR_MAX - 1), Matcher::run, &matcher),
            "Overflowing ECS pointer passes through");
    require(!probe.inspect_ecs(ecs_context, nullptr, Matcher::run, &matcher), "Null ECS argument passes through");
    native::PlayerCandidate* actor = nullptr;
    component = reinterpret_cast<native::ActorOwnerRef*>(&actor);
    require(!probe.inspect_ecs(ecs_context, component, Matcher::run, &matcher), "Null actor in component passes through");
    require(matcher.calls == 2 && probe.take_frame().paths[1].invalid == 4, "Invalid ECS reads never call local getter");
    actor = reinterpret_cast<native::PlayerCandidate*>(matcher.local);
    require(!probe.inspect_near(nullptr, actor, Matcher::run, &matcher), "Null near callback passes through");
    require(!probe.inspect_near(near_context, nullptr, Matcher::run, &matcher), "Null near actor passes through");
    require(!probe.inspect_ecs(nullptr, component, Matcher::run, &matcher), "Null ECS context passes through");
    require(matcher.calls == 2 && probe.take_frame().invalid == 3, "Null arguments never call matcher");
    auto* data = reinterpret_cast<native::ActorRenderData*>(&actor);
    require(!probe.inspect_render(nullptr, render_context, data, Matcher::run, &matcher) &&
            !probe.inspect_render(renderer, nullptr, data, Matcher::run, &matcher) &&
            !probe.inspect_render(renderer, render_context, nullptr, Matcher::run, &matcher), "Null render inputs pass through");
    require(probe.take_frame().paths[2].invalid == 3 && matcher.calls == 2, "Null render inputs never invoke matcher");
    require(probe.inspect_near(near_context, actor, Matcher::run, &matcher) &&
            probe.inspect_ecs(ecs_context, component, Matcher::run, &matcher) &&
            probe.inspect_render(renderer, render_context, data, Matcher::run, &matcher), "All routes suppress the same local actor");
    const auto sample = probe.take_frame();
    require(sample.skipped == 3 && sample.paths[0].skipped == 1 && sample.paths[1].skipped == 1 &&
            sample.paths[2].skipped == 1 && sample.paths[0].actor == matcher.local &&
            sample.paths[1].actor == matcher.local && sample.paths[2].actor == matcher.local, "Per-route diagnostics");
}
void preserve_hands_test() {
    Probe probe;probe.set_ready(true);probe.set_enabled(true);probe.set_suppress_data_driven(false);
    Matcher matcher;
    auto* context=reinterpret_cast<native::BaseActorRenderContext*>(0x3000);
    auto* renderer=reinterpret_cast<native::DataDrivenRenderer*>(0x2000);
    auto* actor=reinterpret_cast<native::PlayerCandidate*>(matcher.local);
    auto* data=reinterpret_cast<native::ActorRenderData*>(&actor);
    require(!probe.inspect_render(renderer,context,data,Matcher::run,&matcher)&&matcher.calls==0,"Custom model leaves native hands/items untouched");
    require(!probe.inspect_render(renderer,context,reinterpret_cast<native::ActorRenderData*>(1),Matcher::run,&matcher),"Preserved native render path never reads borrowed actor data");
    require(probe.inspect_near(reinterpret_cast<native::NearActorQueueCallback*>(context),actor,Matcher::run,&matcher)&&
            probe.inspect_ecs(reinterpret_cast<native::EcsActorQueueContext*>(context),reinterpret_cast<native::ActorOwnerRef*>(&actor),Matcher::run,&matcher),"Keeping hands still hides vanilla third-person body on both queues");
    probe.set_suppress_data_driven(true);
    const auto matches=matcher.calls;
    require(!probe.inspect_render(renderer,context,reinterpret_cast<native::ActorRenderData*>(1),Matcher::run,&matcher,false)&&matcher.calls==matches,"Unavailable current custom-arm pose restores native hands without reading actor data");
    require(probe.inspect_render(renderer,context,data,Matcher::run,&matcher),"Hide vanilla probe mode restores arm suppression");
    probe.set_enabled(false);
    require(!probe.inspect_render(renderer,context,data,Matcher::run,&matcher),"Module OFF passes hands through");
}
}
int main() {
    try {
        resolution_test();
        selection_and_restore_test();
        native_argument_test();
        preserve_hands_test();
        std::cout << "Custom model P0 probe tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

