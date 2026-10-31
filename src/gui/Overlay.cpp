#include <medicon/gui/Overlay.hpp>
#include <medicon/platform/OverlayInput.hpp>

#include <medicon/core/ModuleManager.hpp>
#include <medicon/gui/GuiMath.hpp>
#include <medicon/gui/Draw.hpp>
#include <medicon/gui/KeyNames.hpp>
#include <medicon/gui/Theme.hpp>
#include <medicon/modules/ClientModules.hpp>

#include <Windows.h>

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace medicon::gui {

PointerInput read_pointer_input() {
    PointerInput input;
    const auto& io = ImGui::GetIO();
    input.position = io.MousePos;
    input.valid = ImGui::IsMousePosValid(&io.MousePos);
    for (std::size_t i = 0; i < input.buttons.size(); ++i) {
        input.buttons[i].down = io.MouseDown[i];
        input.buttons[i].pressed = io.MouseClicked[i];
        input.buttons[i].released = io.MouseReleased[i];
    }
    input.wheel = io.MouseWheel;
    input.owned_by_imgui = io.WantCaptureMouse;
    return input;
}

namespace {
float window_dpi() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    HWND window = viewport ? static_cast<HWND>(viewport->PlatformHandleRaw) : nullptr;
    if (!window) return 96.0F;
    const auto& io = ImGui::GetIO();
    static HWND cached_window{};
    static float cached_w{}, cached_h{}, cached_dpi = 96.0F;
    // GetDpiForWindow takes the USER lock. Present must not query it every frame
    // while the window thread is warping the hardware cursor.
    if (window == cached_window && cached_w == io.DisplaySize.x && cached_h == io.DisplaySize.y &&
        cached_dpi > 0.0F)
        return cached_dpi;
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static const auto get_dpi = reinterpret_cast<GetDpiForWindowFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    const UINT dpi = get_dpi ? get_dpi(window) : 96U;
    cached_window = window;
    cached_w = io.DisplaySize.x;
    cached_h = io.DisplaySize.y;
    cached_dpi = dpi > 0 ? static_cast<float>(dpi) : 96.0F;
    return cached_dpi;
}
} // namespace

Overlay::Overlay(ModuleManager& modules) : modules_(modules), gui_(modules) {}

void Overlay::set_main_menu_active(bool active) noexcept {
    if (active == main_menu_.active()) return;
    modules_.reset_keys();
    // The title screen owns input exclusively; closing it must not reveal a
    // ClickGUI that was opened behind the opaque replacement.
    if (active) gui_.set_open(false);
    else social_.close_screen();
    main_menu_.set_active(active);
}

bool Overlay::allows_game_movement() const noexcept {
    return gui_.open() && !main_menu_.active() && !disconnect_.visible() && !social_.screen_visible() && !social_.screen_open() &&
        !gui_.capturing_keybind() && (!ImGui::GetCurrentContext() || !ImGui::GetIO().WantTextInput);
}

void Overlay::set_disconnect_state(bool visible,bool available,std::uint64_t generation,std::string error) {
    if (disconnect_.visible() && (!visible || disconnect_.generation()!=generation)) social_.cancel_alt_choice();
    if (visible && !disconnect_.visible()) { gui_.set_open(false); modules_.reset_keys(); }
    disconnect_.set_state(visible,available,generation,std::move(error));
}

bool Overlay::key_event(int virtual_key, bool down, bool repeat) {
    if (social_.screen_open() || social_.screen_visible()) {
        social_.key_event(virtual_key, down);
        if (!down) modules_.key_event(virtual_key, false, repeat);
        return true;
    }
    if (main_menu_.active()) {
        if (!down) modules_.key_event(virtual_key, false, repeat);
        return true;
    }
    if (disconnect_.visible()) return false;
    // Home opens Social in game, over the ClickGUI or without it. A keybind
    // being captured takes Home like any other key.
    if (virtual_key == social_key && down && social_.attached() && !gui_.capturing_keybind()) {
        if (!repeat) {
            modules_.reset_keys();
            social_.open_screen();
        }
        return true;
    }
    const bool was_open = gui_.open();
    const bool capturing_bind = gui_.capturing_keybind();
    const bool owned = gui_.key_event(virtual_key, down, repeat);
    const bool toggled = was_open != gui_.open();
    if (toggled) modules_.reset_keys();
    // Releases always reach the hotkey state so held keys never stick. The
    // open/close key itself never toggles a module.
    if (!down || (!owned && !toggled && !gui_.open())) modules_.key_event(virtual_key, down, repeat);
    const auto* clickgui = modules_.find(ClickGuiModule::module_id);
    const int open_key = clickgui && clickgui->keybind() ? clickgui->keybind() : VK_INSERT;
    if (!toggled && !capturing_bind && virtual_key != open_key && allows_game_movement() &&
        platform::clickgui_movement_key(virtual_key)) return false;
    return owned;
}

