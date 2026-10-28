#include "medicon/modules/ClientModules.hpp"
#include "medicon/gui/Theme.hpp"

#include <string>

namespace medicon {

namespace {
constexpr float position_limit = 100000.0F;
} // namespace

ClickGuiModule::ClickGuiModule()
    : Module(module_id, "ClickGUI", "Dropdown client GUI. Toggle to open or close.", Category::Client, default_key),
      scale(add_setting({"scale", "GUI Scale", "Multiplier on the automatic UI scale.", 1.0F, 0.5F, 2.0F, 0.05F})),
      panel_shadow(add_setting({"shadow.enabled", "Panel Shadow", "Drop shadow under panels.", true})),
      shadow_opacity(add_setting({"shadow.opacity", "Shadow Opacity", "", theme::shadow_panel.opacity, 0.0F, 1.0F, 0.01F})),
      shadow_blur(add_setting({"shadow.blur", "Shadow Blur", "CSS blur radius in reference pixels.", theme::shadow_panel.blur, 0.0F, 32.0F, 1.0F})),
      shadow_offset_y(add_setting({"shadow.offset_y", "Shadow Offset Y", "", theme::shadow_panel.offset_y, -16.0F, 16.0F, 1.0F})),
      panel_order(add_setting({"panels.order", "Panel order", "", std::string{}, 0.0F, 1.0F, 1.0F, false})),
      input_animation(add_setting({"input.animation", "Input Animation",
                                   "Typed characters dip slightly and fade in to their colour.", true})) {
    set_show_in_arraylist(false);
    for (std::size_t index = 0; index < category_count; ++index) {
        const std::string prefix = std::string("panels.") + category_id(static_cast<Category>(index));
        auto& panel = panels_[index];
        panel.x = &add_setting({prefix + ".x", "", "", 0.0F, -position_limit, position_limit, 1.0F, false});
        panel.y = &add_setting({prefix + ".y", "", "", 0.0F, -position_limit, position_limit, 1.0F, false});
        panel.collapsed = &add_setting({prefix + ".collapsed", "", "", false, 0.0F, 1.0F, 1.0F, false});
    }
}

ArrayListModule::ArrayListModule()
    : Module(module_id, "ArrayList", "Edge-lit module plates illuminated by a pink-to-violet tube.", Category::Client),
      accent_a(add_setting({"accentA", "Accent A", "Tube and reflected light at the top of the list.", Color{1.0F, 79.0F / 255.0F, 160.0F / 255.0F, 1.0F}})),
      accent_b(add_setting({"accentB", "Accent B", "Tube and reflected light at the bottom of the list.", Color{139.0F / 255.0F, 107.0F / 255.0F, 1.0F, 1.0F}})),
      glow(add_setting({"glow", "Glow", "Multiplier for reflected light, bloom, text glow, tube glow, and pulses.", 1.0F, 0.0F, 1.6F, 0.05F})),
      shadow(add_setting({"shadow", "Shadow", "Multiplier for the soft cast shadow and text contact shadows.", 1.0F, 0.0F, 1.6F, 0.05F})),
      pulse(add_setting({"pulse", "Pulse", "Send light along the tube when a module is toggled.", true})),
      scale(add_setting({"scale", "Scale", "Scale all Edge-lit dimensions in physical screen pixels.", 1.0F, 0.5F, 2.0F, 0.05F})) {
    set_show_in_arraylist(false);
    set_enabled(true);
}

WatermarkModule::WatermarkModule()
    : Module(module_id, "Watermark", "Medicon tag at the top left while playing.", Category::Client),
      show_version(add_setting({"version", "Show Version", "Client version after the name.", true})),
      show_fps(add_setting({"fps", "Show FPS", "Frames per second, updated twice a second.", false})),
      show_ping(add_setting({"ping", "Show Ping", "Server latency, when the client can read it.", false})),
      shadow(add_setting({"shadow", "Shadow", "Soft blue shadow under the plate.", true})),
      glow(add_setting({"glow", "Glow", "Pale blue light around the plate and its accent line.", true})),
      scale(add_setting({"scale", "Scale", "Multiplier on the HUD scale.", 1.0F, 0.75F, 1.5F, 0.05F})),
      opacity(add_setting({"opacity", "Opacity", "", 1.0F, 0.5F, 1.0F, 0.05F})) {
    set_show_in_arraylist(false);
    set_enabled(true);
}

} // namespace medicon
