#pragma once
namespace medicon::scripts { struct HostServices; }
namespace medicon::gui {
class FontManager;
class Overlay;
void attach_script_glass_bridge(scripts::HostServices& host,FontManager* fonts,Overlay* overlay);
}
