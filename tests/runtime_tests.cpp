#include <medicon/client/Client.hpp>
#include <medicon/gui/InjectSplash.hpp>
#include <medicon/sdk/CustomModelProbe.hpp>
#include <medicon/modules/CustomModelProbeModule.hpp>
#include <medicon/modules/RotationsModule.hpp>
#include <medicon/modules/VelocityModule.hpp>
#include <Windows.h>
#include <imgui.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
using Function = int(*)();
Function original;
Function second_original;
int detour() { return original() + 5; }
int second_detour() { return second_original() + 9; }

namespace native_model = medicon::sdk::custom_model::bedrock_1_26_60_5;
medicon::sdk::custom_model::Probe render_probe;
native_model::NearActorQueueFn near_original;
native_model::EcsActorQueueFn ecs_original;
native_model::DataDrivenRenderFn render_original;
int fixture_local_actor;
bool fixture_is_local(void* actor, void*) noexcept { return actor == &fixture_local_actor; }
void near_detour(native_model::NearActorQueueCallback* callback, native_model::PlayerCandidate* actor) {
    if (render_probe.inspect_near(callback, actor, fixture_is_local, nullptr)) return;
    near_original(callback, actor);
}
void ecs_detour(native_model::EcsActorQueueContext* context, native_model::ActorOwnerRef* component) {
    if (render_probe.inspect_ecs(context, component, fixture_is_local, nullptr)) return;
    ecs_original(context, component);
}
void render_detour(native_model::DataDrivenRenderer* renderer, native_model::BaseActorRenderContext* context,
                   native_model::ActorRenderData* data) {
    if (render_probe.inspect_render(renderer, context, data, fixture_is_local, nullptr)) return;
    render_original(renderer, context, data);
}

