#include "medicon/modules/ArrayListModule.hpp"

namespace medicon {

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

} // namespace medicon
