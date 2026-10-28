#pragma once

#include "medicon/core/Module.hpp"

namespace medicon {

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
