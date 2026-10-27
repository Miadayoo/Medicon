#include "medicon/modules/GameplayModules.hpp"
#include "medicon/sdk/Gameplay.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using namespace medicon;
using namespace medicon::sdk;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool near(float a, float b, float tolerance = 0.001F) {
    return std::abs(a - b) <= tolerance;
}

using Cell = std::tuple<int, int, int>;
Cell cell(BlockPos position) { return {position.x, position.y, position.z}; }

class MockGame final : public GameplayAccess {
public:
    MockGame() {
        state.ready = true;
        state.local.id = 1;
        state.local.local = true;
        state.local.alive = true;
        state.local.player = true;
        state.local.position = {0.5F, 65.62F, 0.5F};
        state.local.bounds = {{0.2F, 64.0F, 0.2F}, {0.8F, 65.8F, 0.8F}};
        state.eye = state.local.position;
    }

    GameplayFrame state;
    std::vector<std::pair<float, float>> rotations;
    std::vector<std::uintptr_t> attacks;
    std::vector<std::pair<BlockPos, bool>> placements;
    std::set<Cell> solid;
    bool rotate_result = true;
    bool attack_result = true;
    bool place_result = true;
    std::set<Cell> refuse; // cells place_block fails for (no support face)
    bool throw_rotation = false;
    bool throw_attack = false;
    bool throw_place = false;
    bool throw_query = false;

    const GameplayFrame& frame() const noexcept override { return state; }
    bool rotate(float yaw, float pitch) override {
        if (throw_rotation) throw std::runtime_error("rotate");
        rotations.emplace_back(yaw, pitch);
        return rotate_result;
    }
    bool attack(std::uintptr_t actor_id) override {
        if (throw_attack) throw std::runtime_error("attack");
        attacks.push_back(actor_id);
        return attack_result;
    }
    bool place_block(BlockPos position, bool auto_select) override {
        if (throw_place) throw std::runtime_error("place");
        placements.emplace_back(position, auto_select);
        const bool placed = place_result && !refuse.contains(cell(position));
        if (placed) solid.insert(cell(position));
        return placed;
    }
    bool replaceable(BlockPos position) const override {
        if (throw_query) throw std::runtime_error("query");
        return !solid.contains(cell(position));
    }

    ActorSnapshot target(std::uintptr_t id, float yaw, float distance, bool player = true) const {
        const float radians = yaw * 0.017453292519943295F;
        ActorSnapshot actor;
        actor.id = id;
        actor.alive = true;
        actor.player = player;
        actor.visible = true;
        actor.position = {state.eye.x - std::sin(radians) * distance,
                          state.eye.y,
                          state.eye.z + std::cos(radians) * distance};
        return actor;
    }
};

void test_disabled_and_unavailable() {
    MockGame game;
    game.state.actors.push_back(game.target(2, 0.0F, 2.0F));
    DerpModule derp;
    AimbotModule aimbot;
    KillauraModule aura;
    ScaffoldModule scaffold;
    GameplayModule* modules[] = {&derp, &aimbot, &aura, &scaffold};
    check(derp.category() == Category::Player && aimbot.category() == Category::Player &&
          aura.category() == Category::Player && scaffold.category() == Category::Movement,
          "Gameplay module categories changed");
    for (auto* module : modules) {
        check(!module->enabled(), "Gameplay modules must be disabled by default");
        module->game_tick(game, 0.05F);
        module->set_enabled(true);
    }
    check(game.rotations.empty() && game.attacks.empty() && game.placements.empty(), "Disabled modules touched the game");

    game.state.ready = false;
    for (auto* module : modules) module->game_tick(game, 0.05F);
    game.state.ready = true;
    game.state.local.alive = false;
    for (auto* module : modules) module->game_tick(game, 0.05F);
    game.state.local.alive = true;
    for (auto* module : modules) {
        module->game_tick(game, std::numeric_limits<float>::quiet_NaN());
        module->game_tick(game, -1.0F);
        module->game_tick(game, 0.0F);
    }
    check(game.rotations.empty() && game.attacks.empty() && game.placements.empty(), "Invalid frames or deltas caused an action");
}

