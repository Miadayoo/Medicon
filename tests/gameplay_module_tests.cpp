#include "medicon/modules/GameplayModules.hpp"
#include "medicon/modules/RotationsModule.hpp"
#include "medicon/sdk/Gameplay.hpp"

#include <cmath>
#include <cstring>
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
    std::vector<float> head_body_rotations;
    std::vector<bool> sprint_requests;
    std::vector<Vec3> velocity_requests;
    bool velocity_result = true;
    bool throw_velocity = false;
    bool native_prediction = false, prediction_available = true;
    std::vector<std::pair<Vec3, std::int64_t>> prediction_requests;
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

    const GameplayFrame& frame() const noexcept override { return state; }
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
    KnockbackApplyResult apply_knockback(Vec3 velocity, std::int64_t received_timestamp) override {
        if (!native_prediction) return GameplayAccess::apply_knockback(velocity, received_timestamp);
        prediction_requests.emplace_back(velocity, received_timestamp);
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
    check(velocity.handle_motion(a, game.state, start, 1000000000000LL) == VelocityModule::MotionResult::delayed &&
          velocity.handle_latency(first, game.state) &&
          velocity.handle_motion(b, game.state, start + 2s, 1002000000000LL) == VelocityModule::MotionResult::delayed &&
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
        test_velocity_motion();
        test_velocity_delay_timing();
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
        test_rotations_vertical_render();
        test_sprint_input_and_release();
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
