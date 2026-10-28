#include <medicon/modules/CustomModelProbeModule.hpp>

namespace medicon {
CustomModelProbeModule::CustomModelProbeModule()
    : Module(module_id, "CustomModel", "Local FBX player model, custom first-person arms and animation (F8).", Category::Visual, 0x77),
      show_debug(add_setting({"show_debug", "Show Debug", "Show loading, native draw failures, cost and ON/OFF FPS.", false})),
      mode(add_setting({"mode", "Mode", "Custom model keeps the vanilla player visible until loading completes.", Choice{0,{"Custom model", "Test box", "Hide vanilla"}}})),
      model(add_setting({"model", "Model", "Select the bundled character. The vanilla player stays visible while the model loads.", Choice{0,{"Neon", "Chocolat"}}})),
      animation(add_setting({"animation", "Animation", "Animate the model and first-person arms. Turn OFF to keep a static pose.", true})),
      test_box(add_setting({"test_box", "P1 Test Box", "Draw a 1 x 2 x 1 test box at the local player.", true})),
      material(add_setting({"material", "Material", "Choose the native entity material used to draw the model.", Choice{0,{"Neon","entity"}}})),
      brightness(add_setting({"brightness", "Brightness", "Multiply test face colors; compare day, night and caves.", 1.0F, 0.05F, 1.0F, 0.05F})) {}
}
