#include "medicon/modules/ClickGuiModule.hpp"

#include "medicon/gui/Theme.hpp"

#include <string>

namespace medicon {
namespace {
constexpr float position_limit = 100000.0F;
}

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

} // namespace medicon
