/**
 * @file mission_done_notice_policy_tests.cpp
 * @brief Stage 6 "scan complete" notice rule — must never misfire.
 */

#include "mission_done_notice_policy.hpp"

#include <gtest/gtest.h>

namespace policy = f2c_cpp::mission_done_notice_policy;

namespace {
policy::Inputs live(bool complete) {
    policy::Inputs in;
    in.status_fresh = true;
    in.robot_complete = complete;
    in.mission_live = true;
    in.autonomy_ran = true;
    return in;
}

// Robot seen working after Start, then reports COMPLETE.
policy::Tracker afterWorking() {
    return policy::step({}, live(false)).next;
}
}  // namespace

TEST(MissionDoneNoticePolicy, ShowsOnWorkingToComplete) {
    const auto out = policy::step(afterWorking(), live(true));
    EXPECT_TRUE(out.show);
    EXPECT_TRUE(out.next.shown);
}

TEST(MissionDoneNoticePolicy, OnlyOncePerMission) {
    auto t = policy::step(afterWorking(), live(true)).next;
    EXPECT_FALSE(policy::step(t, live(true)).show);
    // Even if the state wobbles and returns.
    t = policy::step(t, live(false)).next;
    EXPECT_FALSE(policy::step(t, live(true)).show);
}

TEST(MissionDoneNoticePolicy, NeverOnACompleteThatWasAlreadyThere) {
    // First fresh status of the mission already reads COMPLETE.
    EXPECT_FALSE(policy::step({}, live(true)).show);
}

TEST(MissionDoneNoticePolicy, WorkingBeforeAutonomyDoesNotCount) {
    // Status seen before Start (autonomy never ran) does not arm it.
    auto in = live(false);
    in.autonomy_ran = false;
    const auto t = policy::step({}, in).next;
    EXPECT_FALSE(t.seen_working);
    EXPECT_FALSE(policy::step(t, live(true)).show);
}

TEST(MissionDoneNoticePolicy, SilentAfterAnyOperatorEnd) {
    // Complete Mission / Finish scan / Cancel, even if later cancelled.
    auto in = live(true);
    in.operator_ended = true;
    EXPECT_FALSE(policy::step(afterWorking(), in).show);
}

TEST(MissionDoneNoticePolicy, StaleStatusChangesNothing) {
    auto in = live(true);
    in.status_fresh = false;
    const auto out = policy::step(afterWorking(), in);
    EXPECT_FALSE(out.show);
    EXPECT_FALSE(out.close);
    EXPECT_FALSE(out.next.shown);
}

TEST(MissionDoneNoticePolicy, NothingOutsideALiveMission) {
    auto in = live(true);
    in.mission_live = false;  // no mission, or tearing down
    EXPECT_FALSE(policy::step(afterWorking(), in).show);
}

TEST(MissionDoneNoticePolicy, WaitsWhileDrivingByHand) {
    auto in = live(true);
    in.manual_override = true;
    const auto out = policy::step(afterWorking(), in);
    EXPECT_FALSE(out.show);
    EXPECT_FALSE(out.next.shown);
    // Shown once the operator lets go.
    EXPECT_TRUE(policy::step(out.next, live(true)).show);
}

TEST(MissionDoneNoticePolicy, ClosesWhenStateLeavesComplete) {
    // e.g. ERROR after a late save failure.
    auto t = policy::step(afterWorking(), live(true)).next;
    EXPECT_TRUE(policy::step(t, live(false)).close);
}