void test_derp_spin_and_reset() {
    MockGame game;
    game.state.yaw = 170.0F;
    DerpModule derp;
    derp.set_enabled(true);
    derp.game_tick(game, 0.05F);
    check(game.rotations.size() == 1 && near(game.rotations.back().first, -172.0F) &&
          near(game.rotations.back().second, 89.0F), "Derp did not wrap the yaw or use its pitch");
    derp.game_tick(game, 0.05F);
    check(near(game.rotations.back().first, -154.0F), "Derp did not accumulate its spin");

    derp.set_enabled(false);
    game.state.yaw = 20.0F;
    derp.set_enabled(true);
    derp.pitch.get<float>() = -30.0F;
    derp.game_tick(game, 0.05F);
    check(near(game.rotations.back().first, 38.0F) && near(game.rotations.back().second, -30.0F), "Derp retained state after disabling");
    game.state.local.id = 7;
    game.state.yaw = 100.0F;
    derp.game_tick(game, 0.05F);
    check(near(game.rotations.back().first, 118.0F), "Derp retained another player's yaw");
    game.state.ready = false;
    derp.game_tick(game, 0.05F);
    game.state.ready = true;
    game.state.yaw = 0.0F;
    derp.game_tick(game, 0.05F);
    check(near(game.rotations.back().first, 18.0F), "Derp retained state across an unavailable world");

    game.rotate_result = false;
    derp.game_tick(game, 0.05F);
    game.rotate_result = true;
    derp.game_tick(game, 0.05F);
    check(near(game.rotations.back().first, 36.0F), "Derp advanced its spin after a rejected rotation");
    game.throw_rotation = true;
    derp.game_tick(game, 0.05F);
    game.throw_rotation = false;
    game.state.yaw = 30.0F;
    derp.game_tick(game, 0.05F);
    check(near(game.rotations.back().first, 48.0F), "Derp did not recover after a bridge exception");
}

void test_aimbot_selection_and_cone() {
    MockGame game;
    AimbotModule aimbot;
    aimbot.set_enabled(true);
    game.state.actors.push_back(game.target(2, 20.0F, 3.0F));
    game.state.actors.push_back(game.target(3, 30.0F, 1.0F));
    game.state.actors.push_back(game.target(1, 0.0F, 1.0F)); // local id
    auto actor = game.target(4, 0.0F, 1.0F);
    actor.local = true;
    game.state.actors.push_back(actor);
    actor.local = false;
    actor.alive = false;
    game.state.actors.push_back(actor);
    actor.alive = true;
    actor.visible = false;
    game.state.actors.push_back(actor);
    actor.visible = true;
    actor.player = false;
    game.state.actors.push_back(actor);
    game.state.actors.push_back(game.target(5, 0.0F, 20.0F)); // out of range
    aimbot.game_tick(game, 0.05F);
    check(game.rotations.empty(), "Aimbot ignored Require Click");
    game.state.attack_down = true;
    aimbot.game_tick(game, 0.05F);
    const float expected = 20.0F * (1.0F - std::exp(-8.0F * 0.05F));
    check(game.rotations.size() == 1 && near(game.rotations.back().first, expected),
          "Aimbot did not choose the closest angular eligible target or apply smoothing");

    game.state.actors = {game.target(6, 60.0F, 3.0F)};
    aimbot.game_tick(game, 0.05F);
    check(game.rotations.size() == 1, "Aimbot selected a target outside its cone");
    aimbot.require_click.get<bool>() = false;
    game.state.attack_down = false;
    game.state.actors = {game.target(7, 10.0F, 2.0F, false)};
    game.state.actors.front().visible = false;
    aimbot.players_only.get<bool>() = false;
    aimbot.game_tick(game, 0.05F);
    check(game.rotations.size() == 1, "Aimbot ignored visibility filtering");
    aimbot.visible_only.get<bool>() = false;
    aimbot.game_tick(game, 0.05F);
    check(game.rotations.size() == 2 && game.rotations.back().first > 0.0F,
          "Aimbot target or input filter settings do not work");
    check(game.attacks.empty(), "Aimbot attacked a target");
}

