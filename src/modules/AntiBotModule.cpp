#include "medicon/modules/AntiBotModule.hpp"

namespace medicon {

namespace {

// Solstice's AntiBot limits; a sneaking player is still above the minimum height.
constexpr float player_height_min = 1.35F, player_height_max = 1.81F;
constexpr float player_width_min = 0.54F, player_width_max = 0.66F;

} // namespace

AntiBotModule::AntiBotModule()
    : Module(module_id, "AntiBot", "Exclude server-side fake players from targeting and ESP.", Category::Misc),
      mode(add_setting({"mode", "Mode", "Simple uses the Hitbox, Player, Invisible and Name checks.",
                        Choice{0, {"Simple", "Custom"}}})),
      hitbox_check(add_setting({"hitboxCheck", "Hitbox Check", "Treat actors without a player-sized hitbox as bots.", true})),
      player_check(add_setting({"playerCheck", "Player Check", "Treat non-player actors as bots.", true})),
      invisible_check(add_setting({"invisibleCheck", "Invisible Check", "Treat invisible actors as bots.", true})),
      name_check(add_setting({"nameCheck", "Name Check", "Treat players with a multi-line nametag as bots.", true})),
      armor_check(add_setting({"armorCheck", "Has Armor Check", "Treat actors without armor as bots.", false})),
      armor_mode(add_setting({"armorMode", "Armor Mode", "Armor pieces an actor must wear.",
                              Choice{0, {"Full", "One Element"}}})) {}

bool AntiBotModule::setting_visible(const Setting& setting) const noexcept {
    if (&setting == &mode) return true;
    if (!custom()) return false;
    return &setting != &armor_mode || armor_check.get<bool>();
}

bool AntiBotModule::is_bot(const sdk::ActorSnapshot& actor) const {
    if (!enabled()) return false;
    const auto check = [this](const Setting& setting) { return !custom() || setting.get<bool>(); };
    if (check(player_check) && !actor.player) return true;

    const float width = actor.bounds.max.x - actor.bounds.min.x;
    const float height = actor.bounds.max.y - actor.bounds.min.y;
    if (check(hitbox_check) && !(width >= player_width_min && width <= player_width_max &&
                                 height >= player_height_min && height <= player_height_max))
        return true;

    if (check(invisible_check) && actor.invisible) return true;
    if (check(name_check) && actor.multiline_name) return true;

    if (custom() && armor_check.get<bool>()) {
        if (!actor.player) return true;
        // Unreadable armor is not evidence; only a read that found too few counts.
        const int required = armor_mode.get<Choice>().index == 0 ? 4 : 1;
        if (actor.armor_pieces >= 0 && actor.armor_pieces < required) return true;
    }
    return false;
}

} // namespace medicon
