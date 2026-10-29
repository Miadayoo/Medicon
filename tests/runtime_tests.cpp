#include <medicon/client/Client.hpp>
#include <medicon/gui/InjectSplash.hpp>
#include <medicon/sdk/CustomModelProbe.hpp>
#include <medicon/modules/CustomModelProbeModule.hpp>
#include <Windows.h>
#include <imgui.h>
#include <chrono>
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

namespace native_model = medicon::sdk::custom_model::bedrock_1_26_52_3;
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
    struct Game final : medicon::sdk::GameplayAccess {
        medicon::sdk::GameplayFrame current;
        int rotations = 0;
        const medicon::sdk::GameplayFrame& frame() const noexcept override { return current; }
        bool rotate(float, float) override { ++rotations; return true; }
        bool attack(std::uintptr_t) override { throw std::runtime_error("Unexpected attack"); }
        bool place_block(medicon::sdk::BlockPos, bool) override { throw std::runtime_error("Unexpected placement"); }
        bool replaceable(medicon::sdk::BlockPos) const override { return false; }
    } game;
    game.current.ready = true;
    game.current.local.id = 1;
    game.current.local.alive = true;
    game.current.local.local = true;
    medicon::Client client(path);
    require(client.start(), "Gameplay client starts");
    for (const auto* id : {"chinahat", "derp", "aimbot", "killaura", "scaffold", "esp", "custommodel"})
        require(client.modules().find(id) && !client.modules().find(id)->enabled(), "Gameplay module registered disabled");
    client.game_tick(game, 0.05F);
    require(game.rotations == 0, "Disabled gameplay modules do not act");
    client.modules().find("derp")->set_enabled(true);
    client.tick(0.05F);
    require(game.rotations == 0, "Present tick never mutates the game");
    client.game_tick(game, 0.05F);
    require(game.rotations == 1, "Native game tick dispatches Derp");
    client.game_tick(game, 0.05F, false);
    require(game.rotations == 1, "Unfocused or inventory input suppresses actions");
    client.gui().set_open(true);
    client.game_tick(game, 0.05F);
    require(game.rotations == 1, "ClickGUI suppresses gameplay actions");
    client.gui().set_open(false);
    game.current.ready = false;
    client.game_tick(game, 0.05F);
    require(game.rotations == 1, "Missing world suppresses gameplay actions");
    require(client.stop(), "Gameplay client stops");
    game.current.ready = true;
    client.game_tick(game, 0.05F);
    require(game.rotations == 1, "Stopped client cannot mutate the game");
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

