/**
 * @file motors_state_policy.hpp
 * @brief Pure Stage 6 motor-state reading and the Arm-button hint — no Qt,
 *        no ROS.
 *
 * The state comes from the ODrive controller_status topics, not from the
 * axis-state RPC: a request being accepted is not the axes having moved.
 * Both the top MOTORS chip and the Arm hint read it through `classify`, so
 * they cannot disagree.
 */

#pragma once

namespace f2c_cpp {
namespace motors_state_policy {

/** ODrive AxisState values. Kept in step with RosLink::kAxisIdle /
    kAxisClosedLoop by a static_assert in satellite_screen.cpp. */
constexpr int kAxisIdle = 1;
constexpr int kAxisClosedLoop = 8;

enum class MotorsState {
    Unknown,   ///< no fresh controller_status from both axes
    Armed,     ///< both axes CLOSED_LOOP
    Disarmed,  ///< both axes IDLE
    Split,     ///< anything else (one armed, one not, or mid-transition)
};

/** `fresh` means both axes reported within the stale window. */
constexpr MotorsState classify(int left_axis_state, int right_axis_state,
                               bool fresh) noexcept {
    if (!fresh) {
        return MotorsState::Unknown;
    }
    if (left_axis_state == kAxisClosedLoop &&
        right_axis_state == kAxisClosedLoop) {
        return MotorsState::Armed;
    }
    if (left_axis_state == kAxisIdle && right_axis_state == kAxisIdle) {
        return MotorsState::Disarmed;
    }
    return MotorsState::Split;
}

/** Blink the Arm button while the operator is driving by hand on IDLE axes:
 *  the wheels will not turn, typically right after an E-Stop is cleared.
 *
 *  - Only a known Disarmed state blinks. Unknown (stale link) never does, so
 *    the hint is never a guess.
 *  - Stops by itself once the axes report CLOSED_LOOP, manual drive ends, or
 *    the buttons are not commandable (no mission / teardown). */
constexpr bool shouldBlinkArm(bool manual_override, bool commandable,
                              MotorsState state) noexcept {
    return manual_override && commandable && state == MotorsState::Disarmed;
}

}  // namespace motors_state_policy
}  // namespace f2c_cpp
