#pragma once

#include "medicon/core/Module.hpp"

namespace medicon {

// A harmless settings gallery. Replace it or add another Module subclass.
// All six setting variants receive GUI controls and config persistence.
class ExampleModule final : public Module {
public:
    ExampleModule();

    Setting& show_label;
    Setting& scale;
    Setting& sample_count;
    Setting& alignment;
    Setting& tint;
    Setting& label;
};

} // namespace medicon
