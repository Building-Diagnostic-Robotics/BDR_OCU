/**
 * @file estop_latch_policy_tests.cpp
 * @brief Stage 6 E-Stop latch transitions, including the 2026-09-16 incident.
 */

#include "estop_latch_policy.hpp"

#include <gtest/gtest.h>

using f2c_cpp::estop_latch_policy::RunState;
namespace policy = f2c_cpp::estop_latch_policy;

TEST(EstopLatchPolicy, EmergencyStopWinsFromEveryState) {
    for (const RunState state :
         {RunState::Idle, RunState::Running, RunState::Paused,
          RunState::EmergencyStopped, RunState::Completed}) {
        EXPECT_EQ(policy::onEmergencyStop(state), RunState::EmergencyStopped);
    }
}

TEST(EstopLatchPolicy, ArmingRefusedOnlyWhileLatched) {
    EXPECT_FALSE(policy::armingAllowed(RunState::EmergencyStopped));
    for (const RunState state : {RunState::Idle, RunState::Running,
                                 RunState::Paused, RunState::Completed}) {
        EXPECT_TRUE(policy::armingAllowed(state));
    }
}

TEST(EstopLatchPolicy, ClearingDoesNotArm) {
    // Paused, never Running: the operator still has to press Resume.
    EXPECT_EQ(policy::clearEmergencyStop(RunState::EmergencyStopped),
              RunState::Paused);
}

TEST(EstopLatchPolicy, ClearingIsInertOnEveryOtherState) {
    for (const RunState state : {RunState::Idle, RunState::Running,
                                 RunState::Paused, RunState::Completed}) {
        EXPECT_EQ(policy::clearEmergencyStop(state), state);
    }
}

TEST(EstopLatchPolicy, ManualOverrideDoesNotLiftTheLatch) {
    EXPECT_EQ(policy::pauseIfRunning(RunState::EmergencyStopped),
              RunState::EmergencyStopped);
    EXPECT_EQ(policy::pauseIfRunning(RunState::Running), RunState::Paused);
    EXPECT_EQ(policy::pauseIfRunning(RunState::Idle), RunState::Idle);
    EXPECT_EQ(policy::pauseIfRunning(RunState::Completed),
              RunState::Completed);
}

TEST(EstopLatchPolicy, ReleaseNeverResumesAutonomy) {
    for (const RunState state :
         {RunState::Idle, RunState::Running, RunState::Paused,
          RunState::EmergencyStopped, RunState::Completed}) {
        EXPECT_FALSE(policy::releaseResumesAutonomy(state));
    }
}

TEST(EstopLatchPolicy, LatchOwnsTheStatusPill) {
    EXPECT_TRUE(policy::holdsStatusPill(RunState::EmergencyStopped));
    for (const RunState state : {RunState::Idle, RunState::Running,
                                 RunState::Paused, RunState::Completed}) {
        EXPECT_FALSE(policy::holdsStatusPill(state));
    }
}

// The space bar is stop-only: SatelliteScreen::onEstopShortcut calls
// onEmergencyStop and never clearEmergencyStop, so any number of presses
// leaves the robot stopped.
TEST(EstopLatchPolicy, RepeatedEmergencyStopsAreIdempotent) {
    RunState state = RunState::Running;
    for (int i = 0; i < 5; ++i) {
        state = policy::onEmergencyStop(state);
        EXPECT_EQ(state, RunState::EmergencyStopped);
        EXPECT_FALSE(policy::armingAllowed(state));
    }
}

// Why the shortcut must not route through the button's toggle: an operator
// mashing a panic key an even number of times would release the very stop
// they were applying. This is the hazard, made executable.
TEST(EstopLatchPolicy, ToggleWouldReleaseOnAnEvenNumberOfTaps) {
    const auto toggle = [](RunState s) {
        return s == RunState::EmergencyStopped ? policy::clearEmergencyStop(s)
                                              : policy::onEmergencyStop(s);
    };
    RunState toggled = RunState::Running;
    toggled = toggle(toggled);
    toggled = toggle(toggled);
    EXPECT_NE(toggled, RunState::EmergencyStopped);   // the hazard
    EXPECT_TRUE(policy::armingAllowed(toggled));

    RunState stop_only = RunState::Running;
    stop_only = policy::onEmergencyStop(stop_only);
    stop_only = policy::onEmergencyStop(stop_only);
    EXPECT_EQ(stop_only, RunState::EmergencyStopped);
    EXPECT_FALSE(policy::armingAllowed(stop_only));
}

// The incident, mission_20260916_135241.log, 14:15:56 onward. The operator
// E-Stopped next to a roof edge, took manual control, then handed it back by
// clicking the map. Pre-fix that last step re-armed CLOSED_LOOP and
// republished autonomy_enable=true, three times, and the robot drove at the
// edge. The run must stay latched across all of it.
TEST(EstopLatchPolicy, IncidentSequenceNeverRearms) {
    RunState state = RunState::Running;

    state = policy::onEmergencyStop(state);
    ASSERT_EQ(state, RunState::EmergencyStopped);
    EXPECT_FALSE(policy::armingAllowed(state));

    state = policy::pauseIfRunning(state);   // teleop engaged
    EXPECT_EQ(state, RunState::EmergencyStopped);
    EXPECT_FALSE(policy::armingAllowed(state));

    // Handing control back: no resume, latch intact.
    EXPECT_FALSE(policy::releaseResumesAutonomy(state));
    EXPECT_EQ(state, RunState::EmergencyStopped);
    EXPECT_FALSE(policy::armingAllowed(state));

    // Three map clicks in a row cannot arm it either.
    for (int i = 0; i < 3; ++i) {
        EXPECT_FALSE(policy::releaseResumesAutonomy(state));
        EXPECT_FALSE(policy::armingAllowed(state));
    }

    // Only the explicit clear opens the door, and only as far as Paused.
    state = policy::clearEmergencyStop(state);
    EXPECT_EQ(state, RunState::Paused);
    EXPECT_TRUE(policy::armingAllowed(state));
}
