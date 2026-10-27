#include "medicon/modules/VisualModules.hpp"

namespace medicon {

ChinaHatModule::ChinaHatModule()
    : Module(module_id, "ChinaHat", "Draw a conical hat above your head in third person.", Category::Visual),
      radius(add_setting({"radius", "Radius", "Hat brim radius in blocks.", 0.8F, 0.2F, 2.0F, 0.05F})),
      height(add_setting({"height", "Height", "Distance from the brim to the tip in blocks.", 0.35F, 0.1F, 1.5F, 0.05F})),
      offset(add_setting({"offset", "Head Offset", "Lift the brim above your head in blocks.", 0.1F, 0.0F, 1.0F, 0.05F})),
      color(add_setting({"color", "Color", "Hat color when Rainbow is disabled.", Color{0.93F, 0.35F, 0.75F, 1.0F}})),
      rainbow(add_setting({"rainbow", "Rainbow", "Cycle the hat through the color spectrum.", false})),
      opacity(add_setting({"opacity", "Opacity", "Opacity of the hat surface.", 0.3F, 0.0F, 1.0F, 0.05F})) {}

EspModule::EspModule()
    : Module(module_id, "ESP", "Draw entity bounds with a soft glow around the outline.", Category::Visual),
      range(add_setting({"range", "Range", "Maximum entity distance in blocks.", 64.0F, 4.0F, 256.0F, 1.0F})),
      players_only(add_setting({"players_only", "Players Only", "Only draw other players.", true})),
      color(add_setting({"color", "Color", "Outline, fill and glow color.", Color{0.93F, 0.35F, 0.75F, 1.0F}})),
      filled(add_setting({"filled", "Filled", "Add a translucent fill inside the outline.", false})),
      fill_opacity(add_setting({"fill_opacity", "Fill Opacity", "Opacity of the optional box fill.", 0.08F, 0.0F, 0.5F, 0.01F})),
      line_width(add_setting({"line_width", "Line Width", "Outline width in screen pixels.", 1.5F, 1.0F, 4.0F, 0.25F})),
      glow(add_setting({"glow", "Glow", "Draw a fading halo outside the outline.", true})),
      glow_width(add_setting({"glow_width", "Glow Width", "Halo spread in screen pixels.", 10.0F, 1.0F, 32.0F, 1.0F})),
      glow_intensity(add_setting({"glow_intensity", "Glow Intensity", "Brightness of the outline halo.", 1.0F, 0.0F, 2.5F, 0.05F})) {}

} // namespace medicon
