#pragma once

#include "medicon/core/Module.hpp"

namespace medicon {

class WatermarkModule final : public Module {
public:
    static constexpr const char* module_id = "watermark";

    WatermarkModule();

    Setting& show_version;
    Setting& show_fps;
    Setting& show_ping;
    Setting& shadow;
    Setting& glow;
    Setting& scale;
    Setting& opacity;
};

} // namespace medicon
