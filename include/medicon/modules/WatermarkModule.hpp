#pragma once

#include "medicon/core/Module.hpp"

namespace medicon {

class WatermarkModule final : public Module {
public:
    static constexpr const char* module_id = "watermark";

    WatermarkModule();

    [[nodiscard]] bool setting_visible(const Setting& setting) const noexcept override;
    // The glowing めじこん instead of the glass plate.
    [[nodiscard]] bool japanese() const noexcept;

    Setting& mode;
    Setting& show_version;
    Setting& show_fps;
    Setting& show_ping;
    Setting& shadow;
    Setting& glow;
    Setting& scale;
    Setting& opacity;
};

} // namespace medicon