FrameInfo Overlay::compute_frame() const {
    FrameInfo frame;
    const auto& io = ImGui::GetIO();
    frame.client_w = std::max(1.0F, io.DisplaySize.x);
    frame.client_h = std::max(1.0F, io.DisplaySize.y);
    frame.dpi = dpi_override_ > 0.0F ? dpi_override_ : window_dpi();
    frame.auto_scale = auto_scale(frame.client_w, frame.client_h, frame.dpi);
    frame.hud_scale = clamp_scale(frame.auto_scale);
    frame.ui_scale = clamp_scale(frame.auto_scale * gui_.effective_gui_scale());
    return frame;
}

FontSizes Overlay::font_sizes(const FrameInfo& frame) const {
    using namespace theme::metrics;
    const float s = frame.ui_scale;
    FontSizes sizes;
    sizes.header = std::round(font_header * s);
    sizes.module = std::round(font_module * s);
    sizes.key_hint = std::round(font_key_hint * s);
    sizes.label = std::round(font_label * s);
    sizes.value = std::round(font_value * s);
    float arraylist_scale = 1.0F;
    if (const auto* module = dynamic_cast<const ArrayListModule*>(modules_.find(ArrayListModule::module_id))) {
        arraylist_scale = std::clamp(module->scale.get<float>(), 0.5F, 2.0F);
    }
    // Edge-lit sizes are physical pixels, independent of ClickGUI/DPI scaling.
    sizes.arraylist = 15.0F * arraylist_scale;
    sizes.arraylist_mode = 12.0F * arraylist_scale;
    // The Watermark follows the HUD scale (resolution and DPI) and its own Scale.
    // Kept while it fades out so the text never drops out of the atlas mid-fade.
    if (const auto* module = dynamic_cast<const WatermarkModule*>(modules_.find(WatermarkModule::module_id));
        module && (module->enabled() || watermark_.visible())) {
        const float k = frame.hud_scale * std::clamp(module->scale.get<float>(), 0.75F, 1.5F);
        sizes.watermark = std::round(theme::watermark::font * k);
        sizes.watermark_small = std::round(theme::watermark::font_small * k);
        if (module->japanese()) sizes.watermark_japanese = std::round(theme::watermark::title_font * k);
    }
    // Always in the atlas: a rebuild waits for the GPU, and Scaffold is turned on
    // in the middle of a jump.
    sizes.block_count = std::round(19.0F * frame.hud_scale);
    // Prepare the intro and menu together before the first intro render starts
    // its clock. Otherwise the handoff rebuilds the atlas and waits for the GPU
    // while the last animation frames should be presented.
    sizes.splash = 0.0F;
    const bool splash_active = splash_.active();
    if (splash_active) {
        // Rasterize the four title glyphs at twice the normal size so the pop stays
        // sharp. Draw geometry still follows the current window's proportions.
        // A detected main menu waits until the intro finishes, so its font
        // request must not replace the splash face partway through the animation.
        sizes.splash = std::clamp(std::round(std::min(frame.client_w, frame.client_h) * 0.13F * 2.0F), 32.0F, 512.0F);
    }
    if (splash_active || main_menu_.active()) {
        // Logo/button raster sizes depend only on the window size, not on the
        // animated blob phase. Avoid a full sinusoidal evaluation every frame.
        // Prewarm even before native menu detection, which can arrive late.
        main_menu_text_sizes({frame.client_w, frame.client_h}, sizes.main_menu_logo, sizes.main_menu_button);
    }
    return sizes;
}

void Overlay::collect_text() {
    for (std::size_t i = 0; i < category_count; ++i) fonts_.add_text(category_name(static_cast<Category>(i)));
    fonts_.add_text("Keybind[...]#");
    fonts_.add_text("Medicon Play Settings Quit Social AltManager");
    fonts_.add_text("Reconnect Switch Account Switch Account and Reconnect No saved server. Join from Play.");
    fonts_.add_text(watermark_.version());
    fonts_.add_text("0123456789 FPS ms");
    for (const auto& module : modules_.all()) {
        fonts_.add_text(module->name());
        fonts_.add_text(module->suffix());
        fonts_.add_text(key_name(module->keybind()));
        for (const auto& setting : module->settings()) {
            if (!setting.visible) continue;
            fonts_.add_text(setting.name);
            if (const auto* choice = std::get_if<Choice>(&setting.value)) {
                for (const auto& option : choice->options) fonts_.add_text(option);
            } else if (const auto* text = std::get_if<std::string>(&setting.value)) {
                fonts_.add_text(*text);
            }
        }
    }
}

bool Overlay::prepare_frame() {
    frame_ = compute_frame();
    const bool splash = splash_.active();
    if (!splash) splash_glyphs_sealed_ = false;
    // Social events wait in the channel during the intro: their glyphs would
    // rebuild the atlas while the animation plays (see splash_glyphs_sealed_).
    if (!splash) social_.pump(fonts_, in_menu());
    if (const auto chosen = social_.consume_alt_choice(); chosen) {
        if (alt_choice_handler_) alt_choice_handler_(*chosen);
        if (*chosen) disconnect_.reconnect_after_choice();
    }
    // Collect once up front, then again only after the intro. Walking every
    // module on the later splash frames can grow the atlas and stall Present
    // exactly while the collapse should be on screen.
    if (!splash_glyphs_sealed_) {
        collect_text();
        if (splash) splash_glyphs_sealed_ = true;
    }
    prepared_sizes_ = font_sizes(frame_);
    sizes_ready_ = true;
    return fonts_.prepare(*ImGui::GetIO().Fonts, prepared_sizes_);
}

