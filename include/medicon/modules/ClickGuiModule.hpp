#pragma once

#include "medicon/core/Module.hpp"

#include <array>
#include <cstddef>

namespace medicon {

// Holds the ClickGUI settings and persistent panel layout.
class ClickGuiModule final : public Module {
public:
    static constexpr const char* module_id = "clickgui";
    static constexpr int default_key = 0x2D; // VK_INSERT

    struct PanelSettings {
        Setting* x = nullptr;
        Setting* y = nullptr;
        Setting* collapsed = nullptr;
    };

    ClickGuiModule();

    Setting& scale;
    Setting& panel_shadow;
    Setting& shadow_opacity;
    Setting& shadow_blur;
    Setting& shadow_offset_y;
    Setting& panel_order;
    Setting& input_animation;

    PanelSettings& panel(Category category) noexcept { return panels_[static_cast<std::size_t>(category)]; }
    int open_key() const noexcept { return keybind() != 0 ? keybind() : default_key; }

private:
    std::array<PanelSettings, category_count> panels_{};
};

} // namespace medicon
