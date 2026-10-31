#include "medicon/modules/VelocityModule.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace medicon {

VelocityModule::VelocityModule()
    : GameplayModule(module_id, "Velocity", "Cancel, reduce or delay incoming knockback.", Category::Movement),
      mode(add_setting({"mode", "Mode", "Full cancels knockback; Percent scales it; Delay applies it later.",
                        Choice{0, {"Full", "Percent", "Delay"}}})),
      horizontal(add_setting({"horizontal", "Horizontal", "Horizontal knockback received: 0% cancels, 100% is normal.",
                              0, 0.0F, 100.0F, 1.0F})),
      vertical(add_setting({"vertical", "Vertical", "Vertical knockback received: 0% cancels, 100% is normal.",
                            0, 0.0F, 100.0F, 1.0F})),
      delay_ms(add_setting({"delay_ms", "Delay (ms)", "Time before applying received knockback. Server acceptance is unverified.",
                            8000, 0.0F, 10000.0F, 50.0F})),
      sync_replies(add_setting({"sync_replies", "Sync Replies", "Hold latency replies behind delayed knockback. Server acceptance is unverified.", true})) {}

bool VelocityModule::setting_visible(const Setting& setting) const noexcept {
    if (!setting.visible) return false;
    const auto* selection = std::get_if<Choice>(&mode.value);
    if (&setting == &delay_ms || &setting == &sync_replies) return selection && selection->index == 2;
    return (&setting != &horizontal && &setting != &vertical) || (selection && selection->index == 1);
}

bool VelocityModule::delay_enabled() const noexcept {
    const auto* selection = std::get_if<Choice>(&mode.value);
    return enabled() && selection && selection->index == 2;
}

void VelocityModule::reset_delay() noexcept {
    pending_.clear();
    replies_.clear();
    sequence_ = applied_sequence_ = cancelled_sequence_ = 0;
    world_identity_ = 0;
    local_id_ = 0;
    unique_id_ = 0;
}

void VelocityModule::on_tick(float) noexcept {
    // UI callbacks cannot send native packets. Keep held replies for cleanup
    // on the next game tick, even after leaving Delay or disabling the module.
    if (!delay_enabled()) cancel_motion();
}

void VelocityModule::cancel_motion() noexcept {
    pending_.clear();
    cancelled_sequence_ = sequence_;
}

std::size_t VelocityModule::handle_position_reset(std::uint8_t mode) noexcept {
    if (mode != 1 && mode != 2) return 0; // Normal and OnlyHeadRot do not retire KB.
    const auto discarded = pending_.size();
    ++diagnostics_.position_resets;
    diagnostics_.invalidated_motion += discarded;
    cancel_motion();
    return discarded;
}

bool VelocityModule::sync_context(const sdk::GameplayFrame& frame) noexcept {
    if (!frame.ready || !frame.local.alive || !frame.local.id || !frame.world_identity) {
        reset_delay();
        return false;
    }
    if (world_identity_ != frame.world_identity || local_id_ != frame.local.id || unique_id_ != frame.local.unique_id) {
        reset_delay();
        world_identity_ = frame.world_identity;
        local_id_ = frame.local.id;
        unique_id_ = frame.local.unique_id;
    }
    return true;
}

VelocityModule::MotionResult VelocityModule::handle_motion(
    sdk::Vec3& motion, const sdk::GameplayFrame& frame, TimePoint now, std::int64_t received_timestamp) noexcept {
    if (!delay_enabled()) {
        cancel_motion();
        return process_motion(motion);
    }
    if (!std::isfinite(motion.x) || !std::isfinite(motion.y) || !std::isfinite(motion.z))
        return MotionResult::ignored;
    // The server also sends a zero SetActorMotion to stop the local player
    // (setback / velocity cleanup), not only as a knockback. Holding that stop
    // for seconds leaves the client moving after the server has zeroed it, so
    // apply it immediately and retire every older delayed impulse.
    if (std::abs(motion.x) <= 1.0e-6F && std::abs(motion.y) <= 1.0e-6F && std::abs(motion.z) <= 1.0e-6F) {
        ++diagnostics_.zero_motion_stops;
        if (!pending_.empty()) {
            diagnostics_.invalidated_motion += pending_.size();
            cancel_motion();
        }
        return MotionResult::ignored;
    }
    if (!sync_context(frame))
        return MotionResult::ignored;
    const auto* milliseconds = std::get_if<int>(&delay_ms.value);
    if (!milliseconds) return MotionResult::ignored;
    const auto delay = std::chrono::milliseconds(std::clamp(*milliseconds, 0, 10000));
    // Zero delay and overflow pass the incoming packet through normally.
    if (delay.count() == 0 || pending_.size() >= maximum_pending) return MotionResult::ignored;
    auto due = now + delay;
    // Each hit keeps its own deadline; later hits cannot reset an earlier hit's
    // timer or overtake it when the delay setting is shortened.
    if (!pending_.empty()) due = std::max(due, pending_.back().due);
    try { pending_.push_back({motion, due, sequence_ + 1, now, received_timestamp}); }
    catch (...) { return MotionResult::ignored; }
    ++sequence_;
    ++diagnostics_.queued;
    return MotionResult::delayed;
}