void test_aimbot_yaw_wrap_and_bounds_range() {
    MockGame game;
    AimbotModule aimbot;
    aimbot.set_enabled(true);
    aimbot.require_click.get<bool>() = false;
    game.state.yaw = 179.0F;
    game.state.actors = {game.target(2, -179.0F, 3.0F)};
    aimbot.game_tick(game, 0.05F);
    check(game.rotations.size() == 1 && game.rotations.back().first > 179.0F && game.rotations.back().first < 180.0F,
          "Aimbot took the long path at the yaw wrap");

    game.state.yaw = 0.0F;
    auto target = game.target(3, 0.0F, 8.0F);
    target.bounds = {{0.2F, 64.0F, 2.5F}, {0.8F, 65.8F, 8.5F}};
    game.state.actors = {target};
    aimbot.range.get<float>() = 3.0F;
    aimbot.game_tick(game, 0.05F);
    check(game.rotations.size() == 2 && game.rotations.back().second > 0.0F,
          "Aimbot did not use target bounds for range and vertical aim");

    // A 90-degree yaw difference is still inside a 40-degree spatial cone
    // when both directions look almost straight up.
    game.state.pitch = -80.0F;
    aimbot.fov.get<float>() = 40.0F;
    aimbot.range.get<float>() = 6.0F;
    auto overhead = game.target(4, 90.0F, std::cos(80.0F * 0.017453292519943295F) * 5.0F);
    overhead.position.y += std::sin(80.0F * 0.017453292519943295F) * 5.0F;
    game.state.actors = {overhead};
    aimbot.game_tick(game, 0.05F);
    check(game.rotations.size() == 3 && game.rotations.back().first > 0.0F &&
          near(game.rotations.back().second, -80.0F), "Aimbot FOV used yaw/pitch distance instead of a spatial cone");

    game.state.actors.front().position.x = std::numeric_limits<float>::quiet_NaN();
    aimbot.game_tick(game, 0.05F);
    check(game.rotations.size() == 3, "Aimbot selected non-finite target data");
    game.state.pitch = 0.0F;
    game.throw_rotation = true;
    game.state.actors = {game.target(4, 0.0F, 2.0F)};
    aimbot.game_tick(game, 0.05F); // noexcept bridge boundary
}

void test_killaura_cadence_and_filters() {
    MockGame game;
    KillauraModule aura;
    aura.set_enabled(true);
    game.state.actors = {game.target(3, 0.0F, 3.0F), game.target(2, 35.0F, 2.0F),
                         game.target(1, 0.0F, 1.0F), game.target(4, 0.0F, 1.0F, false)};
    auto hidden = game.target(5, 0.0F, 1.0F);
    hidden.visible = false;
    game.state.actors.push_back(hidden);
    aura.game_tick(game, 0.05F);
    check(game.attacks == std::vector<std::uintptr_t>{2} && near(game.rotations.back().first, 35.0F),
          "Killaura selected an invalid target or failed to rotate");
    aura.game_tick(game, 0.05F);
    check(game.attacks.size() == 1, "Killaura exceeded its CPS limit");
    aura.game_tick(game, 0.05F);
    check(game.attacks.size() == 2, "Killaura did not attack after the cooldown");
    aura.game_tick(game, 100.0F);
    check(game.attacks.size() == 3, "Killaura replayed missed attacks as a burst");
    aura.game_tick(game, 0.001F);
    check(game.attacks.size() == 3, "Killaura carried missed attacks into the next tick");

    aura.set_enabled(false);
    aura.rotate.get<bool>() = false;
    aura.set_enabled(true);
    const auto rotation_count = game.rotations.size();
    aura.game_tick(game, 0.01F);
    check(game.attacks.size() == 4 && game.rotations.size() == rotation_count, "Killaura failed to reset or ignored Rotate");
    game.state.local.id = 9;
    aura.game_tick(game, 0.01F);
    check(game.attacks.size() == 5, "Killaura cooldown survived a local player change");
    game.state.ready = false;
    aura.game_tick(game, 0.01F);
    game.state.ready = true;
    aura.game_tick(game, 0.01F);
    check(game.attacks.size() == 6, "Killaura cooldown survived an unavailable world");

    game.state.actors = {game.target(7, 160.0F, 2.0F, false)};
    game.state.actors.front().visible = false;
    aura.game_tick(game, 0.25F);
    check(game.attacks.size() == 6, "Killaura ignored target filters");
    aura.players_only.get<bool>() = false;
    aura.visible_only.get<bool>() = false;
    aura.game_tick(game, 0.05F);
    check(game.attacks.size() == 7 && game.attacks.back() == 7, "Killaura filter settings do not work");
}

