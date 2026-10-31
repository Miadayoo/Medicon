#pragma once

#include "medicon/modules/GameplayModule.hpp"
#include "medicon/sdk/Gameplay.hpp"

#include <chrono>
#include <deque>

namespace medicon {

class VelocityModule final : public GameplayModule {
public:
    static constexpr const char* module_id = "velocity";
    enum class MotionResult { ignored, cancelled, scaled, delayed };
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    static constexpr std::size_t maximum_pending = 256;

    VelocityModule();
    // Called only for a validated incoming motion packet targeting the local player.
    [[nodiscard]] MotionResult process_motion(sdk::Vec3& motion) const noexcept;
    // The host serializes receive, game-tick and UI callbacks. Queue copied
    // values only; native packet references are borrowed until receive returns.
    [[nodiscard]] MotionResult handle_motion(sdk::Vec3& motion, const sdk::GameplayFrame& frame,
                                             TimePoint now = Clock::now(), std::int64_t received_timestamp = 0) noexcept;
    void game_tick(sdk::GameplayAccess& game, float delta_seconds) noexcept override;
    void update(sdk::GameplayAccess& game, bool allow_actions, TimePoint now = Clock::now()) noexcept;
    void reset_delay() noexcept;
    [[nodiscard]] bool handle_latency(const sdk::LatencyReply& reply, const sdk::GameplayFrame& frame) noexcept;
    // A validated local respawn/teleport supersedes all older queued impulses.
    // Keep confirmations for ordered cleanup on the next native game tick.
    [[nodiscard]] std::size_t handle_position_reset(std::uint8_t mode) noexcept;
    [[nodiscard]] bool needs_restore() const noexcept { return !replies_.empty(); }
    [[nodiscard]] std::size_t pending_reply_count() const noexcept { return replies_.size(); }
    struct Diagnostics {
        std::uint64_t queued{}, applied{}, latency_seen{}, latency_held{}, latency_sent{}, write_failures{}, send_failures{};
        std::uint64_t predicted{}, direct{};
        std::uint64_t position_resets{}, invalidated_motion{};
        // A zero-motion SetActorMotion is the server's velocity stop, not a
        // knockback. It must never be held behind Delay.
        std::uint64_t zero_motion_stops{};
    };
    [[nodiscard]] const Diagnostics& diagnostics() const noexcept { return diagnostics_; }
    [[nodiscard]] std::size_t pending_count() const noexcept { return pending_.size(); }
    [[nodiscard]] bool setting_visible(const Setting& setting) const noexcept override;

    Setting& mode;
    Setting& horizontal;
    Setting& vertical;
    Setting& delay_ms;
    Setting& sync_replies;

protected:
    void on_disable() noexcept override { cancel_motion(); }
    void on_tick(float) noexcept override;

private:
    [[nodiscard]] bool delay_enabled() const noexcept;
    [[nodiscard]] bool sync_context(const sdk::GameplayFrame& frame) noexcept;
    void cancel_motion() noexcept;
    void flush_replies(sdk::GameplayAccess& game, bool force) noexcept;
    struct PendingMotion {
        sdk::Vec3 motion;
        TimePoint due;
        std::uint64_t sequence;
        TimePoint received;
        std::int64_t received_timestamp;
    };
    struct PendingReply { sdk::LatencyReply reply; std::uint64_t after_motion; };
    std::deque<PendingMotion> pending_;
    std::deque<PendingReply> replies_;
    Diagnostics diagnostics_;
    std::uint64_t sequence_{}, applied_sequence_{}, cancelled_sequence_{};
    std::uint64_t world_identity_{};
    std::uintptr_t local_id_{};
    std::int64_t unique_id_{};
};

} // namespace medicon
