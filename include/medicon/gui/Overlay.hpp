#pragma once

#include "medicon/gui/ArrayList.hpp"
#include "medicon/gui/BlockCount.hpp"
#include "medicon/gui/ClickGui.hpp"
#include "medicon/gui/Clock.hpp"
#include "medicon/gui/DisconnectScreen.hpp"
#include "medicon/gui/Fonts.hpp"
#include "medicon/gui/GuiContext.hpp"
#include "medicon/gui/InjectSplash.hpp"
#include "medicon/gui/MainMenu.hpp"
#include "medicon/gui/Shadow.hpp"
#include "medicon/gui/SocialUi.hpp"
#include "medicon/gui/Watermark.hpp"

#include <functional>

namespace medicon {
class ModuleManager;
}

namespace medicon::gui {

// Everything the client draws on top of the game: the HUD (ArrayList and
// Watermark), then the ClickGUI, then Social notifications. The DLL and the Preview drive the same object:
//   backend NewFrame -> prepare_frame() [re-upload fonts if true] -> ImGui::NewFrame
//   -> render() -> ImGui::Render
class Overlay {
public:
    explicit Overlay(ModuleManager& modules);

    // Before ImGui::NewFrame(), after the platform backend set DisplaySize.
    // Returns true when the font atlas was rebuilt and its texture must be re-uploaded.
    bool prepare_frame();
    void render();
    void start_splash() noexcept { splash_.start(); }
    bool splash_active() const noexcept { return splash_.active(); }
    void set_main_menu_active(bool active) noexcept;
    MainMenu& main_menu() noexcept { return main_menu_; }
    const MainMenu& main_menu() const noexcept { return main_menu_; }
    DisconnectScreen& disconnect_screen() noexcept { return disconnect_; }
    const DisconnectScreen& disconnect_screen() const noexcept { return disconnect_; }
    void set_disconnect_state(bool visible,bool available,std::uint64_t generation,std::string error = {});
    // Opens the Social screen in game (VK_HOME).
    static constexpr int social_key = 0x24;
    // Routes a Win32 virtual-key event: GUI first (open key, Esc, keybind capture),
    // then module hotkeys. Returns true when the key must not reach the game.
    bool key_event(int virtual_key, bool down, bool repeat);
    [[nodiscard]] bool allows_game_movement() const noexcept;
    // WM_CHAR for Social text inputs, delivered in order with key_event.
    void char_event(wchar_t character) { social_.char_event(character); }

    ClickGui& gui() noexcept { return gui_; }
    const ClickGui& gui() const noexcept { return gui_; }
    ArrayList& hud() noexcept { return hud_; }
    FontManager& fonts() noexcept { return fonts_; }
    FrameClock& clock() noexcept { return clock_; }
    MeshShadowRenderer& shadows() noexcept { return shadows_; }
    SocialUi& social() noexcept { return social_; }
    const SocialUi& social() const noexcept { return social_; }
    const FrameInfo& frame() const noexcept { return frame_; }
    void set_script_ui_focused(bool focused) noexcept { script_ui_focused_=focused; }
    // The client draws its own cursor: the Medicon menu, the ClickGUI or the
    // Social screen is open. Everything else counts as playing for Social (see SocialUi).
    bool in_menu() const noexcept { return main_menu_.active() || disconnect_.visible() || gui_.open() || social_.screen_open(); }

    void set_save_handler(std::function<void()> handler) { save_ = std::move(handler); }
    // Connects Social. The channel belongs to the network service and must outlive this overlay.
    void set_social(social::Channel* channel, std::function<void()> play_sound) {
        social_.attach(channel, std::move(play_sound));
    }
    // Connects the AltManager. The channel must outlive this overlay.
    void set_alts(alt::Channel* channel) noexcept { social_.attach_alts(channel); }
    void set_alt_choice_handler(std::function<void(bool)> handler) { alt_choice_handler_=std::move(handler); }
    void set_texture_host(TextureHost* host) noexcept { social_.set_texture_host(host); }
    // The host's IME bridge for Social text inputs (may be null). Must outlive this overlay.
    void set_ime(ImeBridge* ime) noexcept { social_.set_ime(ime); }
    // 0 = query the host window (GetDpiForWindow on ImGui's main viewport).
    void set_dpi_override(float dpi) { dpi_override_ = dpi; }
    void set_owns_font_atlas(bool owns) { fonts_.set_owns_atlas(owns); }
    // The version the Watermark shows (from the update server, via the loader).
    void set_client_version(std::string_view version) { watermark_.set_version(version); }

private:
    FrameInfo compute_frame() const;
    FontSizes font_sizes(const FrameInfo& frame) const;
    void collect_text();

    ModuleManager& modules_;
    ClickGui gui_;
    ArrayList hud_;
    Watermark watermark_;
    BlockCount block_count_;
    FontManager fonts_;
    MeshShadowRenderer shadows_;
    FrameClock clock_;
    InjectSplash splash_;
    MainMenu main_menu_;
    DisconnectScreen disconnect_;
    SocialUi social_;
    FrameInfo frame_{};
    FontSizes prepared_sizes_{};
    float dpi_override_ = 0.0F;
    bool sizes_ready_ = false;
    bool script_ui_focused_ = false;
    // Glyph discovery is frozen after the first splash prepare. A suffix that
    // appears mid-intro must not rebuild the atlas while the last frames play.
    bool splash_glyphs_sealed_ = false;
    std::function<void()> save_;
    std::function<void(bool)> alt_choice_handler_;
};

} // namespace medicon::gui
