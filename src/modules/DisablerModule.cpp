#include "medicon/modules/DisablerModule.hpp"

namespace medicon {

DisablerModule::DisablerModule()
    : Module(module_id, "Disabler", "Adjusts outgoing movement input using the selected mode.", Category::Misc),
      mode(add_setting({"mode", "Mode", "Movement packet mode.", Choice{0, {"Flareon"}}})),
      move_fix(add_setting({"move_fix", "MoveFix", "Align movement input with horizontal motion and stop sprinting when it is not forward.", true})) {}

bool DisablerModule::move_fix_enabled() const noexcept {
    const auto* selection = std::get_if<Choice>(&mode.value);
    const auto* enabled = std::get_if<bool>(&move_fix.value);
    return this->enabled() && selection && selection->index == 0 && enabled && *enabled;
}

} // namespace medicon
