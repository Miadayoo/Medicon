#pragma once
namespace medicon::scripts { class ScriptManager; }
namespace medicon { class ClickGui; }
namespace medicon::gui {
class FontManager;
class Overlay;
void render_script_screen(scripts::ScriptManager& manager);
void attach_script_gui(scripts::ScriptManager& manager, ClickGui& gui, FontManager* fonts = nullptr,Overlay* overlay=nullptr);
}
