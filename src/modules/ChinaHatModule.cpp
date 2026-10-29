#include "medicon/modules/ChinaHatModule.hpp"

namespace medicon {

ChinaHatModule::ChinaHatModule()
    : Module(module_id, "ChinaHat", "Draw a conical hat above your head in third person.", Category::Visual),
      radius(add_setting({"radius", "Radius", "Hat brim radius in blocks.", 0.8F, 0.2F, 2.0F, 0.05F})),
      height(add_setting({"height", "Height", "Distance from the brim to the tip in blocks.", 0.35F, 0.1F, 1.5F, 0.05F})),
      offset(add_setting({"offset", "Head Offset", "Lift the brim above your head in blocks.", 0.1F, 0.0F, 1.0F, 0.05F})),
      color(add_setting({"color", "Color", "Hat color when Rainbow is disabled.", Color{0.93F, 0.35F, 0.75F, 1.0F}})),
      rainbow(add_setting({"rainbow", "Rainbow", "Cycle the hat through the color spectrum.", false})),
      opacity(add_setting({"opacity", "Opacity", "Opacity of the hat surface.", 0.3F, 0.0F, 1.0F, 0.05F})) {}

} // namespace medicon
