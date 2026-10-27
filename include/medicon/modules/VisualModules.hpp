#pragma once

#include "medicon/core/Module.hpp"

namespace medicon {

class ChinaHatModule final : public Module {
public:
    static constexpr const char* module_id = "chinahat";

    ChinaHatModule();

    Setting& radius;
    Setting& height;
    Setting& offset;
    Setting& color;
    Setting& rainbow;
    Setting& opacity;
};

class EspModule final : public Module {
public:
    static constexpr const char* module_id = "esp";

    EspModule();

    Setting& range;
    Setting& players_only;
    Setting& color;
    Setting& filled;
    Setting& fill_opacity;
    Setting& line_width;
    Setting& glow;
    Setting& glow_width;
    Setting& glow_intensity;
};

} // namespace medicon