bool VelocityModule::handle_latency(const sdk::LatencyReply& reply, const sdk::GameplayFrame& frame) noexcept {
    ++diagnostics_.latency_seen;
    const auto* sync = std::get_if<bool>(&sync_replies.value);
    if (!sync_context(frame) || replies_.size() >= maximum_pending * 2) return false;
    // Even after OFF, hold newer replies behind the cleanup queue until the
    // next game tick flushes it. Passing one through would confirm older KB.
    if (replies_.empty() && (!delay_enabled() || !sync || !*sync || pending_.empty())) return false;
    // Later acknowledgments can confirm every earlier packet. They must not
    // overtake the held replies, including a failed send after the last KB write.
    try { replies_.push_back({reply, sequence_}); }
    catch (...) { return false; }
    ++diagnostics_.latency_held;
    return true;
}

void VelocityModule::flush_replies(sdk::GameplayAccess& game, bool force) noexcept {
    while (!replies_.empty()) {
        const auto required = replies_.front().after_motion;
        if (!force && required > cancelled_sequence_ && required > applied_sequence_) return;
        try {
            if (!game.send_latency_reply(replies_.front().reply)) { ++diagnostics_.send_failures; return; }
        } catch (...) { ++diagnostics_.send_failures; return; }
        replies_.pop_front();
        ++diagnostics_.latency_sent;
    }
}

void VelocityModule::game_tick(sdk::GameplayAccess& game, float delta_seconds) noexcept {
    (void)delta_seconds;
    update(game, true);
}

void VelocityModule::update(sdk::GameplayAccess& game, bool allow_actions, TimePoint now) noexcept {
    if (!sync_context(game.frame())) return;
    const auto* sync = std::get_if<bool>(&sync_replies.value);
    if (!delay_enabled() || !allow_actions) {
        cancel_motion();
        flush_replies(game, true);
        return;
    }
    // At most one application per game tick. A stall must not drain every
    // queued hit into the same simulation step. Failed writes retry next tick.
    if (!pending_.empty() && now >= pending_.front().due) {
        try {
            const auto& next = pending_.front();
            auto timestamp = next.received_timestamp;
            if (timestamp > 0) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - next.received).count();
                if (elapsed < 0 || elapsed > (std::numeric_limits<std::int64_t>::max)() - timestamp) {
                    ++diagnostics_.write_failures;
                    return;
                }
                // Retain the native clock epoch and original receive-to-hook
                // interval. Rebase by actual elapsed time, including retries.
                timestamp += elapsed;
            }
            const auto result = game.apply_knockback(next.motion, timestamp);
            if (result != sdk::KnockbackApplyResult::failed) {
                applied_sequence_ = pending_.front().sequence;
                pending_.pop_front();
                ++diagnostics_.applied;
                if (result == sdk::KnockbackApplyResult::predicted) ++diagnostics_.predicted;
                else ++diagnostics_.direct;
            } else { ++diagnostics_.write_failures; }
        } catch (...) {
            ++diagnostics_.write_failures;
            cancel_motion();
            flush_replies(game, true);
            return;
        }
    }
    // Return eligible confirmations in this same update. Waiting another tick
    // can let movement affected by the KB reach the server before its confirmation.
    // A failed velocity write still leaves the matching confirmation blocked.
    flush_replies(game, !sync || !*sync);
}

VelocityModule::MotionResult VelocityModule::process_motion(sdk::Vec3& motion) const noexcept {
    if (!enabled() || !std::isfinite(motion.x) || !std::isfinite(motion.y) || !std::isfinite(motion.z))
        return MotionResult::ignored;
    const auto* selection = std::get_if<Choice>(&mode.value);
    if (!selection) return MotionResult::ignored;
    if (selection->index == 0) return MotionResult::cancelled;
    if (selection->index != 1) return MotionResult::ignored;
    const auto* h = std::get_if<int>(&horizontal.value);
    const auto* v = std::get_if<int>(&vertical.value);
    if (!h || !v) return MotionResult::ignored;
    const float horizontal_scale = static_cast<float>(std::clamp(*h, 0, 100)) / 100.0F;
    const float vertical_scale = static_cast<float>(std::clamp(*v, 0, 100)) / 100.0F;
    if (horizontal_scale == 1.0F && vertical_scale == 1.0F) return MotionResult::ignored;
    motion = {motion.x * horizontal_scale, motion.y * vertical_scale, motion.z * horizontal_scale};
    return MotionResult::scaled;
}

} // namespace medicon
