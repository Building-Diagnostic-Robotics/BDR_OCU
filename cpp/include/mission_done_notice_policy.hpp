/**
 * @file mission_done_notice_policy.hpp
 * @brief Pure Stage 6 "scan complete" notice rule — no Qt, no ROS.
 *
 * When autonomous coverage finishes, the director ends and saves the section
 * by itself and reports state COMPLETE. Nothing else changes on screen, so
 * the operator is told once, and offered Complete Mission or driving the
 * robot back first.
 *
 * It must never misfire, so it is edge-triggered and conservative:
 *  - Only a working -> COMPLETE transition seen in this mission counts. A
 *    COMPLETE that was already there (leftover status, a director that
 *    restarted finished) never announces.
 *  - Anything the operator started (Complete Mission, Finish scan, Cancel)
 *    silences it for the rest of the mission, even if that attempt was
 *    cancelled: COMPLETE after it is not "finished on its own".
 *  - Stale status changes nothing. A failed save reports ERROR (it outranks
 *    COMPLETE in the director), and any fresh non-COMPLETE state closes an
 *    open notice.
 *  - Deferred while the operator is driving by hand; never during teardown.
 */

#pragma once

namespace f2c_cpp {
namespace mission_done_notice_policy {

struct Tracker {
    // A fresh, non-COMPLETE status was seen after autonomy ran this mission.
    bool seen_working = false;
    bool shown = false;
};

struct Inputs {
    bool status_fresh = false;
    bool robot_complete = false;      // fresh state == "COMPLETE"
    bool mission_live = false;        // mission active and not tearing down
    bool autonomy_ran = false;        // autonomy was enabled this mission
    bool operator_ended = false;      // operator asked to end, this mission
    bool manual_override = false;     // operator is driving by hand
};

struct Output {
    Tracker next;
    bool show = false;   // raise the notice now
    bool close = false;  // close an open notice (state left COMPLETE)
};

constexpr Output step(const Tracker& t, const Inputs& in) noexcept {
    Output out{t, false, false};
    if (!in.status_fresh || !in.mission_live) {
        return out;
    }
    if (!in.robot_complete) {
        out.next.seen_working = t.seen_working || in.autonomy_ran;
        out.close = true;
        return out;
    }
    out.show = t.seen_working && !t.shown && !in.operator_ended &&
               !in.manual_override;
    if (out.show) {
        out.next.shown = true;
    }
    return out;
}

}  // namespace mission_done_notice_policy
}  // namespace f2c_cpp