void custom_model_hook_test() {
    auto* code = static_cast<unsigned char*>(VirtualAlloc(nullptr, 192, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    require(code != nullptr, "Allocate all three combined ABI fixtures");
    struct Free { void* code; ~Free() { VirtualFree(code, 0, MEM_RELEASE); } } free_code{code};
    std::memset(code, 0x90, 192);
    // Observed Windows x64 two-pointer, ignored-return boundary: increment
    // [rcx], record unchanged RDX at +8, return. No native game code involved.
    const unsigned char function[]{0x48, 0xFF, 0x01, 0x48, 0x89, 0x51, 0x08, 0xC3};
    std::memcpy(code, function, sizeof(function));
    std::memcpy(code + 64, function, sizeof(function));
    // Three-pointer render ABI additionally records original R8 at +16.
    const unsigned char render_function[]{0x48, 0xFF, 0x01, 0x48, 0x89, 0x51, 0x08, 0x4C, 0x89, 0x41, 0x10, 0xC3};
    std::memcpy(code + 128, render_function, sizeof(render_function));
    FlushInstructionCache(GetCurrentProcess(), code, 192);
    const auto call_near = reinterpret_cast<native_model::NearActorQueueFn>(code);
    const auto call_ecs = reinterpret_cast<native_model::EcsActorQueueFn>(code + 64);
    const auto call_render = reinterpret_cast<native_model::DataDrivenRenderFn>(code + 128);
    struct Recorder { std::uint64_t calls{}; void* argument{}; void* render_data{}; } near_record, ecs_record, render_record;
    static_assert(offsetof(Recorder, argument) == 8 && offsetof(Recorder, render_data) == 16);
    auto* callback = reinterpret_cast<native_model::NearActorQueueCallback*>(&near_record);
    auto* context = reinterpret_cast<native_model::EcsActorQueueContext*>(&ecs_record);
    auto* renderer = reinterpret_cast<native_model::DataDrivenRenderer*>(&render_record);
    auto* render_context = reinterpret_cast<native_model::BaseActorRenderContext*>(&fixture_local_actor);
    auto* local = reinterpret_cast<native_model::PlayerCandidate*>(&fixture_local_actor);
    int other_actor;
    auto* other = reinterpret_cast<native_model::PlayerCandidate*>(&other_actor);
    medicon::HookManager hooks;
    std::string error;
    require(hooks.create("P0 near fixture", code, reinterpret_cast<void*>(&near_detour),
                        reinterpret_cast<void**>(&near_original), error), error.c_str());
    require(hooks.create("P0 ECS fixture", code + 64, reinterpret_cast<void*>(&ecs_detour),
                        reinterpret_cast<void**>(&ecs_original), error), error.c_str());
    require(hooks.create("P0 render fixture", code + 128, reinterpret_cast<void*>(&render_detour),
                        reinterpret_cast<void**>(&render_original), error), error.c_str());
    require(hooks.enable_all(error), error.c_str());
    render_probe.reset();
    render_probe.set_ready(true);
    auto* component_actor = local;
    auto* component = reinterpret_cast<native_model::ActorOwnerRef*>(&component_actor);
    auto* data = reinterpret_cast<native_model::ActorRenderData*>(&component_actor);
    call_near(callback, local);
    call_ecs(context, component);
    call_render(renderer, render_context, data);
    require(near_record.calls == 1 && near_record.argument == local &&
            ecs_record.calls == 1 && ecs_record.argument == component && render_record.calls == 1 &&
            render_record.argument == render_context && render_record.render_data == data,
            "OFF forwards all native arguments, including render R8, through their trampolines");
    render_probe.set_enabled(true);
    call_near(callback, local);
    call_ecs(context, component);
    call_render(renderer, render_context, data);
    require(near_record.calls == 1 && ecs_record.calls == 1 && render_record.calls == 1, "All local routes skip original");
    component_actor = other;
    call_near(callback, other);
    call_ecs(context, component);
    call_render(renderer, render_context, data);
    require(near_record.calls == 2 && near_record.argument == other &&
            ecs_record.calls == 2 && ecs_record.argument == component && render_record.calls == 2 &&
            render_record.argument == render_context && render_record.render_data == data, "Other actor arguments pass unchanged");
    int processed = 0;
    for (auto* actor : {local, other, other}) {
        component_actor = actor;
        call_near(callback, actor);
        call_ecs(context, component);
        call_render(renderer, render_context, data);
        ++processed;
    }
    require(processed == 3 && near_record.calls == 4 && ecs_record.calls == 4 && render_record.calls == 4,
            "Skipping local still visits later actors in all paths");
    const auto sample = render_probe.take_frame();
    require(sample.paths[0].calls == 6 && sample.paths[1].calls == 6 && sample.paths[2].calls == 6 &&
            sample.paths[0].skipped == 2 && sample.paths[1].skipped == 2 && sample.paths[2].skipped == 2,
            "All native detours publish separate counts");
    render_probe.set_enabled(false);
    component_actor = local;
    call_near(callback, local);
    call_ecs(context, component);
    call_render(renderer, render_context, data);
    require(near_record.calls == 5 && ecs_record.calls == 5 && render_record.calls == 5, "OFF restores all originals");
    require(hooks.disable_all(error), error.c_str());
    render_probe.set_enabled(true);
    call_near(callback, local);
    call_ecs(context, component);
    call_render(renderer, render_context, data);
    require(near_record.calls == 6 && ecs_record.calls == 6 && render_record.calls == 6, "Hook stop restores all native code");
    require(hooks.clear(error), error.c_str());
    render_probe.reset();
}

void hooks_test() {
    auto* code = static_cast<unsigned char*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    require(code != nullptr, "Allocate hook test function");
    struct Free { void* code; ~Free() { VirtualFree(code, 0, MEM_RELEASE); } } free_code{code};
    std::memset(code, 0x90, 64);
    // x64: mov eax, 7; ret. Remaining padding permits a detour patch.
    const unsigned char function[]{0xB8, 7, 0, 0, 0, 0xC3};
    std::memcpy(code, function, sizeof(function));
    FlushInstructionCache(GetCurrentProcess(), code, 64);
    const auto call = reinterpret_cast<Function>(code);
    {
    medicon::HookManager hooks;
    std::string error;
    require(!hooks.create("bad", nullptr, reinterpret_cast<void*>(&detour), reinterpret_cast<void**>(&original), error), "Reject unresolved hook");
    require(hooks.create("test", code, reinterpret_cast<void*>(&detour), reinterpret_cast<void**>(&original), error), error.c_str());
    require(call() == 7, "Hooks start disabled");
    require(!hooks.create("test", code, reinterpret_cast<void*>(&detour), reinterpret_cast<void**>(&original), error), "Reject duplicate hook");
    require(hooks.enable_all(error), error.c_str());
    require(call() == 12 && original() == 7, "Enabled detour and original trampoline");
    require(hooks.disable_all(error), error.c_str());
    require(call() == 7, "Restore original function");
    require(hooks.enable_all(error), error.c_str());
    require(hooks.clear(error), error.c_str());
    require(call() == 7 && hooks.size() == 0, "Clear restores original bytes");
    }
    // Destroying one manager must not uninitialize MinHook for another manager.
    std::memcpy(code + 32, function, sizeof(function));
    FlushInstructionCache(GetCurrentProcess(), code + 32, sizeof(function));
    auto first = std::make_unique<medicon::HookManager>();
    medicon::HookManager second;
    std::string error;
    require(first->create("first", code, reinterpret_cast<void*>(&detour), reinterpret_cast<void**>(&original), error), error.c_str());
    require(second.create("second", code + 32, reinterpret_cast<void*>(&second_detour), reinterpret_cast<void**>(&second_original), error), error.c_str());
    first.reset();
    require(second.enable_all(error), error.c_str());
    require(reinterpret_cast<Function>(code + 32)() == 16, "Other manager retains MinHook session");
    require(second.clear(error), error.c_str());
}

void gameplay_dispatch_test(const std::filesystem::path& path) {
    require(std::abs(medicon::sdk::interpolate_yaw(10.0F, 50.0F, 0.5F) - 30.0F) < 0.001F,
            "Render yaw interpolation uses the frame alpha");
    require(std::abs(std::abs(medicon::sdk::interpolate_yaw(179.0F, -179.0F, 0.5F)) - 180.0F) < 0.001F,
            "Render yaw interpolation follows the shortest path across wraparound");
    struct Game final : medicon::sdk::GameplayAccess {
        medicon::sdk::GameplayFrame current;
        int rotations = 0;
        int head_body_rotations = 0;
        int velocity_writes = 0;
        int knockback_calls = 0;
        int latency_sends = 0;
        float last_yaw = 0;
        float last_pitch = 0;
        bool sprint_requested = false;
        bool render_rotation = false;
        bool rotation_control = false;
        bool server_rotation = false;
        const medicon::sdk::GameplayFrame& frame() const noexcept override { return current; }
        void begin_rotation_control(bool enabled) noexcept override { rotation_control = enabled; }
        void begin_rotation_control(bool enabled, bool render) noexcept override {
            rotation_control = enabled;
            render_rotation = render;
        }
        void finish_rotation_control(bool requested) noexcept override { server_rotation = rotation_control && requested; }
        bool rotate(float, float) override { ++rotations; return true; }
        bool rotate_head_and_body(float yaw) override {
            return rotate_head_and_body(yaw, current.pitch);
        }
        bool rotate_head_and_body(float yaw, float pitch) override {
            if (!rotation_control) return false;
            last_yaw = yaw;
            last_pitch = pitch;
            ++head_body_rotations;
            return true;
        }
        bool set_sprinting(bool requested) override { sprint_requested = requested; return true; }
        bool set_velocity(medicon::sdk::Vec3 velocity) override {
            ++velocity_writes;
            current.local.velocity = velocity;
            return true;
        }
        medicon::sdk::KnockbackApplyResult apply_knockback(medicon::sdk::Vec3 velocity, std::int64_t timestamp, std::uint64_t input_tick = 0) override {
            ++knockback_calls;
            return GameplayAccess::apply_knockback(velocity, timestamp, input_tick);
        }
        bool send_latency_reply(const medicon::sdk::LatencyReply&) override { ++latency_sends; return true; }
        bool attack(std::uintptr_t) override { return true; }
        bool place_block(medicon::sdk::BlockPos, bool) override { throw std::runtime_error("Unexpected placement"); }
        bool replaceable(medicon::sdk::BlockPos) const override { return false; }
    } game;
    game.current.ready = true;
    game.current.local.id = 1;
    game.current.local.alive = true;
    game.current.local.local = true;
    medicon::Client client(path);
    require(client.start(), "Gameplay client starts");
    for (const auto* id : {"chinahat", "derp", "aimbot", "killaura", "scaffold", "esp", "rotations", "custommodel", "velocity"})
        require(client.modules().find(id) && !client.modules().find(id)->enabled(), "Gameplay module registered disabled");
    client.game_tick(game, 0.05F);
    require(game.rotations == 0 && game.head_body_rotations == 0 && !game.server_rotation,
            "Disabled gameplay modules do not act");
    client.modules().find("rotations")->set_enabled(true);
    client.game_tick(game, 0.05F);
    require(game.rotations == 0 && game.head_body_rotations == 0 && !game.rotation_control,
            "Rotations module alone does not change head/body yaw");
    client.modules().find("derp")->set_enabled(true);
    client.tick(0.05F);
    require(game.rotations == 0 && game.head_body_rotations == 0, "Present tick never mutates the game");
    client.game_tick(game, 0.05F);
    require(game.rotations == 0 && game.head_body_rotations == 1 && game.rotation_control && game.server_rotation,
            "Native game tick dispatches head/body Derp through Rotations");
    client.gui().set_open(true);
    client.game_tick(game, 0.05F);
    require(game.rotations == 0 && game.head_body_rotations == 2 && game.last_yaw == game.current.yaw && !game.server_rotation,
            "ClickGUI suppresses gameplay requests and restores the view angle");
    client.modules().find("rotations")->set_enabled(false);
    client.modules().find("derp")->set_enabled(false);
    client.gui().set_open(false);
    auto* render_module = dynamic_cast<medicon::RotationsModule*>(client.modules().find("rotations"));
    render_module->always.get<bool>() = true;
    render_module->set_enabled(true);
    game.current.pitch = 35;
    client.game_tick(game, 0.05F);
    medicon::RotationsModule::RenderAngles render_angles;
    require(game.render_rotation && !game.server_rotation &&
            render_module->update_render(1, game.current.yaw, game.current.pitch, 0, render_angles) &&
            render_angles.pitch == 35,
            "Rotations Always renders vertical view angles without Killaura");
    render_module->always.get<bool>() = false;
    game.current.pitch = 12;
    game.current.move_input_ready = true;
    game.current.move_input = {0, 1};
    medicon::sdk::ActorSnapshot target;
    target.id = 2;
    target.position = {0, 2, 2};
    target.player = target.alive = target.visible = true;
    game.current.actors = {target};
    client.modules().find("rotations")->set_enabled(true);
    client.modules().find("killaura")->set_enabled(true);
    client.modules().find("sprint")->set_enabled(true);
    client.game_tick(game, 0.05F);
    require(game.sprint_requested && game.render_rotation && game.server_rotation && std::abs(game.last_pitch + 45) < 0.001F &&
            game.current.pitch == 12,
            "Client route forwards Sprint input and Killaura model pitch without changing the camera");
    client.game_tick(game, 0.05F, false);
    require(!game.sprint_requested && !game.server_rotation && game.last_pitch == 12 && game.last_yaw == game.current.yaw,
            "Focus loss releases Sprint and restores the actual view angle");
    render_module->set_enabled(false);
    client.game_tick(game, 0.05F);
    require(!game.render_rotation && !render_module->render_enabled() && game.server_rotation,
            "Killaura enabled vertical model rendering while Rotations was off");
    client.modules().find("antibot")->set_enabled(true);
    client.game_tick(game, 0.05F);
    require(!game.server_rotation && game.current.actors[0].bot, "AntiBot did not exclude a marked actor from Killaura");
    client.modules().find("antibot")->set_enabled(false);
    client.game_tick(game, 0.05F);
    require(game.server_rotation && !game.current.actors[0].bot, "Disabling AntiBot did not clear its mark");
    game.current.actors.clear();
    client.game_tick(game, 0.05F);
    require(!game.server_rotation && game.last_yaw == game.current.yaw && game.last_pitch == game.current.pitch,
            "Losing the target retained a server rotation request");
    client.modules().find("rotations")->set_enabled(false);
    client.modules().find("killaura")->set_enabled(false);
    client.modules().find("sprint")->set_enabled(false);
    require(!render_module->update_render(1, game.current.yaw, game.current.pitch, 0.05F, render_angles),
            "Turning Rotations off did not disable vertical model rendering");
    auto* velocity = dynamic_cast<medicon::VelocityModule*>(client.modules().find("velocity"));
    require(velocity != nullptr, "Velocity type is registered");
    velocity->mode.get<medicon::Choice>().index = 2;
    velocity->set_enabled(true);
    game.current.world_identity = 10;
    const auto received = medicon::VelocityModule::Clock::now() - std::chrono::seconds(9);
    medicon::sdk::Vec3 knockback{2, 4, -6};
    const auto queue_hit = [&] {
        require(velocity->handle_motion(knockback, game.current, received) == medicon::VelocityModule::MotionResult::delayed,
                "Runtime could not queue delayed knockback");
    };
    queue_hit(); queue_hit();
    medicon::sdk::LatencyReply reply;
    require(velocity->handle_latency(reply, game.current), "Runtime did not hold the reply behind KB");
    client.tick(0.01F);
    require(game.velocity_writes == 0, "Present applied delayed knockback outside the game thread");
    client.game_tick(game, 0.05F);
    require(game.velocity_writes == 1 && game.knockback_calls == 1 && velocity->pending_count() == 1 &&
            game.current.local.velocity.y == 4 && game.latency_sends == 0,
            "Client did not forward velocity or dispatched Delay twice in one tick");
    client.game_tick(game, 0.05F, false);
    require(game.velocity_writes == 1 && velocity->pending_count() == 0 && game.latency_sends == 1 && !velocity->needs_restore(),
            "Focus loss must discard KB and forward reply cleanup through the Client route");
    queue_hit();
    client.gui().set_open(true);
    client.game_tick(game, 0.05F);
    require(game.velocity_writes == 1 && velocity->pending_count() == 0, "ClickGUI did not discard pending knockback");
    client.gui().set_open(false);
    queue_hit();
    client.clear_gameplay();
    require(velocity->pending_count() == 0, "World snapshot reset retained delayed knockback");
    queue_hit();
    const auto final_rotation_count = game.head_body_rotations;
    game.current.ready = false;
    client.game_tick(game, 0.05F);
    require(game.rotations == 0 && game.head_body_rotations == final_rotation_count && velocity->pending_count() == 0 &&
            game.velocity_writes == 1, "Missing world suppresses gameplay actions and clears delayed knockback");
    game.current.ready = true;
    queue_hit();
    require(client.stop(), "Gameplay client stops");
    require(velocity->pending_count() == 0, "Client stop retained delayed knockback");
    game.current.ready = true;
    client.game_tick(game, 0.05F);
    require(game.rotations == 0 && game.head_body_rotations == final_rotation_count && game.velocity_writes == 1,
            "Stopped client cannot mutate the game");
}

void config_command_test(const std::filesystem::path& path) {
    medicon::Client client(path);
    require(client.start(), "Config client starts");
    auto* model_probe = client.modules().find("custommodel");
    auto* gui_module = client.modules().find("clickgui");
    require(model_probe && gui_module, "Config test modules are registered");
    model_probe->set_enabled(true);
    client.gui().set_open(true);
    gui_module->set_keybind(VK_F9);
    require(client.execute_command(".config save session").success, "Named config saves");
    gui_module->set_keybind(VK_F10);
    const auto loaded = client.execute_command(".config load session");
    require(loaded.success && loaded.changed && loaded.config_loaded, "Named config loads");
    require(gui_module->keybind() == VK_F9, "Load restores the saved bind");
    require(!client.gui().open() && !model_probe->enabled(), "Load applies the startup session rules");
    medicon::Client restored(path);
    require(restored.start(), "Second config client starts");
    require(restored.modules().find("clickgui")->keybind() == VK_F9, "Load persists to the active config");
    require(restored.stop() && client.stop(), "Config clients stop");
}

void client_test(const std::filesystem::path& path) {
    { std::ofstream config(path); config << R"({"version":1,"modules":{"clickgui":{"enabled":true},"custommodelprobe":{"enabled":true,"settings":{"material":1}}}})"; }
    medicon::Client client(path);
    require(!client.execute_command(".bind clickgui rshift").success, "Commands require an initialized client");
    require(client.start(), "Client starts");
    require(client.start(), "Start is idempotent");
    require(client.modules().find("example") != nullptr, "Built-in module registered");
    auto* model_probe = client.modules().find("custommodel");
    require(model_probe&&model_probe->id()=="custommodel"&&model_probe->name()=="CustomModel"&&client.modules().find("custommodelprobe")==model_probe,"CustomModel rename retains legacy command lookup");
    require(model_probe && !model_probe->enabled() && model_probe->keybind() == VK_F8, "P0 defaults OFF with F8 binding");
    auto* model_options = dynamic_cast<medicon::CustomModelProbeModule*>(model_probe);
    require(model_options&&model_options->model.name=="Model"&&model_options->model.get<medicon::Choice>().index==0&&
        model_options->model.get<medicon::Choice>().options==std::vector<std::string>{"Neon","Chocolat"},"Model selection is separate from material and preserves Neon as default");
    require(model_options&&model_options->material.name=="Material"&&model_options->material.get<medicon::Choice>().options[0]=="Neon"&&model_options->material.get<medicon::Choice>().options[1]=="entity"&&model_options->material.get<medicon::Choice>().index==1,"Material keeps its name, the alphatest choice displays Neon, and legacy selection is migrated");
    require(model_options && model_options->show_debug.name == "Show Debug" && !model_options->show_debug.get<bool>(),
            "Show Debug is a module option and defaults OFF");
    client.key_event(VK_F8, true);
    require(model_probe->enabled(), "F8 enables P0 through existing module dispatch");
    require(!model_options->show_debug.get<bool>(), "F8 never enables Show Debug");
    client.key_event(VK_F8, true, true);
    require(model_probe->enabled(), "F8 repeat does not toggle P0");
    client.key_event(VK_F8, false);
    client.key_event(VK_F8, true);
    client.key_event(VK_F8, false);
    require(!model_probe->enabled(), "F8 restores P0 OFF");
    model_options->show_debug.get<bool>() = true;
    require(!model_probe->enabled() && model_options->show_debug.get<bool>(), "Show Debug can be ON while suppression is OFF");
    client.key_event(VK_F8, true);
    client.key_event(VK_F8, false);
    require(model_probe->enabled() && model_options->show_debug.get<bool>(), "F8 preserves explicit Show Debug choice");
    client.key_event(VK_F8, true);
    client.key_event(VK_F8, false);
    model_options->show_debug.get<bool>() = false;
    require(!client.gui().open() && !client.capturing_input(), "Startup leaves ClickGUI closed and game input free");
    require(client.overlay().splash_active(), "Startup arms the injection splash");

    const auto remap = client.execute_command(".bind clickgui rshift");
    require(remap.handled && remap.changed && remap.success, "Bind command changes and persists the GUI key");
    {
        std::ifstream saved(path);const std::string text((std::istreambuf_iterator<char>(saved)),{});
        require(text.find("\"custommodel\"")!=std::string::npos&&text.find("\"custommodelprobe\"")==std::string::npos,"Saved config uses only the new CustomModel ID");
    }
    require(client.modules().find("clickgui")->keybind() == VK_RSHIFT, "GUI binding is shared with the module registry");
    client.key_event(VK_INSERT, true);
    require(!client.gui().open(), "Old GUI key cannot open the GUI after a bind command");
    client.key_event(VK_INSERT, false);
    client.key_event(VK_RSHIFT, true);
    require(client.gui().open(), "New GUI key opens GUI during the splash");
    client.key_event(VK_RSHIFT, true, true);
    require(client.gui().open(), "New GUI key still ignores repeat");
    client.key_event(VK_RSHIFT, false);
    client.key_event(VK_RSHIFT, true);
    require(!client.gui().open(), "New GUI key closes GUI immediately");
    client.key_event(VK_RSHIFT, false);
    client.key_event(VK_RSHIFT, true);
    require(client.gui().open(), "New GUI key reopens GUI immediately");
    client.key_event(VK_RSHIFT, false);
    const auto invalid_bind = client.execute_command(".bind clickgui no_such_key");
    require(invalid_bind.handled && !invalid_bind.success && !invalid_bind.changed &&
            client.modules().find("clickgui")->keybind() == VK_RSHIFT,
            "Invalid bind does not mutate the current GUI key");
    {
        medicon::Client loaded_while_running(path);
        require(loaded_while_running.start(), "Second client reads the immediately saved command configuration");
        require(client.running() && loaded_while_running.modules().find("clickgui")->keybind() == VK_RSHIFT,
                "Binding reaches disk before the first client stops");
        require(!loaded_while_running.gui().open(), "A new client starts with ClickGUI closed after loading configuration");
        require(loaded_while_running.stop(), "Second client stops after persistence check");
    }
    require(client.execute_command(".bind clickgui insert").success, "Restore Insert for lifecycle checks");
    client.key_event(VK_INSERT, true);
    require(!client.gui().open(), "Insert closes GUI");
    client.key_event(VK_INSERT, true, true);
    require(!client.gui().open(), "Repeat does not toggle GUI");
    client.key_event(VK_INSERT, false);
    client.key_event(VK_INSERT, true);
    require(client.gui().open(), "Insert opens GUI");
    client.key_event(VK_INSERT, false);
    client.key_event(VK_INSERT, true);
    client.key_event(VK_INSERT, false);
    require(!client.gui().open(), "Close GUI before checking splash completion");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    struct Context { ~Context() { ImGui::DestroyContext(); } } context;
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 800);
    io.DeltaTime = 1.0F / 60.0F;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    require(pixels && width > 0 && height > 0, "Build preview fonts");
    if (client.prepare_frame()) io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    const auto first_frame = std::chrono::steady_clock::now();
    for (int i = 0; i < 3; ++i) {
        if (client.prepare_frame()) io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        ImGui::NewFrame();
        client.tick(io.DeltaTime);
        client.render();
        ImGui::Render();
        require(ImGui::GetDrawData() != nullptr && ImGui::GetDrawData()->TotalVtxCount > 0,
                "Injection splash produces draw geometry");
    }
    require(client.overlay().splash_active(), "Splash remains active during its initial rendered frames");
    const auto splash_deadline = first_frame + std::chrono::duration<double>(medicon::gui::INJECT_SPLASH_DURATION + 0.25);
    while (std::chrono::steady_clock::now() < splash_deadline) {
        if (client.prepare_frame()) io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        ImGui::NewFrame();
        client.tick(io.DeltaTime);
        client.render();
        ImGui::Render();
        Sleep(16);
    }
    require(!client.overlay().splash_active() && !client.gui().open() && !client.capturing_input(),
            "Splash ends without opening ClickGUI or capturing game input");
    client.key_event(VK_INSERT, true);
    client.key_event(VK_INSERT, false);
    require(client.gui().open(), "The bound key opens ClickGUI after the splash");
    for (int i = 0; i < 3; ++i) {
        if (client.prepare_frame()) io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        ImGui::NewFrame();
        client.tick(io.DeltaTime);
        client.render();
        ImGui::Render();
        require(ImGui::GetDrawData() != nullptr && ImGui::GetDrawData()->TotalVtxCount > 0,
                "Explicitly opened ClickGUI produces draw geometry after the splash");
    }
    client.modules().find("example")->set_enabled(true);
    require(client.stop(), "Stop saves configuration");
    require(client.stop(), "Stop is idempotent");
    medicon::Client restored(path);
    require(restored.start() && restored.modules().find("example")->enabled(), "Enabled state survives shutdown");
    require(!restored.gui().open() && restored.overlay().splash_active(),
            "Restart resets saved ClickGUI enabled state and starts a fresh splash");
    require(restored.stop(), "Restored client stops");

    const std::string invalid = "{broken configuration";
    { std::ofstream file(path); file << invalid; }
    medicon::Client corrupt(path);
    require(corrupt.start() && !corrupt.last_error().empty(), "Malformed config keeps defaults and diagnostic");
    const auto session_only = corrupt.execute_command(".bind clickgui rshift");
    require(session_only.handled && session_only.changed && !session_only.success &&
            session_only.message.find("session") != std::string::npos &&
            session_only.message.find("invalid") != std::string::npos &&
            corrupt.modules().find("clickgui")->keybind() == VK_RSHIFT,
            "Malformed config permits a session bind with an explicit persistence warning");
    {
        std::ifstream protected_file(path);
        const std::string protected_contents((std::istreambuf_iterator<char>(protected_file)), {});
        require(protected_contents == invalid, "Command does not overwrite malformed config immediately");
    }
    require(corrupt.stop(), "Malformed config does not prevent shutdown");
    std::ifstream file(path);
    const std::string contents((std::istreambuf_iterator<char>(file)), {});
    require(contents == invalid, "Malformed config is not overwritten automatically");
}

void main_menu_input_test(const std::filesystem::path& path) {
    medicon::Client client(path);
    require(client.start(), "Main-menu input client starts");
    auto* example = client.modules().find("example");
    require(example != nullptr, "Main-menu input test module exists");
    require(client.execute_command(".bind example k").success, "Bind a module for menu input isolation");
    require(!client.overlay().main_menu().active() && !client.capturing_input(),
            "A new client does not infer the main menu or capture input");

    client.key_event('K', true);
    require(example->enabled(), "Module hotkey works before the main menu takes input");
    // Leave K held across the transition: entering/leaving the menu must reset
    // remembered hotkey edges, even when no release reaches the normal route.
    client.key_event(VK_INSERT, true);
    client.key_event(VK_INSERT, false);
    require(client.gui().open(), "Open ClickGUI before the menu ownership transition");
    client.overlay().set_main_menu_active(true);
    require(client.overlay().main_menu().active() && client.capturing_input() && !client.gui().open(),
            "Main menu exclusively owns input and closes a previously open ClickGUI");
    example->set_enabled(false);

    for (const int key : {static_cast<int>('K'), VK_INSERT, VK_ESCAPE}) {
        require(client.key_event(key, true), "Main menu consumes key presses");
        require(client.key_event(key, true, true), "Main menu consumes repeated key presses");
        require(client.key_event(key, false), "Main menu consumes key releases");
        require(client.overlay().main_menu().active() && client.capturing_input() &&
                !client.gui().open() && !example->enabled(),
                "Module/Insert/Escape keys cannot toggle modules, open ClickGUI or dismiss the main menu");
    }
    client.overlay().set_main_menu_active(true);
    require(client.overlay().main_menu().active() && client.capturing_input(),
            "Repeated active screen observations preserve menu ownership");

    // A held key has no release on the ordinary input route before exit.
    require(client.key_event('K', true), "Main menu consumes a key held during exit");
    client.overlay().set_main_menu_active(false);
    require(!client.overlay().main_menu().active() && !client.capturing_input() && !client.gui().open(),
            "Leaving the menu releases input without reopening ClickGUI");
    client.key_event('K', true);
    require(example->enabled(), "A fresh module hotkey works after leaving the menu");
    // This time K became held through the ordinary module route. No key-up is
    // delivered while crossing both menu transitions.
    client.overlay().set_main_menu_active(true);
    client.overlay().set_main_menu_active(false);
    example->set_enabled(false);
    client.key_event('K', true);
    require(example->enabled(), "Menu transitions clear keys held before entry without requiring a release");
    client.key_event('K', false);
    client.key_event(VK_INSERT, true);
    client.key_event(VK_INSERT, false);
    require(client.gui().open(), "ClickGUI hotkey works again after the menu releases input");
    require(client.execute_command(".bind example w").success, "Bind movement key to check GUI hotkey isolation");
    example->set_enabled(false);
    for (const int key : std::array<int, 7>{'W', 'A', 'S', 'D', VK_SPACE, VK_LSHIFT, VK_LCONTROL}) {
        require(!client.key_event(key, true) && !client.key_event(key, false),
                "ClickGUI must allow default game movement press/release");
        require(!example->enabled(), "ClickGUI movement toggled a module hotkey");
    }
    require(client.key_event('T', true) && client.key_event('T', false), "ClickGUI still blocks game chat keys");
    {
        ImGui::CreateContext();
        struct Context { ~Context() { ImGui::DestroyContext(); } } context;
        ImGui::GetIO().WantTextInput = true;
        require(client.key_event('W', true) && client.key_event('W', false),
                "ClickGUI text editing leaked movement to the game");
    }
    client.overlay().social().open_screen();
    require(client.capturing_input() && client.key_event('W', true) && client.key_event('W', false) &&
            client.key_event(VK_LBUTTON, true) && client.key_event(VK_LBUTTON, false),
            "Social must consume movement and attack input even over ClickGUI");
    client.overlay().social().close_screen();
    require(!client.key_event('W', true) && !client.key_event('W', false),
            "ClickGUI movement did not return after Social closed");

    client.overlay().set_main_menu_active(true);
    require(client.overlay().main_menu().active() && client.capturing_input() && !client.gui().open(),
            "Returning to the main menu reacquires input and hides ClickGUI");
    client.overlay().set_main_menu_active(false);
    client.overlay().set_main_menu_active(false);
    require(!client.overlay().main_menu().active() && !client.capturing_input(),
            "Repeated inactive observations leave game input free");
    client.overlay().set_main_menu_active(true);
    require(client.stop(), "Stop succeeds while the main menu owns input");
    require(!client.overlay().main_menu().active() && !client.capturing_input(),
            "Stop deactivates the menu and releases input ownership");
    require(client.start() && !client.overlay().main_menu().active() && !client.capturing_input(),
            "Restart waits for a new verified main-menu observation");
    require(client.stop(), "Restarted main-menu input client stops");
}
}

int main() {
    const auto directory = std::filesystem::temp_directory_path() / ("medicon-runtime-tests-" +
        std::to_string(GetCurrentProcessId()) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(directory);
        hooks_test();
        custom_model_hook_test();
        gameplay_dispatch_test(directory / "gameplay-config.json");
        client_test(directory / "config.json");
        config_command_test(directory / "config-commands" / "config.json");
        main_menu_input_test(directory / "main-menu-config.json");
        std::filesystem::remove_all(directory);
        std::cout << "Runtime, actual detour and GUI frame tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        return 1;
    }
}