void test_killaura_failures_are_throttled() {
    MockGame game;
    KillauraModule aura;
    aura.set_enabled(true);
    aura.rotate.get<bool>() = false;
    game.state.actors = {game.target(2, 0.0F, 2.0F)};
    game.attack_result = false;
    aura.game_tick(game, 0.05F);
    aura.game_tick(game, 0.05F);
    check(game.attacks.size() == 1, "Killaura retried rejected attacks without a cooldown");
    game.throw_attack = true;
    aura.game_tick(game, 0.05F);
    game.throw_attack = false;
    aura.game_tick(game, 0.05F);
    check(game.attacks.size() == 1, "Killaura retried a bridge exception immediately");
    aura.game_tick(game, 0.25F);
    check(game.attacks.size() == 2, "Killaura did not recover after a bridge exception");
}

void test_scaffold_floor_extension_and_delay() {
    MockGame game;
    ScaffoldModule scaffold;
    scaffold.set_enabled(true);
    scaffold.game_tick(game, 0.05F);
    check(game.placements.size() == 1 && cell(game.placements.back().first) == Cell{0, 63, 0} &&
          game.placements.back().second, "Scaffold did not place below the player's bounds");

    game.state.local.velocity = {0.2F, 0.0F, 0.0F};
    scaffold.game_tick(game, 0.05F);
    check(game.placements.size() == 1, "Scaffold ignored its placement delay");
    scaffold.auto_select.get<bool>() = false;
    scaffold.game_tick(game, 0.05F);
    check(game.placements.size() == 2 && cell(game.placements.back().first) == Cell{1, 63, 0} &&
          !game.placements.back().second, "Scaffold did not extend from velocity or pass Auto Select");
    scaffold.game_tick(game, 10.0F);
    check(game.placements.size() == 2, "Scaffold placed beyond its configured extension");

    scaffold.set_enabled(false);
    scaffold.set_enabled(true);
    game.state.local.position = {-0.2F, 65.62F, -1.2F};
    game.state.local.velocity = {};
    scaffold.game_tick(game, 0.01F);
    check(game.placements.size() == 3 && cell(game.placements.back().first) == Cell{-1, 63, -2},
          "Scaffold retained its cooldown or rounded negative coordinates toward zero");
    game.state.local.id = 9;
    game.solid.clear();
    scaffold.game_tick(game, 0.01F);
    check(game.placements.size() == 4, "Scaffold cooldown survived a local player change");
}

void test_scaffold_stationary_failure_and_invalid_data() {
    MockGame game;
    ScaffoldModule scaffold;
    scaffold.set_enabled(true);
    game.place_result = false;
    // One attempt is the target and, failing that, the four cells beside it.
    constexpr std::size_t attempt = 5;
    scaffold.game_tick(game, 0.05F);
    scaffold.game_tick(game, 0.05F);
    check(game.placements.size() == attempt, "Scaffold retried a failed placement before the delay");
    scaffold.game_tick(game, 0.05F);
    check(game.placements.size() == attempt * 2, "Scaffold did not retry after a failed placement");
    game.solid.insert(Cell{0, 63, 0});
    scaffold.game_tick(game, 0.25F);
    check(game.placements.size() == attempt * 2, "Scaffold extended without horizontal movement");

    game.state.local.position.x = std::numeric_limits<float>::infinity();
    scaffold.game_tick(game, 0.25F);
    game.state.local.position.x = std::numeric_limits<float>::max();
    scaffold.game_tick(game, 0.25F);
    game.state.local.position.x = 0.5F;
    game.state.local.bounds = {};
    scaffold.game_tick(game, 0.25F);
    check(game.placements.size() == attempt * 2, "Scaffold converted invalid geometry to a block position");

    game.state.local.bounds = {{0.2F, 64.0F, 0.2F}, {0.8F, 65.8F, 0.8F}};
    game.solid.clear();
    game.throw_query = true;
    scaffold.game_tick(game, 0.25F);
    game.throw_query = false;
    scaffold.game_tick(game, 0.01F);
    check(game.placements.size() == attempt * 2, "Scaffold immediately retried a bridge exception");
    scaffold.game_tick(game, 0.25F);
    check(game.placements.size() == attempt * 3, "Scaffold did not recover from a bridge exception");
}

