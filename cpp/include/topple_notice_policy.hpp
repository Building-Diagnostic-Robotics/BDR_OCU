/**
 * @file topple_notice_policy.hpp
 * @brief Pure Stage 6 topple-trip edge detector — no Qt, no ROS.
 *
 * The robot's topple guard (pilot_control tip_guard_node) performs the robot
 * half of an E-Stop by itself on a trip: both drive axes to IDLE and
 * /mpc_autonomy_enable=false. The director then counts the trip in
 * /coverage/status as `tip_seq`. This screen's part is the other half of the
 * same E-Stop — the latch — plus telling the operator why it engaged.
 *
 * `tip_seq` is a counter, not a level, on purpose. A level can rise and fall
 * between two 1 Hz status lines (the guard re-arms itself two seconds after
 * the robot settles) and the trip would never be seen here; a counter that
 * moved is unambiguous however late the line arrives.
 *
 * Two more facts ride the same status line. `tip` is the guard's latch: while
 * it holds, the robot refuses an autonomy enable, so Start and Resume must
 * not offer one. `tip_ready` is liveness only — a guard that has not reported
 * is warned about, never gated on.
 */

#pragma once

namespace f2c_cpp {
namespace topple_notice_policy {

/** No status seen yet this mission. */
constexpr int kUnseen = -1;

struct Decision {
    int seen;      ///< value to remember
    bool engage;   ///< latch the E-Stop and notify
};

/** Engage exactly once per new trip.
 *
 *  - A higher count is a new trip, including on the very first status of a
 *    mission: a trip this screen never saw must still latch, and stopping is
 *    the safe reading of it.
 *  - A lower count means the director restarted and is counting from zero
 *    again. Follow it down without engaging, or the next real trip (1 -> 1)
 *    would be invisible.
 *  - Zero is "no trips", never a trip. A missing key decodes as zero, so a
 *    robot without the guard can never engage this. */
constexpr Decision onTripSeq(int seen, int incoming) noexcept {
    if (incoming < 0) {
        return {seen, false};
    }
    if (incoming > seen) {
        return {incoming, incoming > 0};
    }
    if (incoming < seen) {
        return {incoming, false};
    }
    return {seen, false};
}

/** A latched guard blocks Start and Resume: the robot would refuse the
 *  enable. Only on a fresh status — a stale `tip` must not strand the
 *  operator after the radio drops. */
constexpr bool blocksArming(bool status_fresh, bool tip_latched) noexcept {
    return status_fresh && tip_latched;
}

/** The robot refused an enable we sent. We still think the run is going,
 *  the guard is latched, and this sample was produced after the enable
 *  went out. A sample from before the enable is the old state, not a
 *  refusal, and a missing stamp (0) has nothing to compare against. */
constexpr bool refusedWhileRunning(bool running, bool tip_latched,
                                   long long sample_wall_ms,
                                   long long enable_sent_ms) noexcept {
    return running && tip_latched && enable_sent_ms > 0 &&
           sample_wall_ms > enable_sent_ms;
}

/** Guard-offline watch. Warn once after `threshold_ms` of continuous
 *  not-ready, and report recovery once. Readiness is not a gate. */
struct GuardWatch {
    long long not_ready_since_ms = -1;
    bool warned = false;
};

struct GuardStep {
    GuardWatch next{};
    bool warn_now = false;
    bool recovered_now = false;
};

/** Warmup is a few tenths of a second. Ten seconds is a guard that is
 *  down, not one that is starting. */
constexpr long long kGuardOfflineWarnMs = 10000;

constexpr GuardStep onTipReady(GuardWatch watch, bool ready, long long now_ms,
                               long long threshold_ms = kGuardOfflineWarnMs) noexcept {
    GuardStep out;
    if (ready) {
        if (watch.warned) {
            out.recovered_now = true;
        }
        return out;
    }
    if (watch.not_ready_since_ms < 0) {
        watch.not_ready_since_ms = now_ms;
    }
    if (!watch.warned &&
        now_ms - watch.not_ready_since_ms >= threshold_ms) {
        watch.warned = true;
        out.warn_now = true;
    }
    out.next = watch;
    return out;
}

}  // namespace topple_notice_policy
}  // namespace f2c_cpp
