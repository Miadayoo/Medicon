#pragma once

#include "medicon/core/Module.hpp"

namespace medicon {

class DisablerModule final : public Module {
public:
    static constexpr const char* module_id = "disabler";
    DisablerModule();

    Setting& mode;
    Setting& move_fix;

    [[nodiscard]] bool move_fix_enabled() const noexcept;
};

} // namespace medicon
