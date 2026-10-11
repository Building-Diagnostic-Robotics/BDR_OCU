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

}  // namespace topple_notice_policy
}  // namespace f2c_cpp
