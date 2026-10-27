#pragma once

#include "medicon/core/Module.hpp"

#include <array>

namespace medicon {

// Holds the ClickGUI settings. Its enabled state mirrors whether the GUI is open
// and its keybind is the open/close key (Insert by default). Panel layout is kept
// in hidden settings so it persists through the existing ConfigStore schema.
class ClickGuiModule final : public Module {
public:
    static constexpr const char* module_id = "clickgui";
    static constexpr int default_key = 0x2D; // VK_INSERT

    struct PanelSettings {
        Setting* x = nullptr;         // rpx
        Setting* y = nullptr;         // rpx
        Setting* collapsed = nullptr;
    };

    ClickGuiModule();

    Setting& scale;
    Setting& panel_shadow;
    Setting& shadow_opacity;
    Setting& shadow_blur;
    Setting& shadow_offset_y;
    Setting& panel_order; // comma-separated category ids, back to front; empty = never saved
    Setting& input_animation; // typed characters dip and fade in (Social text inputs)

    PanelSettings& panel(Category category) noexcept { return panels_[static_cast<std::size_t>(category)]; }
    // Never unbound: a cleared keybind falls back to Insert so the GUI stays reachable.
    int open_key() const noexcept { return keybind() != 0 ? keybind() : default_key; }

private:
    std::array<PanelSettings, category_count> panels_{};
};

class ArrayListModule final : public Module {
public:
    static constexpr const char* module_id = "arraylist";

    ArrayListModule();

    Setting& accent_a;
    Setting& accent_b;
    Setting& glow;
    Setting& shadow;
    Setting& pulse;
    Setting& scale;
};

} // namespace medicon
