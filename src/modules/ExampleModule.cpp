#include "medicon/modules/ExampleModule.hpp"

namespace medicon {

ExampleModule::ExampleModule()
    : Module("example", "Example", "Settings gallery for creating your own modules.", Category::Misc),
      show_label(add_setting({"show_label", "Show label", "Example boolean setting.", true})),
      scale(add_setting({"scale", "Scale", "Example float setting.", 1.0F, 0.5F, 3.0F, 0.05F})),
      sample_count(add_setting({"sample_count", "Sample count", "Example integer setting.", 8, 1, 32, 1})),
      alignment(add_setting({"alignment", "Alignment", "Example choice setting.", Choice{0, {"Left", "Center", "Right"}}})),
      tint(add_setting({"tint", "Tint", "Example RGBA color setting.", Color{0.35F, 0.65F, 1.0F, 1.0F}})),
      label(add_setting({"label", "Label", "Example text setting.", std::string{"Medicon"}})) {}

} // namespace medicon
