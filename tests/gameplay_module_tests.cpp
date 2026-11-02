#include "medicon/modules/GameplayModules.hpp"
#include "medicon/modules/RotationsModule.hpp"
#include "medicon/sdk/Gameplay.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
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
    std::vector<float> head_body_rotations;
    std::vector<bool> sprint_requests;
    std::vector<Vec3> velocity_requests;
    bool velocity_result = true;
    bool throw_velocity = false;
    bool native_prediction = false, prediction_available = true;
    std::vector<std::pair<Vec3, std::int64_t>> prediction_requests;
    std::vector<std::uint64_t> prediction_ticks;
    std::vector<LatencyReply> latency_replies;
    std::vector<char> velocity_events;
    bool latency_result = true;
    bool sprint_result = true;
    std::vector<std::uintptr_t> attacks;
    std::vector<std::pair<BlockPos, bool>> placements;
    std::set<Cell> solid;
    bool rotate_result = true;
    bool rotate_head_and_body_result = true;
    bool attack_result = true;
    bool place_result = true;
    std::set<Cell> refuse; // cells place_block fails for (no support face)
    bool throw_rotation = false;
    bool throw_attack = false;
    bool throw_place = false;
    bool throw_query = false;
    std::array<InventoryItemInfo, 36> inventory{};
    std::array<InventoryItemInfo, 4> worn{};
    std::vector<std::pair<int, int>> swaps, equips;
    std::vector<int> drops;
    bool inventory_readable = true;
    bool armor_readable = true;
    bool inventory_open = false;
    bool server_answers_swaps = true;
    bool swap_result = true;
    bool server_answers_drops = true;
    bool server_answers_equips = true;
    bool drop_result = true;
    std::map<Cell, std::uint16_t> blocks;
    std::vector<BlockPos> destroy_requests;
    float next_progress = 0.0F;
    bool next_destroyed = false;
    bool destroy_result = true;
    bool switch_before_destroy = false;
    int selected_slot = 0;
    int destroy_stops = 0, mining_swings = 0, safe_finishes = 0;

    const GameplayFrame& frame() const noexcept override { return state; }
    bool block_id(BlockPos position, std::uint16_t& id) const override {
        const auto found = blocks.find(cell(position));
        id = found == blocks.end() ? 0 : found->second;
        return true;
    }
    bool advance_destroy_block(BlockPos position, std::uint8_t, float, float& progress, bool& destroyed) override {
        destroy_requests.push_back(position);
        progress = next_progress;
        destroyed = next_destroyed;
        return destroy_result;
    }
    bool should_switch_before_destroy() const override { return switch_before_destroy; }
    bool prepare_durability_safe_finish() override { ++safe_finishes; return true; }
    void stop_destroy_block() noexcept override { ++destroy_stops; }
    bool mining_swing() override { ++mining_swings; return true; }
    int selected_hotbar_slot() const noexcept override { return selected_slot; }
    bool hotbar_item(int slot, BlockPos, HotbarItemInfo& info) const override {
        const auto& item = inventory[static_cast<std::size_t>(slot)];
        info.occupied = item.kind != InventoryItemKind::empty;
        info.pickaxe = item.kind == InventoryItemKind::pickaxe;
        info.damageable = info.pickaxe;
        info.destroy_speed = info.pickaxe ? item.value : 0.0F;
        return true;
    }
    bool select_hotbar_slot(int slot) override { selected_slot = slot; return true; }
    bool rotate(float yaw, float pitch) override {
        if (throw_rotation) throw std::runtime_error("rotate");
        rotations.emplace_back(yaw, pitch);
        return rotate_result;
    }
    bool rotate_head_and_body(float yaw) override {
        if (throw_rotation) throw std::runtime_error("head and body rotate");
        head_body_rotations.push_back(yaw);
        if (rotate_head_and_body_result) {
            state.head_yaw = yaw;
            state.head_yaw_ready = true;
        }
        return rotate_head_and_body_result;
    }
    bool attack(std::uintptr_t actor_id) override {
        if (throw_attack) throw std::runtime_error("attack");
        attacks.push_back(actor_id);
        return attack_result;
    }
    bool set_sprinting(bool sprinting) override {
        sprint_requests.push_back(sprinting);
        return sprint_result;
    }
    bool set_velocity(Vec3 velocity) override {
        if (throw_velocity) throw std::runtime_error("velocity");
        velocity_requests.push_back(velocity);
        if (velocity_result) { state.local.velocity = velocity; velocity_events.push_back('v'); }
        return velocity_result;
    }
    KnockbackApplyResult apply_knockback(Vec3 velocity, std::int64_t received_timestamp, std::uint64_t input_tick = 0) override {
        if (!native_prediction) return GameplayAccess::apply_knockback(velocity, received_timestamp, input_tick);
        prediction_requests.emplace_back(velocity, received_timestamp);
        prediction_ticks.push_back(input_tick);
        if (!prediction_available) return KnockbackApplyResult::failed;
        velocity_events.push_back('p');
        return KnockbackApplyResult::predicted;
    }
    bool send_latency_reply(const LatencyReply& reply) override {
        if (latency_result) { latency_replies.push_back(reply); velocity_events.push_back('a'); }
        return latency_result;
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
    bool inventory_item(int slot, InventoryItemInfo& info) const override {
        if (slot < 0 || slot >= 36 || !inventory_readable) return false;
        info = inventory[static_cast<std::size_t>(slot)];
        return true;
    }
    bool inventory_screen_open() const override { return inventory_open; }
    bool swap_inventory_slots(int first, int second) override {
        swaps.emplace_back(first, second);
        if (swap_result && server_answers_swaps)
            std::swap(inventory[static_cast<std::size_t>(first)], inventory[static_cast<std::size_t>(second)]);
        return swap_result;
    }
    bool armor_item(int slot, InventoryItemInfo& info) const override {
        if (!armor_readable) return false;
        info = worn[static_cast<std::size_t>(slot)];
        return true;
    }
    bool equip_armor(int inventory_slot, int armor_slot) override {
        equips.emplace_back(inventory_slot, armor_slot);
        if (server_answers_equips)
            std::swap(inventory[static_cast<std::size_t>(inventory_slot)], worn[static_cast<std::size_t>(armor_slot)]);
        return true;
    }
    bool drop_inventory_slot(int slot) override {
        drops.push_back(slot);
        if (drop_result && server_answers_drops) inventory[static_cast<std::size_t>(slot)] = {};
        return drop_result;
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

void test_velocity_motion() {
    VelocityModule velocity;
    Vec3 motion{2.0F, 4.0F, -6.0F};
    using Result = VelocityModule::MotionResult;
    check(!velocity.enabled() && velocity.category() == Category::Movement &&
          velocity.process_motion(motion) == Result::ignored && near(motion.x, 2),
          "Disabled Velocity changed incoming motion");
    check(!velocity.setting_visible(velocity.horizontal) && !velocity.setting_visible(velocity.vertical),
          "Full mode showed Percent sliders");
    velocity.set_enabled(true);
    check(velocity.process_motion(motion) == Result::cancelled && near(motion.y, 4),
          "Full mode must cancel the packet without zeroing the player's existing movement");
    velocity.mode.get<Choice>().index = 1;
    check(velocity.setting_visible(velocity.horizontal) && velocity.setting_visible(velocity.vertical),
          "Percent sliders did not appear on mode change while enabled");
    velocity.horizontal.get<int>() = 25;
    velocity.vertical.get<int>() = 75;
    check(velocity.process_motion(motion) == Result::scaled && near(motion.x, 0.5F) &&
          near(motion.y, 3) && near(motion.z, -1.5F), "Percent did not scale X/Z separately from Y");
    velocity.horizontal.get<int>() = 100;
    velocity.vertical.get<int>() = 100;
    check(velocity.process_motion(motion) == Result::ignored && near(motion.x, 0.5F),
          "100 percent did not preserve incoming motion");
    velocity.horizontal.get<int>() = 0;
    velocity.vertical.get<int>() = 100;
    check(velocity.process_motion(motion) == Result::scaled && motion.x == 0 && motion.z == 0 && near(motion.y, 3),
          "Zero horizontal percent affected vertical motion");
    velocity.vertical.get<int>() = 0;
    check(velocity.process_motion(motion) == Result::scaled && motion.y == 0, "Zero percent retained knockback");
    motion = {2, 4, -6};
    velocity.horizontal.get<int>() = -10;
    velocity.vertical.get<int>() = 120;
    check(velocity.process_motion(motion) == Result::scaled && motion.x == 0 && near(motion.y, 4) && motion.z == 0,
          "Percent values escaped 0..100 bounds");
    motion.x = std::numeric_limits<float>::quiet_NaN();
    check(velocity.process_motion(motion) == Result::ignored, "Non-finite motion was processed");
    velocity.set_enabled(false);
    check(velocity.setting_visible(velocity.horizontal), "Disabled Percent mode hid its settings");
}

void test_velocity_delay_timing() {
    MockGame game;
    game.state.world_identity = 10;
    VelocityModule velocity;
    velocity.mode.get<Choice>().index = 2;
    velocity.set_enabled(true);
    using Result = VelocityModule::MotionResult;
    using namespace std::chrono_literals;
    const VelocityModule::TimePoint start{};
    Vec3 first{2, 4, -6}, second{-1, 0.5F, 3};
    check(velocity.delay_ms.get<int>() == 8000 && velocity.setting_visible(velocity.delay_ms) &&
          !velocity.setting_visible(velocity.horizontal) && !velocity.setting_visible(velocity.vertical),
          "Delay must default to 8000 ms and expose only its delay slider");
    check(velocity.handle_motion(first, game.state, start) == Result::delayed && near(first.x, 2) &&
          near(first.y, 4) && near(first.z, -6), "Delay changed the received knockback vector");
    check(velocity.handle_motion(second, game.state, start + 1s) == Result::delayed && velocity.pending_count() == 2,
          "A second hit replaced the first delayed knockback");
    velocity.update(game, true, start + 7999ms);
    check(game.velocity_requests.empty(), "Delayed knockback was applied before eight seconds");
    velocity.update(game, true, start + 8s);
    check(game.velocity_requests.size() == 1 && near(game.state.local.velocity.x, 2) &&
          near(game.state.local.velocity.y, 4) && near(game.state.local.velocity.z, -6) && velocity.pending_count() == 1,
          "A later hit reset the first hit's deadline or altered its motion");
    velocity.update(game, true, start + 8999ms);
    check(game.velocity_requests.size() == 1, "Second knockback did not keep its own deadline");
    velocity.update(game, true, start + 9s);
    check(game.velocity_requests.size() == 2 && near(game.state.local.velocity.x, -1) &&
          near(game.state.local.velocity.y, 0.5F) && velocity.pending_count() == 0,
          "Second delayed knockback was not applied unchanged");

    velocity.delay_ms.get<int>() = 1000;
    check(velocity.handle_motion(first, game.state, start + 10s) == Result::delayed, "Delay did not queue a hit");
    velocity.delay_ms.get<int>() = 50;
    check(velocity.handle_motion(second, game.state, start + 10100ms) == Result::delayed, "Shortened delay did not queue");
    velocity.update(game, true, start + 10200ms);
    check(game.velocity_requests.size() == 2, "A shortened delay overtook an earlier hit");
    velocity.update(game, true, start + 12s);
    check(game.velocity_requests.size() == 3 && near(game.state.local.velocity.x, 2) && velocity.pending_count() == 1,
          "Overdue knockbacks were drained in a burst");
    velocity.update(game, true, start + 12050ms);
    check(game.velocity_requests.size() == 4 && near(game.state.local.velocity.x, -1), "FIFO replay order changed");

    velocity.delay_ms.get<int>() = 0;
    check(velocity.handle_motion(first, game.state, start + 13s) == Result::ignored && velocity.pending_count() == 0,
          "Zero delay must use native dispatch immediately");
}

void test_velocity_unported_prediction() {
    MockGame game;
    game.state.world_identity = 10;
    VelocityModule velocity;
    velocity.mode.get<Choice>().index = 2;
    velocity.set_enabled(true);
    const VelocityModule::TimePoint start{};
    Vec3 motion{1, 2, 3};
    LatencyReply reply;
    game.state.deferred_knockback_supported = false;
    check(velocity.handle_motion(motion, game.state, start) == VelocityModule::MotionResult::ignored &&
          !velocity.handle_latency(reply, game.state) && velocity.pending_count() == 0,
          "An unported prediction ABI intercepted motion or latency");
    game.state.deferred_knockback_supported = true;
    check(velocity.handle_motion(motion, game.state, start) == VelocityModule::MotionResult::delayed &&
          velocity.handle_latency(reply, game.state), "Could not prepare the capability-change fixture");
    game.state.deferred_knockback_supported = false;
    velocity.update(game, true, start);
    check(velocity.pending_count() == 0 && velocity.pending_reply_count() == 0 &&
          game.velocity_requests.empty() && game.latency_replies.size() == 1,
          "Withholding prediction kept a stale impulse or latency reply");
    velocity.mode.get<Choice>().index = 0;
    check(velocity.handle_motion(motion, game.state, start) == VelocityModule::MotionResult::cancelled,
          "Prediction availability disabled the non-deferred Full mode");
}

void test_velocity_delay2_hit_count() {
    MockGame game;
    game.state.world_identity = 10;
    VelocityModule velocity;
    velocity.mode.get<Choice>().index = 3;
    velocity.set_enabled(true);
    using Result = VelocityModule::MotionResult;
    const VelocityModule::TimePoint start{};
    check(!velocity.setting_visible(velocity.delay_ms) && !velocity.setting_visible(velocity.sync_replies) &&
          !velocity.setting_visible(velocity.horizontal), "Delay2 must expose no extra settings");

    Vec3 hit{1, 2, 3}, stop{};
    for (int cycle = 0; cycle < 2; ++cycle) {
        for (int i = 0; i < 3; ++i) {
            check(velocity.handle_motion(hit, game.state, start) == Result::cancelled, "Delay2 did not ignore a hit");
            check(velocity.handle_motion(stop, game.state, start) == Result::ignored, "A velocity stop was cancelled");
        }
        check(velocity.handle_motion(hit, game.state, start) == Result::ignored && near(hit.x, 1) && near(hit.y, 2) && near(hit.z, 3),
              "The fourth hit must pass through unchanged");
    }
    velocity.update(game, true, start);
    check(game.velocity_requests.empty() && velocity.pending_count() == 0, "Delay2 queued or replayed a hit");

    LatencyReply reply;
    check(velocity.handle_motion(hit, game.state, start) == Result::cancelled && !velocity.handle_latency(reply, game.state),
          "Delay2 held a latency reply");

    velocity.reset_delay();
    for (int i = 0; i < 3; ++i)
        check(velocity.handle_motion(hit, game.state, start) == Result::cancelled, "A reset did not restart the count");
    velocity.set_enabled(false);
    velocity.set_enabled(true);
    check(velocity.handle_motion(hit, game.state, start) == Result::cancelled, "Disabling Velocity kept the Delay2 count");
}

void test_velocity_delay_reset_and_failures() {
    MockGame game;
    game.state.world_identity = 10;
    VelocityModule velocity;
    velocity.mode.get<Choice>().index = 2;
    velocity.set_enabled(true);
    using Result = VelocityModule::MotionResult;
    using namespace std::chrono_literals;
    const VelocityModule::TimePoint start{};
    Vec3 motion{2, 4, -6};
    const auto queue = [&] {
        check(velocity.handle_motion(motion, game.state, start) == Result::delayed && velocity.pending_count() == 1,
              "Could not queue delayed knockback");
    };
    const auto reset = [&](bool allow_actions = true) {
        velocity.update(game, allow_actions, start + 9s);
        check(velocity.pending_count() == 0 && game.velocity_requests.empty(),
              "Stale or blocked delayed knockback was applied");
    };
    queue(); velocity.set_enabled(false);
    check(velocity.pending_count() == 0, "Disable retained delayed knockback");
    velocity.set_enabled(true);
    queue(); velocity.mode.get<Choice>().index = 0; reset();
    velocity.mode.get<Choice>().index = 2;
    queue(); reset(false);
    queue(); game.state.ready = false; reset(); game.state.ready = true;
    queue(); game.state.local.alive = false; reset(); game.state.local.alive = true;
    queue(); ++game.state.world_identity; reset();
    queue(); ++game.state.local.id; reset();
    queue(); ++game.state.local.unique_id; reset();
    queue(); velocity.reset_delay(); reset();
    game.state.world_identity = 0;
    check(velocity.handle_motion(motion, game.state, start) == Result::ignored, "Missing world identity queued a hit");
    game.state.world_identity = 10;
    Vec3 invalid{0, std::numeric_limits<float>::infinity(), 0};
    check(velocity.handle_motion(invalid, game.state, start) == Result::ignored, "Non-finite motion entered the queue");
    queue();
    game.velocity_result = false;
    velocity.update(game, true, start + 8s);
    check(velocity.pending_count() == 1 && game.state.local.velocity.x == 0, "A failed native write lost the queued hit");
    game.velocity_result = true;
    velocity.update(game, true, start + 8050ms);
    check(velocity.pending_count() == 0 && near(game.state.local.velocity.x, 2), "Failed write did not retry next tick");
    queue(); game.throw_velocity = true;
    velocity.update(game, true, start + 9s);
    check(velocity.pending_count() == 0, "Throwing native action retained a stale queue");
}

void test_velocity_delay_capacity() {
    MockGame game;
    game.state.world_identity = 10;
    VelocityModule velocity;
    velocity.mode.get<Choice>().index = 2;
    velocity.set_enabled(true);
    using Result = VelocityModule::MotionResult;
    Vec3 motion{1, 2, 3};
    const VelocityModule::TimePoint start{};
    for (std::size_t i = 0; i < VelocityModule::maximum_pending; ++i)
        check(velocity.handle_motion(motion, game.state, start) == Result::delayed, "Delay queue filled too early");
    check(velocity.handle_motion(motion, game.state, start) == Result::ignored &&
          velocity.pending_count() == VelocityModule::maximum_pending,
          "Overflow must pass through without growing or replacing the queue");
    velocity.update(game, true, start + std::chrono::seconds(30));
    check(game.velocity_requests.size() == 1 && velocity.pending_count() == VelocityModule::maximum_pending - 1,
          "A stalled tick replayed every overdue hit at once");
}

void test_velocity_zero_motion_is_immediate() {
    MockGame game;
    game.state.world_identity = 10;
    VelocityModule velocity;
    velocity.mode.get<Choice>().index = 2;
    velocity.set_enabled(true);
    using Result = VelocityModule::MotionResult;
    using namespace std::chrono_literals;
    const VelocityModule::TimePoint start{};
    LatencyReply reply;
    reply.bytes[0x30] = std::byte{7};
    Vec3 motion{1, 2, 3};
    check(velocity.handle_motion(motion, game.state, start) == Result::delayed &&
          velocity.handle_latency(reply, game.state) && velocity.pending_count() == 1,
          "Zero-motion fixture could not queue a hit and its acknowledgment");
    // A zero SetActorMotion is a server velocity stop, not a knockback. It has
    // to reach the native handler immediately and retire older delayed hits.
    Vec3 stop{};
    check(velocity.handle_motion(stop, game.state, start + 1s) == Result::ignored &&
          velocity.pending_count() == 0 && velocity.pending_reply_count() == 1 &&
          velocity.diagnostics().zero_motion_stops == 1 &&
          velocity.diagnostics().invalidated_motion == 1,
          "Zero motion was delayed or did not retire the older knockback");
    velocity.update(game, true, start + 2s);
    check(game.velocity_requests.empty() && game.latency_replies.size() == 1 &&
          game.latency_replies[0].bytes == reply.bytes && !velocity.needs_restore(),
          "Zero-motion cleanup lost or reordered the held acknowledgment");
    // Repeated stops are still immediate and never become queued hits.
    check(velocity.handle_motion(stop, game.state, start + 3s) == Result::ignored &&
          velocity.pending_count() == 0 && velocity.diagnostics().zero_motion_stops == 2,
          "A stop without pending knockback entered the delay queue");
    Vec3 nearly_zero{1.0e-7F, -1.0e-7F, 0};
    check(velocity.handle_motion(nearly_zero, game.state, start + 4s) == Result::ignored &&
          velocity.pending_count() == 0 && velocity.diagnostics().zero_motion_stops == 3,
          "Near-zero motion was treated as knockback");
    Vec3 tiny{1.0e-4F, 0, 0};
    check(velocity.handle_motion(tiny, game.state, start + 5s) == Result::delayed &&
          velocity.pending_count() == 1,
          "A small but non-zero impulse was discarded as a stop");
}

void test_velocity_latency_ordering() {
    MockGame game;
    game.state.world_identity = 10;
    VelocityModule velocity;
    velocity.mode.get<Choice>().index = 2;
    velocity.set_enabled(true);
    using namespace std::chrono_literals;
    const VelocityModule::TimePoint start{};
    LatencyReply first, second, third;
    // Opaque, non-monotonic server IDs must survive holding and send retries.
    const std::uint64_t first_id = 0xFEDCBA9876543210ULL, second_id = 1000, third_id = UINT64_MAX;
    std::memcpy(first.bytes.data() + 0x30, &first_id, sizeof first_id);
    std::memcpy(second.bytes.data() + 0x30, &second_id, sizeof second_id);
    std::memcpy(third.bytes.data() + 0x30, &third_id, sizeof third_id);
    Vec3 motion{1, 2, 3};
    check(!velocity.handle_latency(first, game.state), "Unrelated latency was delayed before a hit");
    std::uint64_t passed_id{};
    std::memcpy(&passed_id, first.bytes.data() + 0x30, sizeof passed_id);
    check(passed_id == first_id && velocity.pending_reply_count() == 0 && game.latency_replies.empty(),
          "Enabling Delay changed or generated an unrelated latency reply before any KB");
    check(velocity.handle_motion(motion, game.state, start) == VelocityModule::MotionResult::delayed,
          "Could not queue the first KB");
    check(velocity.handle_latency(first, game.state), "Latency following KB was passed through");
    check(velocity.handle_motion(motion, game.state, start + 2s) == VelocityModule::MotionResult::delayed &&
          velocity.handle_latency(second, game.state), "Second KB and its acknowledgment were not queued");
    velocity.update(game, true, start + 7999ms);
    check(game.latency_replies.empty(), "An acknowledgment overtook held KB");
    velocity.update(game, true, start + 8s);
    check(game.velocity_requests.size() == 1 && game.latency_replies.size() == 1 &&
          game.velocity_events == std::vector<char>{'v', 'a'},
          "The applied KB was left unconfirmed across the next movement report");
    velocity.update(game, true, start + 8050ms);
    check(game.latency_replies.size() == 1 && game.latency_replies[0].bytes == first.bytes &&
          velocity.pending_reply_count() == 1, "Replies lost their identity or confirmed the second KB early");
    game.latency_result = false;
    velocity.update(game, true, start + 10s);
    check(game.latency_replies.size() == 1 && velocity.pending_count() == 0,
          "Failed confirmation send changed pending KB or lost reply identity");
    velocity.sync_replies.get<bool>() = false;
    check(velocity.handle_latency(third, game.state), "Turning Sync Replies OFF let a new reply overtake the last held reply");
    game.latency_result = true;
    velocity.update(game, true, start + 10050ms);
    check(game.latency_replies.size() == 3 && game.latency_replies[1].bytes == second.bytes &&
          game.latency_replies[2].bytes == third.bytes && !velocity.needs_restore() &&
          game.velocity_events == std::vector<char>{'v', 'a', 'v', 'a', 'a'}, "Latency FIFO replay failed");
    check(velocity.diagnostics().queued == 2 && velocity.diagnostics().applied == 2 &&
          velocity.diagnostics().latency_seen == 4 && velocity.diagnostics().latency_sent == 3,
          "Velocity diagnostics lost queue or acknowledgment events");
}

void test_velocity_latency_cleanup_and_retry() {
    MockGame game;
    game.state.world_identity = 10;
    VelocityModule velocity;
    velocity.mode.get<Choice>().index = 2;
    velocity.set_enabled(true);
    using namespace std::chrono_literals;
    const VelocityModule::TimePoint start{};
    LatencyReply reply;
    Vec3 motion{1, 2, 3};
    const auto queue = [&] {
        check(velocity.handle_motion(motion, game.state, start) == VelocityModule::MotionResult::delayed &&
              velocity.handle_latency(reply, game.state), "Could not queue KB plus its reply");
    };
    queue();
    game.velocity_result = false;
    velocity.update(game, true, start + 8s);
    check(game.latency_replies.empty() && velocity.pending_reply_count() == 1, "Failed KB write confirmed receipt");
    game.velocity_result = true;
    game.latency_result = false;
    velocity.update(game, true, start + 8050ms);
    check(velocity.needs_restore() && velocity.diagnostics().send_failures == 1, "Failed send lost a held reply");
    game.latency_result = true;
    velocity.update(game, true, start + 8150ms);
    check(!velocity.needs_restore() && game.latency_replies.size() == 1, "Held reply send was not retried");
    queue();
    velocity.set_enabled(false);
    check(velocity.pending_count() == 0 && velocity.needs_restore(), "OFF discarded network cleanup");
    check(velocity.handle_latency(reply, game.state), "A new reply overtook cleanup after OFF");
    velocity.update(game, false, start + 1ms);
    check(!velocity.needs_restore() && game.latency_replies.size() == 3,
          "OFF/input capture did not flush replies on the game thread");
    velocity.set_enabled(true);
    queue();
    velocity.set_enabled(false);
    velocity.set_enabled(true);
    velocity.update(game, true, start + 1ms);
    check(!velocity.needs_restore(), "Fast OFF/ON stranded replies for cancelled KB");
    queue();
    ++game.state.world_identity;
    velocity.update(game, true, start + 9s);
    check(!velocity.needs_restore() && velocity.pending_count() == 0 && game.latency_replies.size() == 4,
          "Replies from an old world were replayed in a new context");
    velocity.sync_replies.get<bool>() = false;
    check(velocity.handle_motion(motion, game.state, start) == VelocityModule::MotionResult::delayed &&
          !velocity.handle_latency(reply, game.state), "Sync Replies OFF still held latency");
}

void test_velocity_position_reset_retires_old_hits() {
    MockGame game;
    game.state.world_identity = 10;
    VelocityModule velocity;
    velocity.mode.get<Choice>().index = 2;
    velocity.set_enabled(true);
    using namespace std::chrono_literals;
    const VelocityModule::TimePoint start{};
    Vec3 a{0.1F, 0.4F, 0.3F}, b{0.2F, 0.4F, 0.2F}, c{0.3F, 0.4F, 0.1F}, fresh{-0.4F, 0.4F, 0};
    LatencyReply first, second, third, fourth;
    first.bytes[0x30] = std::byte{1};
    second.bytes[0x30] = std::byte{2};
    third.bytes[0x30] = std::byte{3};
    fourth.bytes[0x30] = std::byte{4};
    check(velocity.handle_motion(a, game.state, start) == VelocityModule::MotionResult::delayed &&
          velocity.handle_latency(first, game.state) &&
          velocity.handle_motion(b, game.state, start + 1s) == VelocityModule::MotionResult::delayed &&
          velocity.handle_latency(second, game.state) &&
          velocity.handle_motion(c, game.state, start + 2s) == VelocityModule::MotionResult::delayed &&
          velocity.handle_latency(third, game.state), "Position-reset fixture did not queue three hits");
    velocity.update(game, true, start + 8s);
    check(game.velocity_requests.size() == 1 && game.latency_replies.size() == 1,
          "First hit did not precede the simulated server setback");
    check(velocity.handle_position_reset(0) == 0 && velocity.handle_position_reset(3) == 0 &&
          velocity.handle_position_reset(255) == 0 && velocity.pending_count() == 2 &&
          velocity.diagnostics().position_resets == 0, "Normal/head-only/unknown modes cancelled delayed hits");
    check(velocity.handle_position_reset(2) == 2 && velocity.pending_count() == 0 &&
          velocity.pending_reply_count() == 2 && velocity.needs_restore(),
          "Teleport retained stale hits or discarded confirmations needed for cleanup");
    // A setback in the same world and actor must invalidate old hits without
    // clearing a new hit received after the native teleport.
    game.state.local.position = {10, 70, 20};
    check(velocity.handle_motion(fresh, game.state, start + 8100ms) == VelocityModule::MotionResult::delayed &&
          velocity.handle_latency(fourth, game.state), "Post-teleport hit did not queue");
    game.latency_result = false;
    velocity.update(game, true, start + 8200ms);
    check(game.velocity_requests.size() == 1 && velocity.pending_reply_count() == 3,
          "Failed cleanup send applied an old hit or lost a held reply");
    game.latency_result = true;
    velocity.update(game, true, start + 8250ms);
    check(game.latency_replies.size() == 3 && game.latency_replies[1].bytes == second.bytes &&
          game.latency_replies[2].bytes == third.bytes && velocity.pending_reply_count() == 1,
          "Reset cleanup reordered old replies or confirmed a new hit too early");
    velocity.update(game, true, start + 16099ms);
    check(game.velocity_requests.size() == 1 && game.latency_replies.size() == 3,
          "A pre-teleport hit was replayed or a fresh hit was released early");
    velocity.update(game, true, start + 16100ms);
    check(game.velocity_requests.size() == 2 && near(game.velocity_requests.back().x, fresh.x) &&
          game.latency_replies.size() == 4 && game.latency_replies.back().bytes == fourth.bytes,
          "Fresh KB/confirmation did not survive the reset of an older burst");
    check(velocity.handle_motion(a, game.state, start + 17s) == VelocityModule::MotionResult::delayed &&
          velocity.handle_latency(first, game.state) && velocity.handle_position_reset(1) == 1,
          "Respawn did not invalidate its old hit");
    velocity.set_enabled(false);
    velocity.update(game, false, start + 17100ms);
    check(game.velocity_requests.size() == 2 && game.latency_replies.size() == 5 && !velocity.needs_restore() &&
          velocity.diagnostics().position_resets == 2 && velocity.diagnostics().invalidated_motion == 3,
          "Reset cleanup after OFF replayed old KB, stranded replies or lost diagnostics");
}

void test_velocity_simultaneous_release() {
    MockGame game;
    game.state.world_identity = 10;
    VelocityModule velocity;
    velocity.mode.get<Choice>().index = 2;
    velocity.set_enabled(true);
    using namespace std::chrono_literals;
    const VelocityModule::TimePoint start{};
    LatencyReply first, second;
    first.bytes[0x30] = std::byte{1};
    second.bytes[0x30] = std::byte{2};
    Vec3 first_motion{1, 2, 3}, second_motion{4, 5, 6};
    check(velocity.handle_motion(first_motion, game.state, start) == VelocityModule::MotionResult::delayed &&
          velocity.handle_latency(first, game.state) &&
          velocity.handle_motion(second_motion, game.state, start) == VelocityModule::MotionResult::delayed &&
          velocity.handle_latency(second, game.state), "Could not queue simultaneous KB confirmations");
    velocity.update(game, true, start + 8s);
    check(game.velocity_requests.size() == 1 && game.latency_replies.size() == 1 &&
          near(game.state.local.velocity.x, 1) && game.latency_replies[0].bytes == first.bytes &&
          velocity.pending_count() == 1 && velocity.pending_reply_count() == 1,
          "Same-deadline KB overwrote the first impulse or acknowledged the second early");
    velocity.update(game, true, start + 8050ms);
    check(game.velocity_requests.size() == 2 && game.latency_replies.size() == 2 &&
          near(game.state.local.velocity.x, 4) && game.latency_replies[1].bytes == second.bytes &&
          game.velocity_events == std::vector<char>{'v', 'a', 'v', 'a'},
          "Consecutive KB applications were separated from their confirmations");
}

void test_velocity_prediction_timestamps() {
    MockGame game;
    game.state.world_identity = 10;
    game.native_prediction = true;
    VelocityModule velocity;
    velocity.mode.get<Choice>().index = 2;
    velocity.set_enabled(true);
    using namespace std::chrono_literals;
    const VelocityModule::TimePoint start{};
    LatencyReply first, second;
    first.bytes[0x30] = std::byte{1};
    second.bytes[0x30] = std::byte{2};
    Vec3 a{1, 0.4F, 2}, b{-1, 0.4F, 3};
    check(velocity.handle_motion(a, game.state, start, 1000000000000LL, 111) == VelocityModule::MotionResult::delayed &&
          velocity.handle_latency(first, game.state) &&
          velocity.handle_motion(b, game.state, start + 2s, 1002000000000LL, 222) == VelocityModule::MotionResult::delayed &&
          velocity.handle_latency(second, game.state), "Native timestamp KB setup failed");
    game.prediction_available = false;
    velocity.update(game, true, start + 8s);
    check(game.prediction_requests.size() == 1 && game.prediction_requests[0].second == 1008000000000LL &&
          game.latency_replies.empty() && game.velocity_requests.empty(),
          "Prediction failure wrote StateVector, returned confirmation, or retained the old arrival timestamp");
    game.prediction_available = true;
    velocity.update(game, true, start + 8050ms);
    check(game.prediction_requests.size() == 2 && game.prediction_requests[1].second == 1008050000000LL &&
          game.latency_replies.size() == 1 && velocity.pending_count() == 1,
          "A retry reused a stale prediction timestamp or confirmed a later hit");
    velocity.update(game, true, start + 10s);
    check(game.prediction_requests.size() == 3 && game.prediction_requests[2].second == 1010000000000LL &&
          near(game.prediction_requests[2].first.x, -1) && game.velocity_requests.empty() &&
          game.velocity_events == std::vector<char>{'p', 'a', 'p', 'a'} &&
          game.prediction_ticks == std::vector<std::uint64_t>{111,111,222} &&
          velocity.diagnostics().predicted == 2 && velocity.diagnostics().direct == 0,
          "Native prediction was replaced with direct writes or lost per-hit timing/confirmation order");
    velocity.reset_delay();
    check(velocity.handle_motion(a, game.state, start, (std::numeric_limits<std::int64_t>::max)() - 1) ==
          VelocityModule::MotionResult::delayed, "Timestamp overflow fixture setup failed");
    velocity.update(game, true, start + 8s);
    check(game.prediction_requests.size() == 3 && velocity.pending_count() == 1 &&
          velocity.diagnostics().write_failures == 2, "Overflowing native timestamp reached prediction");
}

void test_disabled_and_unavailable() {
    MockGame game;
    game.state.actors.push_back(game.target(2, 0.0F, 2.0F));
    RotationsModule rotations;
    DerpModule derp;
    AimbotModule aimbot;
    KillauraModule aura;
    ScaffoldModule scaffold;
    GameplayModule* modules[] = {&derp, &aimbot, &aura, &scaffold};
    check(derp.category() == Category::Player && aimbot.category() == Category::Player &&
          aura.category() == Category::Player && scaffold.category() == Category::Movement,
          "Gameplay module categories changed");
    check(rotations.category() == Category::Misc && !rotations.enabled() &&
          rotations.settings().size() == 2 && !rotations.always.get<bool>(),
          "Rotations must start disabled with Always off");
    for (auto* module : modules) {
        check(!module->enabled(), "Gameplay modules must be disabled by default");
        module->game_tick(game, 0.05F);
        module->set_enabled(true);
    }
    check(game.rotations.empty() && game.head_body_rotations.empty() && game.attacks.empty() && game.placements.empty(),
          "Disabled modules touched the game");

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
    check(game.rotations.empty() && game.head_body_rotations.empty() && game.attacks.empty() && game.placements.empty(),
          "Invalid frames or deltas caused an action");
}

void test_derp_spin_and_reset() {
    MockGame game;
    game.state.yaw = 170.0F;
    DerpModule derp;
    derp.set_enabled(true);
    derp.game_tick(game, 0.05F);
    check(game.head_body_rotations.size() == 1 && near(game.head_body_rotations.back(), -172.0F) &&
          game.rotations.empty(), "Derp changed view rotation instead of head/body yaw");
    derp.game_tick(game, 0.05F);
    check(near(game.head_body_rotations.back(), -154.0F), "Derp did not accumulate head/body spin");

    derp.set_enabled(false);
    game.state.yaw = 20.0F;
    game.state.head_yaw_ready = false;
    derp.set_enabled(true);
    derp.game_tick(game, 0.05F);
    check(near(game.head_body_rotations.back(), 38.0F) && game.rotations.empty(),
          "Derp retained state or changed view rotation after disabling");
    game.state.local.id = 7;
    game.state.yaw = 100.0F;
    game.state.head_yaw = 100.0F;
    game.state.head_yaw_ready = true;
    derp.game_tick(game, 0.05F);
    check(near(game.head_body_rotations.back(), 118.0F), "Derp retained another player's yaw");
    game.state.ready = false;
    derp.game_tick(game, 0.05F);
    game.state.ready = true;
    game.state.yaw = 0.0F;
    game.state.head_yaw_ready = false;
    derp.game_tick(game, 0.05F);
    check(near(game.head_body_rotations.back(), 18.0F), "Derp retained state across an unavailable world");

    game.rotate_head_and_body_result = false;
    derp.game_tick(game, 0.05F);
    game.rotate_head_and_body_result = true;
    derp.game_tick(game, 0.05F);
    check(near(game.head_body_rotations.back(), 36.0F), "Derp advanced after a rejected head/body rotation");
    game.throw_rotation = true;
    derp.game_tick(game, 0.05F);
    game.throw_rotation = false;
    game.state.yaw = 30.0F;
    game.state.head_yaw_ready = false;
    derp.game_tick(game, 0.05F);
    check(near(game.head_body_rotations.back(), 48.0F), "Derp did not recover after a bridge exception");
}

void test_antibot_checks() {
    AntiBotModule antibot;
    ActorSnapshot player;
    player.player = true;
    player.bounds = {{0.0F, 64.0F, 0.0F}, {0.6F, 65.8F, 0.6F}};
    player.armor_pieces = 0;
    check(!antibot.is_bot(player), "Disabled AntiBot flagged a player");
    auto mob = player;
    mob.player = false;
    check(!antibot.is_bot(mob), "Disabled AntiBot flagged a non-player");

    antibot.set_enabled(true);
    check(!antibot.is_bot(player), "AntiBot flagged a normal player");
    check(antibot.is_bot(mob), "Simple mode kept a non-player");
    auto sneaking = player;
    sneaking.bounds.max.y = 65.49F;
    check(!antibot.is_bot(sneaking), "AntiBot flagged a sneaking player");
    auto wide = player;
    wide.bounds.max.x = 1.0F;
    check(antibot.is_bot(wide), "Simple mode kept a wrong-width hitbox");
    auto tall = player;
    tall.bounds.max.y = 66.0F;
    check(antibot.is_bot(tall), "Simple mode kept a wrong-height hitbox");
    auto invisible = player;
    invisible.invisible = true;
    check(antibot.is_bot(invisible), "Simple mode kept an invisible actor");
    auto titled = player;
    titled.multiline_name = true;
    check(antibot.is_bot(titled), "Simple mode kept a multi-line nametag");

    antibot.armor_check.get<bool>() = true;
    check(!antibot.is_bot(player), "Simple mode applied the armor check");
    check(!antibot.setting_visible(antibot.hitbox_check), "Simple mode showed a Custom setting");
    antibot.mode.get<Choice>().index = 1;
    check(antibot.setting_visible(antibot.armor_mode), "Custom mode hid Armor Mode");
    antibot.player_check.get<bool>() = false;
    antibot.hitbox_check.get<bool>() = false;
    antibot.invisible_check.get<bool>() = false;
    antibot.name_check.get<bool>() = false;
    antibot.armor_check.get<bool>() = false;
    check(!antibot.setting_visible(antibot.armor_mode), "Armor Mode stayed visible without its check");
    check(!antibot.is_bot(mob) && !antibot.is_bot(wide) && !antibot.is_bot(invisible) && !antibot.is_bot(titled),
          "Custom mode ignored disabled checks");

    antibot.armor_check.get<bool>() = true;
    check(antibot.is_bot(player), "Armor check kept an unarmored player");
    auto partial = player;
    partial.armor_pieces = 3;
    check(antibot.is_bot(partial), "Full armor mode kept a partial set");
    antibot.armor_mode.get<Choice>().index = 1;
    check(!antibot.is_bot(partial), "One Element armor mode rejected a partial set");
    auto unread = player;
    unread.armor_pieces = -1;
    check(!antibot.is_bot(unread), "Unreadable armor counted as missing");
    check(antibot.is_bot(mob), "Armor check kept a non-player");
}

void test_bots_are_not_targeted() {
    MockGame game;
    KillauraModule killaura;
    killaura.set_enabled(true);
    killaura.cps.get<float>() = 20.0F;
    auto bot = game.target(2, 0.0F, 2.0F);
    bot.bot = true;
    game.state.actors = {bot, game.target(3, 0.0F, 3.0F)};
    killaura.game_tick(game, 0.05F);
    check(game.attacks.size() == 1 && game.attacks.back() == 3, "Killaura attacked a marked bot");
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
    check(game.head_body_rotations.empty(), "Aimbot ignored Require Click");
    game.state.attack_down = true;
    aimbot.game_tick(game, 0.05F);
    const float expected = 20.0F * (1.0F - std::exp(-8.0F * 0.05F));
    check(game.head_body_rotations.size() == 1 && near(game.head_body_rotations.back(), expected) &&
          game.rotations.empty(), "Aimbot did not smoothly aim with head/body yaw only");

    game.state.actors = {game.target(6, 60.0F, 3.0F)};
    aimbot.game_tick(game, 0.05F);
    check(game.head_body_rotations.size() == 1, "Aimbot selected a target outside its cone");
    aimbot.require_click.get<bool>() = false;
    game.state.attack_down = false;
    game.state.actors = {game.target(7, 10.0F, 2.0F, false)};
    game.state.actors.front().visible = false;
    aimbot.players_only.get<bool>() = false;
    aimbot.game_tick(game, 0.05F);
    check(game.head_body_rotations.size() == 1, "Aimbot ignored visibility filtering");
    aimbot.visible_only.get<bool>() = false;
    aimbot.game_tick(game, 0.05F);
    check(game.head_body_rotations.size() == 2 && game.head_body_rotations.back() > 0.0F,
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
    check(game.head_body_rotations.size() == 1 && game.head_body_rotations.back() > 179.0F &&
          game.head_body_rotations.back() < 180.0F,
          "Aimbot took the long path at the yaw wrap");

    game.state.yaw = 0.0F;
    auto target = game.target(3, 0.0F, 8.0F);
    target.bounds = {{0.2F, 64.0F, 2.5F}, {0.8F, 65.8F, 8.5F}};
    game.state.actors = {target};
    aimbot.range.get<float>() = 3.0F;
    aimbot.game_tick(game, 0.05F);
    check(game.head_body_rotations.size() == 2,
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
    check(game.head_body_rotations.size() == 3 && game.head_body_rotations.back() > 0.0F,
          "Aimbot FOV used yaw/pitch distance instead of a spatial cone");

    game.state.actors.front().position.x = std::numeric_limits<float>::quiet_NaN();
    aimbot.game_tick(game, 0.05F);
    check(game.head_body_rotations.size() == 3, "Aimbot selected non-finite target data");
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
    check(game.attacks == std::vector<std::uintptr_t>{2} && near(game.head_body_rotations.back(), 35.0F) &&
          game.rotations.empty(), "Killaura changed view rotation or failed to turn head/body");
    aura.game_tick(game, 0.05F);
    check(game.attacks.size() == 1, "Killaura exceeded its CPS limit");
    aura.game_tick(game, 0.05F);
    check(game.attacks.size() == 2, "Killaura did not attack after the cooldown");
    aura.game_tick(game, 100.0F);
    check(game.attacks.size() == 3, "Killaura replayed missed attacks as a burst");
    aura.game_tick(game, 0.001F);
    check(game.attacks.size() == 3, "Killaura carried missed attacks into the next tick");

    aura.set_enabled(false);
    aura.rotate.get<Choice>().index = 0;
    aura.set_enabled(true);
    const auto head_body_rotation_count = game.head_body_rotations.size();
    aura.game_tick(game, 0.01F);
    check(game.attacks.size() == 4 && game.head_body_rotations.size() == head_body_rotation_count,
          "Killaura failed to reset or ignored Rotate");
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
    aura.rotate.get<Choice>().index = 0;
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

void test_killaura_modes() {
    MockGame game;
    KillauraModule aura;
    aura.set_enabled(true);
    aura.cps.get<float>() = 20.0F;
    check(aura.mode.get<Choice>().index == 0 && aura.priority.get<Choice>().index == 0,
          "Killaura must default to Switch by Distance");
    auto near_actor = game.target(2, 0.0F, 1.0F), far_actor = game.target(3, 90.0F, 3.0F);
    near_actor.health = 20.0F;
    far_actor.health = 4.0F;
    game.state.actors = {near_actor, far_actor};

    // Switch re-evaluates every tick; unknown health falls back to distance.
    aura.game_tick(game, 0.05F);
    check(game.attacks.back() == 2, "Switch did not pick the nearest actor");
    aura.priority.get<Choice>().index = 1;
    aura.game_tick(game, 0.05F);
    check(game.attacks.back() == 3, "Switch did not change to the lowest-health actor");
    game.state.actors.back().health = -1.0F;
    aura.game_tick(game, 0.05F);
    check(game.attacks.back() == 2, "Health priority preferred an unknown health");
    game.state.actors.front().health = -1.0F;
    aura.game_tick(game, 0.05F);
    check(game.attacks.back() == 2, "Unknown health did not fall back to distance");

    // Single keeps the lock against a better target, and without Rotate.
    aura.mode.get<Choice>().index = 1;
    aura.priority.get<Choice>().index = 0;
    aura.rotate.get<Choice>().index = 0;
    game.state.actors = {far_actor};
    aura.game_tick(game, 0.05F);
    game.state.actors = {near_actor, far_actor};
    aura.game_tick(game, 0.05F);
    check(game.attacks.back() == 3, "Single released its target for a better one");
    game.state.actors.back().alive = false;
    aura.game_tick(game, 0.05F);
    check(game.attacks.back() == 2, "Single kept an ineligible target");
    game.state.actors.back().alive = true;
    aura.game_tick(game, 0.05F);
    check(game.attacks.back() == 2, "Single did not keep its replacement target");

    // Multi hits everyone once per attack interval and rotates to the best one.
    aura.mode.get<Choice>().index = 2;
    aura.rotate.get<Choice>().index = 1;
    auto bot = game.target(4, 0.0F, 1.5F);
    bot.bot = true;
    game.state.actors = {far_actor, near_actor, bot};
    game.attacks.clear();
    aura.game_tick(game, 0.05F);
    check(game.attacks == std::vector<std::uintptr_t>{3, 2}, "Multi did not attack each eligible actor once");
    check(near(game.head_body_rotations.back(), 0.0F), "Multi did not rotate to the best target");
    aura.cps.get<float>() = 10.0F;
    aura.game_tick(game, 0.05F);
    aura.game_tick(game, 0.05F);
    check(game.attacks.size() == 4, "Multi attacked again inside the CPS interval");
}

void test_rotations_vertical_render() {
    RotationsModule rotations;
    RotationsModule::RenderAngles angles;
    rotations.set_server_rotation(20, -50);
    check(!rotations.update_render(1, 20, 10, 0.05F, angles), "Disabled Rotations rendered a pitch request");
    rotations.set_enabled(true);
    check(!rotations.update_render(1, 20, 10, 0.05F, angles), "Enabling Rotations reused a stale angle request");
    rotations.set_server_rotation(20, -50);
    check(rotations.update_render(1, 20, 10, 0, angles) && near(angles.pitch, 10),
          "Rotations did not start vertical interpolation at the current view");
    check(rotations.update_render(1, 20, 10, 1.0F / 60, angles) && near(angles.yaw, 20) &&
          angles.pitch < 10 && angles.pitch > -50, "Pitch-only requests did not animate independently of yaw");
    for (int i = 0; i < 180; ++i) rotations.update_render(1, 20, 10, 1.0F / 60, angles);
    check(near(angles.pitch, -50), "Rotations did not converge to its shared server pitch");
    rotations.set_server_rotation(20, 65);
    rotations.update_render(1, 20, 10, 1.0F / 60, angles);
    check(angles.pitch > -50 && angles.pitch < 65, "Rotations failed to follow a downward pitch request");
    rotations.update_render(2, -30, 35, 0, angles);
    check(near(angles.pitch, 35) && near(angles.yaw, -30), "Another actor inherited the previous model's rendered angles");
    rotations.reset_render();
    rotations.update_render(2, 40, -15, 0, angles);
    check(near(angles.pitch, -15), "View/world reset retained a stale rendered pitch");
    rotations.set_enabled(false);
    check(!rotations.update_render(2, 40, -15, 0.05F, angles), "Disabling Rotations left vertical rendering active");
    rotations.set_enabled(true);
    rotations.set_server_rotation(-179, 25);
    rotations.update_render(1, 179, 25, 0, angles);
    rotations.update_render(1, 179, 25, 1.0F / 60, angles);
    check(angles.yaw > 179 && near(angles.pitch, 25), "Rotations lost shortest-path yaw interpolation while drawing pitch");
    rotations.server_pitch.store(std::numeric_limits<float>::quiet_NaN());
    check(!rotations.update_render(1, 179, 25, 0.05F, angles), "Rotations accepted a non-finite render pitch");
    RotationsModule slow, fast;
    slow.set_enabled(true); fast.set_enabled(true);
    slow.speed.get<float>() = 240; fast.speed.get<float>() = 1440;
    slow.set_server_rotation(0, 60); fast.set_server_rotation(0, 60);
    RotationsModule::RenderAngles slow_angles, fast_angles;
    slow.update_render(1, 0, 0, 1.0F / 60, slow_angles);
    fast.update_render(1, 0, 0, 1.0F / 60, fast_angles);
    check(fast_angles.pitch > slow_angles.pitch && slow_angles.pitch > 0,
          "Rotations Speed did not control vertical render interpolation");
}

void test_sprint_input_and_release() {
    MockGame game;
    SprintModule sprint;
    sprint.update(game, true);
    check(game.sprint_requests.empty(), "Disabled Sprint changed native input");
    sprint.set_enabled(true);
    game.state.move_input_ready = true;
    game.state.move_input = {0, 1};
    sprint.update(game, true);
    check(game.sprint_requests.back() && sprint.requested_sprinting() && sprint.packet_sprint_override(),
          "Sprint failed to start from rest on forward input");
    game.state.move_input = {1, 0};
    sprint.update(game, true);
    check(game.sprint_requests.back(), "Rage failed on strafe input");
    game.state.move_input = {0, -1};
    sprint.update(game, true);
    check(game.sprint_requests.back(), "Rage failed on backward input");
    sprint.mode.get<Choice>().index = 1;
    sprint.update(game, true);
    check(!game.sprint_requests.back(), "Legit sprinted on backward input");
    game.state.move_input = {1, 0};
    sprint.update(game, true);
    check(!game.sprint_requests.back(), "Legit sprinted on strafe-only input");
    game.state.yaw = 179;
    game.state.move_input = {1, 1};
    sprint.update(game, true);
    check(game.sprint_requests.back(), "Legit rejected forward-diagonal input after turning the camera");
    game.state.move_input = {};
    game.state.local.velocity = {0, 0, 0.4F};
    sprint.update(game, true);
    check(!game.sprint_requests.back(), "Sprint treated knockback or coasting as movement input");
    game.state.move_input = {0, 1};
    game.state.sneaking = true;
    sprint.update(game, true);
    check(!game.sprint_requests.back(), "Sprint ignored sneaking");
    game.state.sneaking = false;
    game.state.move_input.y = std::numeric_limits<float>::quiet_NaN();
    sprint.update(game, true);
    check(!game.sprint_requests.back(), "Sprint accepted non-finite input");
    game.state.move_input = {0, 1};
    game.state.move_input_ready = false;
    sprint.update(game, true);
    check(!game.sprint_requests.back(), "Sprint used unavailable native input");
    game.state.move_input_ready = true;
    sprint.update(game, true);
    game.sprint_result = false;
    sprint.update(game, true);
    check(!sprint.requested_sprinting(), "Rejected input write retained a stale server sprint request");
    game.sprint_result = true;
    sprint.update(game, true);
    sprint.update(game, false);
    check(!game.sprint_requests.back() && !sprint.needs_restore() && !sprint.packet_sprint_override(),
          "Sprint stayed forced while a menu captured input");
    sprint.update(game, true);
    sprint.set_enabled(false);
    game.sprint_result = false;
    sprint.update(game, true);
    check(sprint.needs_restore() && !sprint.packet_sprint_override(), "Failed sprint release was not retained for retry");
    game.sprint_result = true;
    sprint.update(game, true);
    check(!sprint.needs_restore() && !game.sprint_requests.back(), "Disabling Sprint did not clear native input");
    sprint.set_enabled(true);
    sprint.update(game, true);
    game.state.ready = false;
    sprint.update(game, true);
    check(!sprint.needs_restore() && !sprint.packet_sprint_override(), "Sprint retained control across world unload");
    game.state.ready = true;
    game.state.local.id = 9;
    game.sprint_result = false;
    sprint.update(game, true);
    check(!sprint.packet_sprint_override(), "Sprint reused another player's successful input write");
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

void test_scaffold_block_count() {
    MockGame game;
    ScaffoldModule scaffold;
    check(scaffold.block_count() == -1, "Scaffold knew its blocks before a tick");
    scaffold.set_enabled(true);
    const InventoryItemInfo stone{InventoryItemKind::block, 0.0F, 40};
    game.inventory[0] = stone;
    game.inventory[3] = {InventoryItemKind::block, 0.0F, 25};
    game.inventory[5] = classify_inventory_item("apple", 16);
    game.inventory[9] = {InventoryItemKind::block, 0.0F, 64};
    scaffold.game_tick(game, 0.05F);
    check(scaffold.block_count() == 65, "Scaffold counted more than its reachable hotbar blocks");

    // The count is read through the placement delay.
    game.inventory[0].count = 39;
    scaffold.game_tick(game, 0.01F);
    check(scaffold.block_count() == 64, "Scaffold's count waited for the placement delay");

    scaffold.auto_select.get<bool>() = false;
    game.selected_slot = 3;
    scaffold.game_tick(game, 0.01F);
    check(scaffold.block_count() == 25, "Without Auto Select only the held stack can be placed");
    scaffold.auto_select.get<bool>() = true;

    game.inventory_readable = false;
    scaffold.game_tick(game, 0.01F);
    check(scaffold.block_count() == 25, "An unreadable hotbar replaced the known count");
    game.inventory_readable = true;

    game.state.ready = false;
    scaffold.game_tick(game, 0.01F);
    check(scaffold.block_count() == -1, "Scaffold kept a count without a world");
    game.state.ready = true;
    scaffold.game_tick(game, 0.01F);
    check(scaffold.block_count() == 64, "Scaffold did not count again after the world returned");
    scaffold.reset_block_count();
    check(scaffold.block_count() == -1, "The block count did not reset");
    scaffold.game_tick(game, 0.01F);
    scaffold.set_enabled(false);
    check(scaffold.block_count() == -1, "Turning Scaffold off kept its count");
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

void test_inventory_item_classification() {
    using Kind = InventoryItemKind;
    check(classify_inventory_item("diamond_sword", 1) == InventoryItemInfo{Kind::sword, 4.0F, 1}, "Sword not classified");
    check(classify_inventory_item("netherite_pickaxe", 1).kind == Kind::pickaxe, "Pickaxe not classified");
    check(classify_inventory_item("netherite_pickaxe", 1).value == 5.0F, "Tool tier not read");
    check(classify_inventory_item("wooden_axe", 1) == InventoryItemInfo{Kind::axe, 0.5F, 1}, "A pickaxe rule swallowed the axe");
    check(classify_inventory_item("stone_shovel", 1).kind == Kind::shovel, "Shovel not classified");
    check(classify_inventory_item("apple", 3) == InventoryItemInfo{Kind::other, 0.0F, 3}, "Unknown item given a kind");
}

const InventoryItemInfo wooden_sword{InventoryItemKind::sword, 0.5F, 1};
const InventoryItemInfo diamond_sword{InventoryItemKind::sword, 4.0F, 1};
const InventoryItemInfo iron_pickaxe{InventoryItemKind::pickaxe, 3.0F, 1};
const InventoryItemInfo stone_axe{InventoryItemKind::axe, 1.0F, 1};

void test_inv_manager_defaults_and_slots() {
    InvManagerModule manager;
    const auto slot_option = [&manager](std::size_t slot) {
        const auto& choice = manager.slots[slot]->get<Choice>();
        return choice.options[static_cast<std::size_t>(choice.index)];
    };
    check(slot_option(0) == "Sword" && slot_option(1) == "Pickaxe" && slot_option(2) == "Axe" &&
          slot_option(3) == "Block" && slot_option(4) == "None", "Default hotbar layout changed");
    check(!manager.auto_armor.get<bool>(), "Default InvManager options changed");
    manager.set_enabled(true);
    manager.delay.get<float>() = 0.0F;
    manager.mode.get<Choice>().index = 1;
    MockGame game;
    game.inventory[0] = {InventoryItemKind::other, 0.0F, 1};
    game.inventory[9] = iron_pickaxe;
    game.inventory[12] = diamond_sword;
    game.inventory[20] = wooden_sword;
    game.inventory[30] = {InventoryItemKind::block, 0.0F, 32};
    game.inventory[31] = stone_axe;
    for (int tick = 0; tick < 10; ++tick) manager.game_tick(game, 0.05F);
    check(game.drops == std::vector<int>({20}), "The weaker sword was not dropped first");
    check(game.swaps.size() >= 4 && std::vector<std::pair<int, int>>(game.swaps.begin(), game.swaps.begin() + 4) ==
          std::vector<std::pair<int, int>>({{0, 12}, {1, 9}, {2, 31}, {3, 30}}),
          "Slots were not filled in order with the best items");
    check(game.inventory[0] == diamond_sword && game.inventory[1] == iron_pickaxe && game.inventory[2] == stone_axe &&
          game.inventory[3].kind == InventoryItemKind::block, "Swaps did not produce the configured hotbar");
    const auto moves = game.swaps.size();
    manager.game_tick(game, 0.05F);
    check(game.swaps.size() == moves, "A correct inventory was reshuffled");

    game.inventory[10] = {InventoryItemKind::block, 0.0F, 64};
    game.inventory[3].count = 4;
    for (int tick = 0; tick < 4; ++tick) manager.game_tick(game, 0.05F);
    check(game.inventory[3].count == 4, "A block slot was churned for a larger stack");

    manager.slots[2]->get<Choice>().index = 0;
    game.inventory[2] = {};
    game.inventory[35] = stone_axe;
    for (int tick = 0; tick < 4; ++tick) manager.game_tick(game, 0.05F);
    check(game.inventory[2].kind == InventoryItemKind::empty, "A None slot was filled");
}

void test_inv_manager_modes_delay_and_refusal() {
    InvManagerModule manager;
    manager.set_enabled(true);
    manager.delay.get<float>() = 200.0F;
    MockGame game;
    game.inventory[10] = diamond_sword;
    manager.game_tick(game, 0.05F);
    check(game.swaps.empty(), "On Inventory acted with the inventory closed");
    game.inventory_open = true;
    manager.game_tick(game, 0.05F);
    check(game.swaps.size() == 1, "On Inventory did not act with the inventory open");

    game.swaps.clear();
    game.inventory[11] = iron_pickaxe;
    for (int tick = 0; tick < 3; ++tick) manager.game_tick(game, 0.05F);
    check(game.swaps.empty(), "Delay did not hold back the next move");
    manager.game_tick(game, 0.05F);
    check(game.swaps.size() == 1 && game.swaps[0] == std::pair{1, 11}, "Delay held the next move too long");

    game.swaps.clear();
    game.server_answers_swaps = false;
    game.inventory[12] = stone_axe;
    manager.delay.get<float>() = 0.0F;
    for (int tick = 0; tick < 8; ++tick) manager.game_tick(game, 0.05F);
    check(game.swaps.size() == 1, "An unanswered swap was resent before its timeout");
    for (int tick = 0; tick < 12; ++tick) manager.game_tick(game, 0.05F);
    check(game.swaps.size() == 1, "A refused swap was not backed off");
    for (int tick = 0; tick < 110; ++tick) manager.game_tick(game, 0.05F);
    check(game.swaps.size() == 2, "A refused swap was never retried");

    // The local update confirms at once; the server's refusal restores the slot later.
    manager.set_enabled(false);
    manager.set_enabled(true);
    game.inventory = {};
    game.inventory[0] = diamond_sword;
    const auto weaker = classify_inventory_item("iron_sword", 1);
    game.inventory[5] = weaker;
    game.drops.clear();
    manager.game_tick(game, 0.05F);
    check(game.drops == std::vector<int>({5}), "The weaker sword was not dropped");
    manager.game_tick(game, 0.05F);
    game.inventory[5] = weaker;
    for (int tick = 0; tick < 20; ++tick) manager.game_tick(game, 0.05F);
    check(game.drops.size() == 1 && manager.diagnostic().status == std::string_view("delay-or-retry-backoff"),
          "A move the server reverted was resent in a loop");
    for (int tick = 0; tick < 90; ++tick) manager.game_tick(game, 0.05F);
    check(game.drops.size() == 2, "A reverted move was never retried");

    manager.set_enabled(false);
    game.inventory_readable = false;
    manager.set_enabled(true);
    game.swaps.clear();
    manager.game_tick(game, 0.05F);
    check(game.swaps.empty(), "An unreadable inventory produced a swap");
}

void test_inv_manager_sorts_inventory() {
    InvManagerModule manager;
    manager.set_enabled(true);
    manager.delay.get<float>() = 0.0F;
    manager.mode.get<Choice>().index = 1;
    for (auto* slot : manager.slots) slot->get<Choice>().index = 0;
    const InventoryItemInfo apple{InventoryItemKind::other, 0.0F, 3, 260};
    const InventoryItemInfo bread{InventoryItemKind::other, 0.0F, 7, 297};
    const InventoryItemInfo small_blocks{InventoryItemKind::block, 0.0F, 8};
    const InventoryItemInfo large_blocks{InventoryItemKind::block, 0.0F, 32};
    const InventoryItemInfo iron_helmet{InventoryItemKind::helmet, 3.0F, 1};
    MockGame game;
    game.inventory[0] = wooden_sword;
    game.inventory[9] = bread;
    game.inventory[10] = iron_helmet;
    game.inventory[14] = wooden_sword;
    game.inventory[15] = small_blocks;
    game.inventory[18] = large_blocks;
    game.inventory[20] = stone_axe;
    game.inventory[27] = apple;
    game.inventory[33] = diamond_sword;
    for (int tick = 0; tick < 20; ++tick) manager.game_tick(game, 0.05F);
    check(game.drops == std::vector<int>({0, 14}), "Weaker swords were not dropped, hotbar included");
    const std::vector<InventoryItemInfo> sorted{diamond_sword, stone_axe, iron_helmet, large_blocks, small_blocks, apple, bread};
    for (std::size_t place = 0; place < sorted.size(); ++place)
        check(game.inventory[9 + place] == sorted[place], "The inventory was not sorted in order");
    for (std::size_t slot = 9 + sorted.size(); slot < 36; ++slot)
        check(game.inventory[slot].kind == InventoryItemKind::empty, "The sorted inventory left a gap");
    const auto moves = game.swaps.size();
    manager.game_tick(game, 0.05F);
    check(game.swaps.size() == moves, "A sorted inventory kept moving");
}

void test_inv_manager_keeps_assigned_copies() {
    InvManagerModule manager;
    manager.set_enabled(true);
    manager.delay.get<float>() = 0.0F;
    manager.mode.get<Choice>().index = 1;
    manager.slots[1]->get<Choice>().index = 1;
    MockGame game;
    game.inventory[10] = diamond_sword;
    game.inventory[11] = wooden_sword;
    for (int tick = 0; tick < 6; ++tick) manager.game_tick(game, 0.05F);
    check(game.drops.empty(), "A second Sword slot lost its sword");
    check(game.inventory[0] == diamond_sword && game.inventory[1] == wooden_sword, "Both Sword slots were not filled");
}

void test_inv_manager_auto_armor_and_drop() {
    InvManagerModule manager;
    manager.set_enabled(true);
    manager.delay.get<float>() = 0.0F;
    manager.mode.get<Choice>().index = 1;
    const InventoryItemInfo iron_helmet{InventoryItemKind::helmet, 3.0F, 1};
    const InventoryItemInfo diamond_helmet{InventoryItemKind::helmet, 4.0F, 1};
    const InventoryItemInfo diamond_boots{InventoryItemKind::boots, 4.0F, 1};
    const auto holds = [](const MockGame& game, const InventoryItemInfo& item) {
        return std::find(game.inventory.begin(), game.inventory.end(), item) != game.inventory.end();
    };
    MockGame game;
    game.worn[0] = iron_helmet;
    game.inventory[12] = diamond_helmet;
    game.inventory[13] = diamond_boots;
    game.inventory[14] = iron_helmet;
    for (int tick = 0; tick < 6; ++tick) manager.game_tick(game, 0.05F);
    check(game.equips.empty(), "Auto Armor acted while off");
    check(game.drops == std::vector<int>({14}), "Armor no better than the worn piece was kept");
    check(holds(game, diamond_helmet) && holds(game, diamond_boots), "Better armor was dropped");

    manager.auto_armor.get<bool>() = true;
    for (int tick = 0; tick < 10; ++tick) manager.game_tick(game, 0.05F);
    check(game.worn[0] == diamond_helmet && game.worn[3] == diamond_boots, "Auto Armor did not wear the best pieces");
    check(game.drops.size() == 2 && !holds(game, iron_helmet), "The replaced armor was not dropped");
}

void test_inv_manager_waits_for_affected_slots() {
    InvManagerModule manager;
    manager.delay.get<float>() = 0.0F;
    manager.mode.get<Choice>().index = 1;
    manager.set_enabled(true);
    MockGame game;
    game.server_answers_swaps = false;
    game.inventory[10] = diamond_sword;
    manager.game_tick(game, 0.05F);
    check(game.swaps.size() == 1, "Initial swap was not requested");
    // The destination arrives first. Treating any inventory change as success
    // used to drop the original sword while this swap was still in flight.
    game.inventory[0] = diamond_sword;
    game.inventory[20] = iron_pickaxe;
    manager.game_tick(game, 0.05F);
    check(game.drops.empty() && game.swaps.size() == 1, "A half-applied swap triggered another operation");
    game.inventory[10] = {};
    manager.game_tick(game, 0.05F);
    check(game.drops.empty() && game.swaps.size() == 2 && game.swaps.back() == std::pair{1, 20},
          "A fully applied swap did not release the next operation");

    manager.set_enabled(false);
    manager.set_enabled(true);
    game = MockGame{};
    game.server_answers_drops = false;
    game.inventory[0] = diamond_sword;
    game.inventory[10] = wooden_sword;
    manager.game_tick(game, 0.05F);
    for (int tick = 0; tick < 25; ++tick) {
        game.inventory[35] = {InventoryItemKind::other, 0, static_cast<std::uint8_t>(tick + 1)};
        manager.game_tick(game, 0.05F);
    }
    check(game.drops == std::vector<int>{10} && game.swaps.empty(),
          "Unrelated slot changes acknowledged or repeatedly resent a rejected drop");
    game.inventory[10] = {};
    for (int tick = 0; tick < 110; ++tick) manager.game_tick(game, 0.05F);
    check(game.drops.size() == 1, "A later successful drop was repeated");
}

void test_inv_manager_armor_confirmation_and_failures() {
    InvManagerModule manager;
    manager.delay.get<float>() = 0.0F;
    manager.mode.get<Choice>().index = 1;
    manager.auto_armor.get<bool>() = true;
    manager.set_enabled(true);
    MockGame game;
    const InventoryItemInfo helmet{InventoryItemKind::helmet, 4.0F, 1};
    game.inventory[10] = helmet;
    game.server_answers_equips = false;
    manager.game_tick(game, 0.05F);
    game.worn[0] = helmet;
    manager.game_tick(game, 0.05F);
    check(game.equips.size() == 1 && game.drops.empty(), "Partially applied armor equip dropped its source");
    game.inventory[10] = {};
    game.armor_readable = false;
    game.inventory[11] = stone_axe;
    manager.game_tick(game, 0.05F);
    check(game.swaps.empty(), "Unreadable armor acknowledged an equip");
    game.armor_readable = true;
    manager.game_tick(game, 0.05F);
    check(game.swaps.size() == 1, "Completed armor equip did not release the next operation");

    manager.set_enabled(false);
    manager.set_enabled(true);
    game = MockGame{};
    game.swap_result = false;
    game.inventory[10] = diamond_sword;
    for (int tick = 0; tick < 20; ++tick) manager.game_tick(game, 0.05F);
    check(game.swaps.size() == 1, "An unavailable native swap was retried every tick at zero delay");
    game.state.world_identity = 2;
    game.swap_result = true;
    manager.game_tick(game, 0.05F);
    check(game.swaps.size() == 2 && game.inventory[0] == diamond_sword, "Refusal state survived a world change");

    manager.set_enabled(false);
    manager.set_enabled(true);
    game = MockGame{};
    game.drop_result = false;
    game.inventory[0] = diamond_sword;
    game.inventory[10] = wooden_sword;
    for (int tick = 0; tick < 20; ++tick) manager.game_tick(game, 0.05F);
    check(game.drops == std::vector<int>{10} && game.inventory[10] == wooden_sword,
          "A failed drop was repeated or changed local inventory");
}

void test_inv_manager_keeps_equally_strong_assigned_gear() {
    InvManagerModule manager;
    manager.delay.get<float>() = 0.0F;
    manager.mode.get<Choice>().index = 1;
    manager.slots[0]->get<Choice>().index = 0;
    manager.slots[8]->get<Choice>().index = 1;
    manager.set_enabled(true);
    MockGame game;
    game.inventory[0] = diamond_sword;
    game.inventory[8] = diamond_sword;
    for (int tick = 0; tick < 5; ++tick) manager.game_tick(game, 0.05F);
    check(game.drops == std::vector<int>{0} && game.inventory[8] == diamond_sword && game.swaps.empty(),
          "An equally strong sword already in its assigned slot was dropped and replaced");
}

class MockChest final : public ContainerScreen {
public:
    std::array<bool, slot_limit> items{};
    std::vector<int> taken;
    bool is_chest = true;
    int closes = 0;
    bool chest() const override { return is_chest; }
    bool slot_occupied(int slot) const override { return items[static_cast<std::size_t>(slot)]; }
    bool take(int slot) override {
        taken.push_back(slot);
        items[static_cast<std::size_t>(slot)] = false;
        return true;
    }
    bool close() override { ++closes; return true; }
};

void test_chest_stealer_delay_and_close() {
    ChestStealerModule stealer;
    MockChest chest;
    chest.items[3] = chest.items[7] = true;
    stealer.screen_tick(chest, 1000);
    check(chest.taken.empty(), "Disabled ChestStealer took an item");
    stealer.set_enabled(true);
    stealer.delay.get<float>() = 100.0F;
    stealer.screen_tick(chest, 1000);
    stealer.screen_tick(chest, 1050);
    check(chest.taken.empty(), "ChestStealer ignored the delay after opening");
    stealer.screen_tick(chest, 1100);
    check(chest.taken == std::vector<int>({3}), "ChestStealer did not take the first stack");
    stealer.screen_tick(chest, 1150);
    check(chest.taken.size() == 1, "ChestStealer ignored the delay between stacks");
    stealer.screen_tick(chest, 1200);
    check(chest.taken == std::vector<int>({3, 7}) && chest.closes == 0, "ChestStealer did not take the next stack");
    stealer.screen_tick(chest, 1250);
    check(chest.closes == 0, "ChestStealer closed before the chest settled");
    stealer.screen_tick(chest, 1450);
    check(chest.closes == 1, "ChestStealer did not close the emptied chest");

    MockChest empty;
    stealer.screen_tick(empty, 3000);
    stealer.screen_tick(empty, 3500);
    check(empty.closes == 0, "An untouched chest was closed");

    stealer.auto_close.get<bool>() = false;
    MockChest kept;
    kept.items[0] = true;
    stealer.delay.get<float>() = 0.0F;
    stealer.screen_tick(kept, 5000);
    stealer.screen_tick(kept, 5500);
    check(kept.taken.size() == 1 && kept.closes == 0, "Auto Close off still closed the chest");

    MockChest furnace;
    furnace.is_chest = false;
    furnace.items[0] = true;
    stealer.screen_tick(furnace, 9000);
    check(furnace.taken.empty(), "ChestStealer acted on another container type");
}

} // namespace

constexpr BlockPos regen_ore{2, 65, 0};
constexpr BlockPos regen_other_ore{-3, 65, 0};
constexpr std::uint16_t redstone = 73;
constexpr int regen_pickaxe_slot = 2;

void prepare_regen(MockGame& game, RegenModule& regen) {
    game.blocks[cell(regen_ore)] = redstone;
    game.inventory[regen_pickaxe_slot].kind = InventoryItemKind::pickaxe;
    game.inventory[regen_pickaxe_slot].value = 4.0F;
    regen.set_enabled(true);
}

void tick_regen(MockGame& game, RegenModule& regen, int ticks = 1) {
    for (int tick = 0; tick < ticks; ++tick) regen.game_tick(game, 0.05F);
}

void test_regen_reactive_waits_for_block_update() {
    MockGame game;
    RegenModule regen;
    regen.swing.get<Choice>().index = 2;
    prepare_regen(game, regen);
    game.blocks[cell(regen_other_ore)] = redstone;
    tick_regen(game, regen, 3);
    check(game.selected_slot == regen_pickaxe_slot, "Regen did not select the pickaxe");
    check(game.destroy_requests.size() == 2, "Regen did not mine after the slot change");

    game.next_progress = 1.0F;
    game.next_destroyed = true;
    tick_regen(game, regen);
    const auto requests = game.destroy_requests.size();
    check(game.mining_swings == 0, "Reactive swung before the block update");
    tick_regen(game, regen, 10);
    check(game.destroy_requests.size() == requests, "Reactive moved on before the block update");
    check(game.selected_slot == regen_pickaxe_slot, "Reactive restored the slot before the block update");

    game.blocks.clear();
    tick_regen(game, regen);
    check(game.mining_swings == 1, "Reactive did not swing on the block update");
    tick_regen(game, regen);
    check(game.selected_slot == 0, "Reactive did not restore the slot after the block update");
}

void test_regen_reactive_retries_a_refused_break() {
    MockGame game;
    RegenModule regen;
    prepare_regen(game, regen);
    tick_regen(game, regen, 3);
    game.next_progress = 1.0F;
    game.next_destroyed = true;
    tick_regen(game, regen);
    const auto requests = game.destroy_requests.size();
    game.next_progress = 0.0F;
    game.next_destroyed = false;
    tick_regen(game, regen, 21);
    check(game.destroy_requests.size() > requests, "Reactive never retried a block the server kept");
}

void test_regen_fastchange_retries_a_refused_break() {
    MockGame game;
    RegenModule regen;
    regen.switch_mode.get<Choice>().index = 1;
    prepare_regen(game, regen);
    game.selected_slot = regen_pickaxe_slot;
    tick_regen(game, regen);
    game.next_progress = 1.0F;
    tick_regen(game, regen);
    check(game.destroy_stops >= 1, "Fastchange did not finish at full progress");
    game.next_progress = 0.0F;
    const auto requests = game.destroy_requests.size();
    tick_regen(game, regen, 5);
    check(game.destroy_requests.size() == requests, "Fastchange reselected the block it just finished");
    tick_regen(game, regen, 25);
    check(game.destroy_requests.size() > requests, "Fastchange excluded a block the server kept forever");
}

void test_regen_restores_slot_after_a_quick_toggle() {
    MockGame game;
    RegenModule regen;
    prepare_regen(game, regen);
    tick_regen(game, regen, 2);
    check(game.selected_slot == regen_pickaxe_slot, "Regen did not select the pickaxe");
    regen.set_enabled(false);
    regen.set_enabled(true);
    game.blocks.clear();
    tick_regen(game, regen);
    check(game.selected_slot == 0, "A quick toggle lost the slot to restore");
}

void test_regen_restores_the_latest_slot() {
    MockGame game;
    RegenModule regen;
    prepare_regen(game, regen);
    game.selected_slot = regen_pickaxe_slot;
    tick_regen(game, regen);
    game.blocks.clear();
    tick_regen(game, regen, 2);

    game.selected_slot = 5;
    game.blocks[cell(regen_ore)] = redstone;
    tick_regen(game, regen, 8);
    check(game.selected_slot == regen_pickaxe_slot, "Regen did not select the pickaxe again");
    game.blocks.clear();
    tick_regen(game, regen, 2);
    check(game.selected_slot == 5, "Regen restored the slot from an earlier target");
}

void test_regen_slot_waits_survive_scrolling() {
    MockGame game;
    RegenModule regen;
    prepare_regen(game, regen);
    tick_regen(game, regen);
    check(game.selected_slot == regen_pickaxe_slot, "Regen did not select the pickaxe");
    game.selected_slot = 7;
    tick_regen(game, regen, 2);
    check(game.selected_slot == regen_pickaxe_slot, "Regen stayed stuck after the player scrolled");
    check(!game.destroy_requests.empty(), "Regen did not mine after recovering the slot");

    game.inventory[4].kind = InventoryItemKind::block;
    regen.infinity_durability.value = true;
    game.switch_before_destroy = true;
    tick_regen(game, regen);
    check(game.selected_slot == 4 && game.safe_finishes == 1, "Regen did not select the final-hit item");
    game.selected_slot = 7;
    const auto requests = game.destroy_requests.size();
    tick_regen(game, regen, 2);
    check(game.selected_slot == 4, "Regen lost the final-hit item after the player scrolled");
    check(game.destroy_requests.size() > requests, "Regen did not finish after recovering the final-hit item");
}

void test_regen_failed_starts_are_throttled() {
    MockGame game;
    RegenModule regen;
    prepare_regen(game, regen);
    game.selected_slot = regen_pickaxe_slot;
    game.destroy_result = false;
    tick_regen(game, regen);
    tick_regen(game, regen, 10);
    check(game.destroy_requests.size() == 1, "Regen retried a refused block without a cooldown");
    check(regen.diagnostic_status() == RegenModule::DiagnosticStatus::mine_failed, "Regen hid the refused break");
    tick_regen(game, regen, 15);
    check(game.destroy_requests.size() == 2, "Regen did not retry after the cooldown");
}

int main() {
    try {
        test_disabled_and_unavailable();
        test_regen_reactive_waits_for_block_update();
        test_regen_reactive_retries_a_refused_break();
        test_regen_fastchange_retries_a_refused_break();
        test_regen_restores_slot_after_a_quick_toggle();
        test_regen_restores_the_latest_slot();
        test_regen_slot_waits_survive_scrolling();
        test_regen_failed_starts_are_throttled();
        test_antibot_checks();
        test_bots_are_not_targeted();
        test_velocity_motion();
        test_velocity_delay_timing();
        test_velocity_unported_prediction();
        test_velocity_delay2_hit_count();
        test_velocity_delay_reset_and_failures();
        test_velocity_delay_capacity();
        test_velocity_zero_motion_is_immediate();
        test_velocity_latency_ordering();
        test_velocity_latency_cleanup_and_retry();
        test_velocity_position_reset_retires_old_hits();
        test_velocity_simultaneous_release();
        test_velocity_prediction_timestamps();
        test_derp_spin_and_reset();
        test_aimbot_selection_and_cone();
        test_aimbot_yaw_wrap_and_bounds_range();
        test_killaura_cadence_and_filters();
        test_killaura_failures_are_throttled();
        test_killaura_modes();
        test_rotations_vertical_render();
        test_sprint_input_and_release();
        test_scaffold_floor_extension_and_delay();
        test_scaffold_block_count();
        test_scaffold_stationary_failure_and_invalid_data();
        test_scaffold_level_and_support();
        test_inventory_item_classification();
        test_inv_manager_defaults_and_slots();
        test_inv_manager_modes_delay_and_refusal();
        test_inv_manager_sorts_inventory();
        test_inv_manager_keeps_assigned_copies();
        test_inv_manager_auto_armor_and_drop();
        test_inv_manager_waits_for_affected_slots();
        test_inv_manager_armor_confirmation_and_failures();
        test_inv_manager_keeps_equally_strong_assigned_gear();
        test_chest_stealer_delay_and_close();
        std::cout << "Gameplay module tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Gameplay module test failed: " << error.what() << '\n';
        return 1;
    }
}
