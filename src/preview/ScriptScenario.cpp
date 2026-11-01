#include "Scenarios.hpp"
#include <medicon/gui/Overlay.hpp>
#include <medicon/gui/ScriptScreen.hpp>
#include <medicon/gui/Theme.hpp>
#include <medicon/gui/GuiMath.hpp>
#include <medicon/gui/Accessibility.hpp>
#include <medicon/gui/LiquidGlass.hpp>
#include <medicon/modules/ScriptManagerModule.hpp>
#include <medicon/scripts/ScriptManager.hpp>
#include <algorithm>
#include <stdexcept>

namespace medicon::preview {
void run_script_scenario(ScenarioHost& host, bool capture) {
    using namespace scripts;
    gui::override_accessibility_preferences(gui::AccessibilityPreferences{});
    const auto root = fs::temp_directory_path() / ("medicon-script-preview-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    ScriptPaths paths(root); paths.initialize();
    fs::copy_file(paths.sdk / "examples" / "HelloWorld.cpp", paths.sources / "HelloWorld.cpp");
    fs::copy_file(paths.sdk / "examples" / "Settings.cpp", paths.sources / "Settings.cpp");
    for(const auto* name:{"GlassHUD","GlassQuickPanel","GlassFallback"}) fs::copy_file(paths.sdk/"examples"/(std::string(name)+".cpp"),paths.sources/(std::string(name)+".cpp"));
    ScriptManager manager(host.modules(), root / "config.json");
    struct Cleanup {
        ScriptManager& manager; ScenarioHost& host; fs::path root;
        ~Cleanup() {
            host.set_frame_hook({});host.set_ui_hook({}); manager.stop();
            host.overlay().gui().set_module_settings_renderer(ScriptManagerModule::module_id, {});
            gui::override_accessibility_preferences(std::nullopt);
            std::error_code error;
            if (ScriptPaths::contains(fs::temp_directory_path(), root)) fs::remove_all(root, error);
        }
    } cleanup{manager, host, root};
    gui::attach_script_gui(manager, host.overlay().gui(), &host.overlay().fonts(),&host.overlay());
    manager.host().features |= MEDICON_FEATURE_CHAT;
    bool greeting = false;
    manager.host().chat = [&](std::string_view message) { greeting = greeting || message == "Hello, Medicon!"; host.log("Script local chat fixture: " + std::string(message)); return true; };
    bool hud_submitted=false,ui_submitted=false;
    manager.start(); host.set_frame_hook([&] { manager.pump(); });host.set_ui_hook([&]{
        manager.render_ui();for(const auto& pane:gui::current_liquid_glass_frames()) {
            hud_submitted=hud_submitted||pane.owner=="glass_hud";ui_submitted=ui_submitted||pane.owner=="glass_quick_panel";
        }
    });
    const auto wait = [&](const auto& predicate) {
        // Five native modules may each need a second pass to verify newly discovered includes.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        while (std::chrono::steady_clock::now() < deadline) {
            host.frame(); const auto s = manager.snapshot();
            if (predicate(s)) return s;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const auto s = manager.snapshot();
        for (const auto& v : s.scripts) { host.log(v.status); for (const auto& d : v.diagnostics) host.log(d.message); }
        throw std::runtime_error("Script preview timed out: " + s.error);
    };
    auto& gui = host.overlay().gui();
    gui.set_open(true); gui.set_module_expanded(ScriptManagerModule::module_id, true);
    wait([](const ScriptStatus& s) { return std::ranges::any_of(s.scripts, [](const auto& script) { return script.progress.stage == BuildStage::compiling; }); });
    gui.finish_animations(); host.frame();
    if (capture) host.capture("building");
    gui.set_module_expanded(ScriptManagerModule::module_id, false); gui.set_open(false);
    const auto built = wait([](const ScriptStatus& s) { return s.scripts.size() == 5 && std::ranges::all_of(s.scripts, &ScriptView::approval_required); });
    // These are the SDK's own known sample sources in a fresh temporary directory.
    for (const auto& script : built.scripts) manager.request_approval(script.key, script.binary_hash);
    wait([](const ScriptStatus& s) { return s.scripts.size() == 5 && std::ranges::all_of(s.scripts, &ScriptView::active); });
    auto* hello = host.modules().find("hello"); auto* settings = host.modules().find("settings_example");
    if (!hello || !settings || hello->category() != Category::Scripts || settings->category() != Category::Scripts)
        throw std::runtime_error("Compiled scripts must register in the Scripts category");
    hello->set_enabled(true); settings->set_enabled(true);
    if (!greeting) throw std::runtime_error("Script enable did not reach the local chat bridge");
    gui.set_open(true);
    for (int i = 0; i < 100; ++i) host.frame();
    gui.finish_animations(); host.frame();
    const auto panels = gui.panel_rects();
    if (std::ranges::none_of(panels, [](const auto& p) { return p.category == Category::Scripts; }))
        throw std::runtime_error("Scripts panel missing");
    const auto rect = *std::ranges::find(panels, Category::Scripts, &ClickGui::PanelRect::category);
    int row = 0;
    for (const auto& module : host.modules().all()) { if (module.get() == hello) break; if (module->category() == Category::Scripts) ++row; }
    const auto scale = gui.ui_scale();
    const auto x = rect.x + gui::px(theme::metrics::panel_width * .4F, scale);
    const auto y = rect.y + gui::px(theme::metrics::header_height, scale) + gui::px(theme::metrics::separator, scale) + gui::px(theme::metrics::module_height * (static_cast<float>(row) + .5F), scale);
    const auto point = MAKELPARAM(static_cast<short>(std::lround(x)), static_cast<short>(std::lround(y)));
    const auto click = [&](UINT down, UINT up, WPARAM flag) {
        host.send(WM_MOUSEMOVE, 0, point); host.frame(); host.send(down, flag, point); host.frame(); host.send(up, 0, point); host.frame();
    };
    click(WM_LBUTTONDOWN, WM_LBUTTONUP, MK_LBUTTON);
    if (hello->enabled()) throw std::runtime_error("Script row click must disable the module");
    click(WM_LBUTTONDOWN, WM_LBUTTONUP, MK_LBUTTON);
    if (!hello->enabled()) throw std::runtime_error("Script row click must enable the module");
    click(WM_MBUTTONDOWN, WM_MBUTTONUP, MK_MBUTTON);
    if (!gui.capturing_keybind()) throw std::runtime_error("Script row must support native keybind capture");
    host.send(WM_KEYDOWN, 'J', 1); host.frame(); host.send(WM_KEYUP, 'J', static_cast<LPARAM>(0xC0000001)); host.frame();
    if (hello->keybind() != 'J') throw std::runtime_error("Script keybind was not assigned");
    gui.set_open(false); host.frame();
    host.send(WM_KEYDOWN, 'J', 1); host.frame(); host.send(WM_KEYUP, 'J', static_cast<LPARAM>(0xC0000001)); host.frame();
    if (hello->enabled()) throw std::runtime_error("Script hotkey must use ModuleManager dispatch");
    gui.set_open(true); hello->set_enabled(true);
    host.send(WM_MOUSEMOVE, 0, MAKELPARAM(static_cast<short>(-100), static_cast<short>(-100)));
    for (int i = 0; i < 50; ++i) host.frame();
    host.log("Script row clicks, native keybind capture, and ModuleManager hotkeys passed");
    host.log("Scripts panel: compiled Hello World and Settings Example registered and enabled");
    if (capture) host.capture("list");
    gui.set_module_expanded("settings_example", true);
    for (int i = 0; i < 30; ++i) host.frame();
    gui.finish_animations(); host.frame();
    if (capture) host.capture("settings");
    gui.set_module_expanded("settings_example", false);
    gui.set_module_expanded(ScriptManagerModule::module_id, true);
    for (int i = 0; i < 20; ++i) host.frame();
    if (capture) host.capture("manager");
    gui.set_module_expanded(ScriptManagerModule::module_id,false);
    for(const auto* id:{"glass_hud","glass_quick_panel","glass_fallback"}) host.modules().find(id)->set_enabled(true);
    for(int i=0;i<60;++i)host.frame();
    const auto extended=manager.snapshot();
    const auto fallback=std::ranges::find(extended.scripts,std::string("glass_fallback"),&ScriptView::id);
    if(fallback==extended.scripts.end()||std::ranges::none_of(fallback->diagnostics,[](const auto& d){return d.code=="GLASS_UNSUPPORTED";})) throw std::runtime_error("Missing unsupported style diagnostic");
    if(!(extended.features&MEDICON_FEATURE_GLASS)) throw std::runtime_error("Preview native Glass device was not ready");
    if(!hud_submitted||!ui_submitted)throw std::runtime_error("Glass HUD and UI must both reach the existing native publisher");
    if(capture)host.capture("glass");
    host.recreate_graphics();for(int i=0;i<15;++i)host.frame();
    if(!(manager.snapshot().features&MEDICON_FEATURE_GLASS))throw std::runtime_error("Glass capability did not recover after graphics recreation");
    host.log("Compiled native Glass HUD, interactive UI, fallback diagnostic and graphics recreation passed");
}
} // namespace medicon::preview
