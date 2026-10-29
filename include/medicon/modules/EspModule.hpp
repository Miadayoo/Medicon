#pragma once

#include "medicon/core/Module.hpp"

namespace medicon {

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
