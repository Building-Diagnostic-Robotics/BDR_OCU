/**
 * @file mission_done_notice_policy.hpp
 * @brief Pure Stage 6 "scan complete" notice rule — no Qt, no ROS.
 *
 * When autonomous coverage finishes, the director ends and saves the section
 * by itself and reports state COMPLETE. Nothing else changes on screen, so
 * the operator is told once, with a way to Complete Mission.
 *
 * COMPLETE is also what an operator-initiated Finish produces; that case must
 * stay silent. A failed save reports ERROR instead (it outranks COMPLETE in
 * the director), so this can never announce a run whose data is not saved.
 */

#pragma once

namespace f2c_cpp {
namespace mission_done_notice_policy {

struct Inputs {
    bool robot_complete = false;      // /coverage/status state == "COMPLETE"
    bool status_fresh = false;        // a stale line never announces
    bool mission_active = false;
    bool tearing_down = false;
    bool autonomy_ran = false;        // autonomy ran in this mission
    bool operator_finishing = false;  // Complete Mission already in flight
    bool already_shown = false;       // once per mission
};

constexpr bool shouldShow(const Inputs& in) noexcept {
    return in.robot_complete && in.status_fresh && in.mission_active &&
           !in.tearing_down && in.autonomy_ran && !in.operator_finishing &&
           !in.already_shown;
}

}  // namespace mission_done_notice_policy
}  // namespace f2c_cpp