// The cell under the feet between block levels, and the fallback support.
void test_scaffold_level_and_support() {
    {
        MockGame game;
        ScaffoldModule scaffold;
        scaffold.set_enabled(true);
        game.solid.insert(Cell{0, 63, 0});
        // Mid-jump the cell around the feet would intersect the player.
        game.state.local.bounds = {{0.2F, 64.45F, 0.2F}, {0.8F, 66.25F, 0.8F}};
        scaffold.game_tick(game, 0.05F);
        check(game.placements.empty(), "Scaffold placed into the cell the jumping player occupies");
        // Above the next level (towering), the block goes under the feet.
        game.state.local.bounds = {{0.2F, 65.1F, 0.2F}, {0.8F, 66.9F, 0.8F}};
        scaffold.game_tick(game, 0.25F);
        check(game.placements.size() == 1 && cell(game.placements.back().first) == Cell{0, 64, 0},
              "Scaffold did not place under the feet while towering");
        // Just below the bridge level at the start of a fall, catch one lower.
        game.solid.clear();
        game.state.local.bounds = {{0.2F, 63.8F, 0.2F}, {0.8F, 65.6F, 0.8F}};
        scaffold.game_tick(game, 0.25F);
        check(game.placements.size() == 2 && cell(game.placements.back().first) == Cell{0, 62, 0},
              "Scaffold chose a cell intersecting a falling player");
        // Standing a hair below an integer height still means that level.
        game.solid.clear();
        game.state.local.bounds = {{0.2F, 63.995F, 0.2F}, {0.8F, 65.795F, 0.8F}};
        scaffold.game_tick(game, 0.25F);
        check(game.placements.size() == 3 && cell(game.placements.back().first) == Cell{0, 63, 0},
              "Scaffold rounded a resting player down a level");
    }
    {
        MockGame game;
        ScaffoldModule scaffold;
        scaffold.set_enabled(true);
        // A diagonal step: the target has no face to click, the cell nearest
        // the player beside it does and becomes the support.
        game.state.local.position = {0.9F, 65.62F, 0.5F};
        game.refuse.insert(Cell{0, 63, 0});
        scaffold.game_tick(game, 0.05F);
        check(game.placements.size() == 2 && cell(game.placements.front().first) == Cell{0, 63, 0} &&
              cell(game.placements.back().first) == Cell{1, 63, 0},
              "Scaffold did not place a support beside an unsupported target");
        game.refuse.clear();
        scaffold.game_tick(game, 0.25F);
        check(game.placements.size() == 3 && cell(game.placements.back().first) == Cell{0, 63, 0},
              "Scaffold did not return to the target once supported");
    }
}

} // namespace

int main() {
    try {
        test_disabled_and_unavailable();
        test_derp_spin_and_reset();
        test_aimbot_selection_and_cone();
        test_aimbot_yaw_wrap_and_bounds_range();
        test_killaura_cadence_and_filters();
        test_killaura_failures_are_throttled();
        test_scaffold_floor_extension_and_delay();
        test_scaffold_stationary_failure_and_invalid_data();
        test_scaffold_level_and_support();
        std::cout << "Gameplay module tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Gameplay module test failed: " << error.what() << '\n';
        return 1;
    }
}
