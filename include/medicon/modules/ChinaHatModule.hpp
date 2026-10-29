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

} // namespace medicon
