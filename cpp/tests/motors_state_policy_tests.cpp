/**
 * @file motors_state_policy_tests.cpp
 * @brief Stage 6 motor-state reading and the Arm-button blink rule.
 */

#include "motors_state_policy.hpp"

#include <gtest/gtest.h>

namespace policy = f2c_cpp::motors_state_policy;
using policy::MotorsState;

TEST(MotorsStatePolicy, StaleIsUnknownWhateverTheAxesSay) {
    EXPECT_EQ(policy::classify(8, 8, false), MotorsState::Unknown);
    EXPECT_EQ(policy::classify(1, 1, false), MotorsState::Unknown);
}

TEST(MotorsStatePolicy, BothClosedLoopIsArmed) {
    EXPECT_EQ(policy::classify(8, 8, true), MotorsState::Armed);
}

TEST(MotorsStatePolicy, BothIdleIsDisarmed) {
    EXPECT_EQ(policy::classify(1, 1, true), MotorsState::Disarmed);
}

TEST(MotorsStatePolicy, MixedIsSplit) {
    EXPECT_EQ(policy::classify(8, 1, true), MotorsState::Split);
    EXPECT_EQ(policy::classify(1, 8, true), MotorsState::Split);
    EXPECT_EQ(policy::classify(0, 0, true), MotorsState::Split);
}

TEST(MotorsStatePolicy, BlinksWhenDrivingByHandOnIdleAxes) {
    // E-Stop cleared, axes still IDLE, operator takes the camera view.
    EXPECT_TRUE(policy::shouldBlinkArm(true, true, MotorsState::Disarmed));
}

TEST(MotorsStatePolicy, StopsOnceArmed) {
    EXPECT_FALSE(policy::shouldBlinkArm(true, true, MotorsState::Armed));
}

TEST(MotorsStatePolicy, StopsWhenManualDriveEnds) {
    EXPECT_FALSE(policy::shouldBlinkArm(false, true, MotorsState::Disarmed));
}

TEST(MotorsStatePolicy, NoBlinkOnAGuess) {
    // A stale link must never start the hint.
    EXPECT_FALSE(policy::shouldBlinkArm(true, true, MotorsState::Unknown));
    EXPECT_FALSE(policy::shouldBlinkArm(true, true, MotorsState::Split));
}

TEST(MotorsStatePolicy, NoBlinkWhenNotCommandable) {
    // No mission, or the stack is being torn down.
    EXPECT_FALSE(policy::shouldBlinkArm(true, false, MotorsState::Disarmed));
}
