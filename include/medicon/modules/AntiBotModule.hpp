#pragma once

#include "medicon/core/Module.hpp"
#include "medicon/sdk/Gameplay.hpp"

namespace medicon {

// Keeps server-side fake players out of targeting and ESP. Client marks each
// captured actor before any gameplay module runs.
class AntiBotModule final : public Module {
public:
    static constexpr const char* module_id = "antibot";
    AntiBotModule();

    [[nodiscard]] bool setting_visible(const Setting& setting) const noexcept override;
    [[nodiscard]] bool is_bot(const sdk::ActorSnapshot& actor) const;

    Setting& mode;
    Setting& hitbox_check;
    Setting& player_check;
    Setting& invisible_check;
    Setting& name_check;
    Setting& armor_check;
    Setting& armor_mode;

private:
    [[nodiscard]] bool custom() const { return mode.get<Choice>().index == 1; }
};

} // namespace medicon
