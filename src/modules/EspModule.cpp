#include "medicon/modules/EspModule.hpp"

namespace medicon {

EspModule::EspModule()
    : Module(module_id, "ESP", "Draw entity bounds with a soft glow around the outline.", Category::Visual),
      range(add_setting({"range", "Range", "Maximum entity distance in blocks.", 64.0F, 4.0F, 256.0F, 1.0F})),
      players_only(add_setting({"players_only", "Players Only", "Only draw other players.", true})),
      color(add_setting({"color", "Color", "Outline, fill and glow color.", Color{0.93F, 0.35F, 0.75F, 1.0F}})),
      filled(add_setting({"filled", "Filled", "Add a translucent fill inside the outline.", false})),
      fill_opacity(add_setting({"fill_opacity", "Fill Opacity", "Opacity of the optional box fill.", 0.08F, 0.0F, 0.5F, 0.01F})),
      line_width(add_setting({"line_width", "Line Width", "Outline width in screen pixels.", 1.5F, 1.0F, 4.0F, 0.25F})),
      glow(add_setting({"glow", "Glow", "Draw a soft bloom around the outline.", true})) {}

} // namespace medicon