void Overlay::render() {
    frame_ = compute_frame();
    frame_.dt = clock_.tick();
    if (!sizes_ready_) prepared_sizes_ = font_sizes(frame_);
    fonts_.set_sizes(prepared_sizes_);
    sizes_ready_ = false;
    // Keep the intro on top for its full duration. A detected title screen is
    // drawn underneath it, so the collapsing intro reveals the Medicon menu
    // instead of the vanilla screen. The hidden menu gets no pointer: a click
    // on the intro must not press a button behind it, and the menu requires a
    // neutral pointer frame before it accepts presses after the intro.
    if (splash_.active()) {
        auto* list = ImGui::GetForegroundDrawList();
        if (main_menu_.active()) main_menu_.render(list, fonts_, {frame_.client_w, frame_.client_h}, PointerInput{});
        splash_.render(list, fonts_, {frame_.client_w, frame_.client_h});
        return;
    }
    // After the intro: a client without a working key shows only the sign-in panel.
    social_.update_sign_in_gate();
    const PointerInput pointer = read_pointer_input();
    if (const auto* settings = dynamic_cast<const ClickGuiModule*>(modules_.find(ClickGuiModule::module_id)))
        social_.set_input_animation(settings->input_animation.get<bool>());
    // Nothing under the Social screen takes the pointer while it is up, and a
    // fully open screen covers everything, so the rest is not drawn at all.
    // Notifications and the music widget sit above the ClickGUI, top centre included.
    const bool notice_pointer = in_menu() && pointer.valid &&
        (social_.music().captures_pointer(pointer) || social_.toast_covers(pointer.position));
    const PointerInput below = social_.screen_visible() || notice_pointer ? PointerInput{} : pointer;
    social_.set_backdrop_time(main_menu_.elapsed());
    const bool covered = social_.screen_opaque();
    if (main_menu_.active() || covered) watermark_.hide();
    // Opened from the ClickGUI, the Social screen keeps its button: it shows as
    // selected above the screen and closes it. The sign-in gate cannot be closed.
    const bool button_above_screen = social_.screen_visible() && gui_.open() && !main_menu_.active() &&
        !disconnect_.visible() && !social_.sign_in_gate_active();
    gui_.set_social({social_.attached(), social_.badge(), social_.screen_open(), button_above_screen});
    if (main_menu_.active()) {
        main_menu_.set_social(social_.attached(), social_.badge());
        main_menu_.set_alts(social_.alts_attached());
        if (!covered) main_menu_.render(ImGui::GetForegroundDrawList(), fonts_, {frame_.client_w, frame_.client_h}, below);
        if (main_menu_.consume_social_request()) social_.open_screen();
        if (main_menu_.consume_alts_request()) social_.open_alts();
    } else if (disconnect_.visible()) {
        watermark_.hide();
        if (!covered) disconnect_.render(ImGui::GetForegroundDrawList(),fonts_,shadows_,frame_,below,social_.alts_attached());
        if (const auto action=disconnect_.consume_action())
            social_.choose_alt(*action==DisconnectAction::SwitchAccountAndReconnect);
    } else if (!covered) {
        GuiContext context{modules_, fonts_, shadows_, frame_, below, ImGui::GetBackgroundDrawList()};
        hud_.render(context);
        watermark_.render(context, clock_.elapsed());
        block_count_.render(context);
        gui_.render(context);
        if (gui_.consume_save_request() && save_) save_();
        if (gui_.consume_social_request()) social_.open_screen();
    }
    // Last on the foreground list: above the menu and the ClickGUI (spec 4.7).
    // TODO(social): the pause screen and other vanilla menus cannot be told apart
    // from playing yet, so in_menu() treats them as playing (3 s, in-game offset).
    FrameInfo social_frame = frame_;
    social_frame.dt = clock_.elapsed();
    GuiContext button{modules_, fonts_, shadows_, frame_, notice_pointer ? PointerInput{} : pointer, ImGui::GetForegroundDrawList()};
    bool button_owns_pointer = false;
    if (button_above_screen) {
        button_owns_pointer = gui_.social_button_input(button);
        if (gui_.consume_social_request()) {
            if (social_.screen_open()) social_.close_screen();
            else social_.open_screen();
        }
    }
    const auto draw_button = [&] { gui_.draw_social_button(button); };
    // In a menu the screen's font and blurred backdrop are prepared ahead, on
    // their worker threads, so its first frame is already complete.
    social_.render(ImGui::GetForegroundDrawList(), shadows_, social_frame, button_owns_pointer ? PointerInput{} : pointer,
                   in_menu(), in_menu(), button_above_screen ? draw_button : std::function<void()>{});
}

} // namespace medicon::gui
