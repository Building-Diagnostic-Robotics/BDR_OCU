/**
 * @file mission_done_notice_policy_tests.cpp
 * @brief Stage 6 "scan complete" notice rule.
 */

#include "mission_done_notice_policy.hpp"

#include <gtest/gtest.h>

namespace policy = f2c_cpp::mission_done_notice_policy;

namespace {
// The robot finished on its own during a live autonomous mission.
policy::Inputs finishedOnItsOwn() {
    policy::Inputs in;
    in.robot_complete = true;
    in.status_fresh = true;
    in.mission_active = true;
    in.tearing_down = false;
    in.autonomy_ran = true;
    in.operator_finishing = false;
    in.already_shown = false;
    return in;
}
}  // namespace

TEST(MissionDoneNoticePolicy, ShowsWhenRobotFinishesOnItsOwn) {
    EXPECT_TRUE(policy::shouldShow(finishedOnItsOwn()));
}

TEST(MissionDoneNoticePolicy, OnlyOncePerMission) {
    auto in = finishedOnItsOwn();
    in.already_shown = true;
    EXPECT_FALSE(policy::shouldShow(in));
}

TEST(MissionDoneNoticePolicy, SilentWhenOperatorFinished) {
    // Complete Mission (or "Finish scan" on the revisit prompt) also ends in
    // COMPLETE; the operator already knows.
    auto in = finishedOnItsOwn();
    in.operator_finishing = true;
    EXPECT_FALSE(policy::shouldShow(in));
}

TEST(MissionDoneNoticePolicy, SilentWhileNotComplete) {
    // Includes ERROR: a failed save outranks COMPLETE on the robot.
    auto in = finishedOnItsOwn();
    in.robot_complete = false;
    EXPECT_FALSE(policy::shouldShow(in));
}

TEST(MissionDoneNoticePolicy, SilentOnStaleStatus) {
    auto in = finishedOnItsOwn();
    in.status_fresh = false;
    EXPECT_FALSE(policy::shouldShow(in));
}

TEST(MissionDoneNoticePolicy, SilentWhenAutonomyNeverRan) {
    // A leftover COMPLETE from before this mission's first Start.
    auto in = finishedOnItsOwn();
    in.autonomy_ran = false;
    EXPECT_FALSE(policy::shouldShow(in));
}

TEST(MissionDoneNoticePolicy, SilentOutsideALiveMission) {
    auto in = finishedOnItsOwn();
    in.mission_active = false;
    EXPECT_FALSE(policy::shouldShow(in));
    in = finishedOnItsOwn();
    in.tearing_down = true;
    EXPECT_FALSE(policy::shouldShow(in));
}
