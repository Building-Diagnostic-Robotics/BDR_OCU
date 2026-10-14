/**
 * @file attention_policy.hpp
 * @brief Pure Stage 6 "robot needs the operator" rule — no Qt, no ROS.
 *
 * The director's state is "what is happening now", not "needs help": a
 * route handover or an in-tick replan reads STOPPED / REPLAN_FAILED for
 * seconds while the robot is fine. So the prompt keys off lack of
 * progress, plus the robot's own takeover / pose-hold / fault signals.
 * A robot that publishes `attention` is followed instead.
 */

#pragma once

#include <cmath>

namespace f2c_cpp {
namespace attention_policy {

enum class Kind { None, Takeover, PoseHold, Stalled, Fault };

constexpr long long kStallMs = 15000;
constexpr long long kRemindMs = 60000;
constexpr long long kHoldConfirmMs = 2000;
constexpr long long kOdomFreshMs = 2000;
constexpr double kProgressTranslationM = 0.25;
constexpr double kProgressYawRad = 15.0 * 3.14159265358979323846 / 180.0;

struct Pose {
    double x = 0.0;
    double y = 0.0;
    double yaw = 0.0;
    bool valid = false;
};

struct Inputs {
    long long now_ms = 0;
    // Mission active, not tearing down, run Running, status fresh.
    bool gates_open = false;
    // Robot is stationary on purpose (DC start/pause, rollover, revisit
    // prompt, complete, topple latch). Restarts the stall clock.
    bool robot_hold = false;
    bool odom_fresh = false;
    Pose pose;
    double swept = -1.0;   // ledger total, < 0 unknown
    bool takeover = false;
    bool pose_hold = false;
    bool fault = false;
    // Robot publishes `attention`: its kind and episode seq win.
    bool robot_speaks = false;
    Kind robot_kind = Kind::None;
    int robot_seq = -1;
};

struct Tracker {
    // Last place progress was seen. anchor_ms is the stall clock origin.
    Pose anchor;
    double anchor_swept = -1.0;
    long long anchor_ms = 0;
    long long takeover_since = -1;
    long long pose_hold_since = -1;
    long long fault_since = -1;
    // Current episode. None = no episode.
    Kind kind = Kind::None;
    int robot_seq = -1;
    bool shown = false;
    bool dismissed = false;
    long long dismissed_ms = 0;
    // Robot episode the operator dismissed. Stays quiet across a radio gap
    // while the robot still reports that seq; cleared when the robot says
    // nothing is wrong.
    int quiet_seq = -1;
};

struct Output {
    Tracker next;
    bool show = false;      // raise the prompt (first time or reminder)
    bool ended = false;     // episode closed: auto-close any open prompt
    bool progressed = false;
    Kind kind = Kind::None;
    long long stalled_ms = 0;
};

inline double angleDiff(double a, double b) {
    return std::fabs(std::remainder(a - b, 2.0 * 3.14159265358979323846));
}

/** Moved, turned, or swept more. Replans do not count: replanning without
    moving is the stuck case. */
inline bool madeProgress(const Tracker& t, const Pose& p, double swept) {
    if (swept >= 0.0 && t.anchor_swept >= 0.0 && swept > t.anchor_swept) {
        return true;
    }
    if (!p.valid || !t.anchor.valid) {
        return false;
    }
    return std::hypot(p.x - t.anchor.x, p.y - t.anchor.y) >=
               kProgressTranslationM ||
           angleDiff(p.yaw, t.anchor.yaw) >= kProgressYawRad;
}

inline long long heldSince(long long since, bool on, long long now) {
    if (!on) {
        return -1;
    }
    return since < 0 ? now : since;
}

inline bool held(long long since, long long now) {
    return since >= 0 && now - since >= kHoldConfirmMs;
}

inline Output step(Tracker t, const Inputs& in) {
    Output out;
    const long long now = in.now_ms;
    // Robot should be driving now. Anything else (pause, E-Stop, teardown,
    // stale status, an on-purpose hold) suppresses and resets everything.
    const bool active = in.gates_open && !in.robot_hold;

    const bool clock_runs = active && in.odom_fresh;
    if (!clock_runs || !t.anchor.valid ||
        madeProgress(t, in.pose, in.swept)) {
        out.progressed = clock_runs && t.anchor.valid;
        t.anchor = in.pose;
        t.anchor.valid = clock_runs && in.pose.valid;
        t.anchor_swept = in.swept;
        t.anchor_ms = now;
    }

    t.takeover_since = heldSince(t.takeover_since, in.takeover, now);
    t.pose_hold_since = heldSince(t.pose_hold_since, in.pose_hold, now);
    t.fault_since = heldSince(t.fault_since, in.fault, now);

    Kind want = Kind::None;
    if (active) {
        if (in.robot_speaks) {
            want = in.robot_kind;
        } else if (held(t.fault_since, now)) {
            want = Kind::Fault;
        } else if (held(t.takeover_since, now)) {
            want = Kind::Takeover;
        } else if (held(t.pose_hold_since, now)) {
            want = Kind::PoseHold;
        } else if (t.anchor.valid && now - t.anchor_ms >= kStallMs) {
            want = Kind::Stalled;
        }
    }

    // An episode ends on progress or when the robot should not be driving.
    // A held signal clearing alone does not end it: the robot is still
    // stuck, and a flapping flag must not re-prompt. A robot verdict is
    // followed exactly — its seq is the episode.
    const bool over =
        !active ||
        (in.robot_speaks
             ? want == Kind::None || in.robot_seq != t.robot_seq
             : out.progressed);
    if (active && in.robot_speaks && want == Kind::None) {
        t.quiet_seq = -1;
    }
    if (t.kind != Kind::None && over) {
        out.ended = true;
        t.kind = Kind::None;
        t.shown = false;
        t.dismissed = false;
    }
    if (want != Kind::None) {
        if (t.kind == Kind::None && in.robot_speaks && t.quiet_seq >= 0 &&
            in.robot_seq == t.quiet_seq) {
            // Same robot episode back after a gap: keep the Dismiss.
            t.shown = true;
            t.dismissed = true;
        }
        // Kind can change inside one episode (takeover clears, still stuck).
        t.kind = want;
        t.robot_seq = in.robot_seq;
        if (!t.shown) {
            out.show = true;
            t.shown = true;
        } else if (t.dismissed && now - t.dismissed_ms >= kRemindMs) {
            out.show = true;
            t.dismissed = false;
        }
    }
    out.kind = t.kind;
    out.stalled_ms = t.anchor.valid ? now - t.anchor_ms : 0;
    out.next = t;
    return out;
}

/** Operator pressed Dismiss: quiet until kRemindMs, unless the episode ends. */
inline Tracker onDismissed(Tracker t, long long now_ms) {
    if (t.kind != Kind::None) {
        t.dismissed = true;
        t.dismissed_ms = now_ms;
        t.quiet_seq = t.robot_seq;
    }
    return t;
}

/** Accept is honoured only while the episode it was raised for is open. */
inline bool acceptStillValid(const Tracker& t) {
    return t.kind != Kind::None;
}

}  // namespace attention_policy
}  // namespace f2c_cpp
