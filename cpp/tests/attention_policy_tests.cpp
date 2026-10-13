#include "attention_policy.hpp"

#include <gtest/gtest.h>

namespace ap = f2c_cpp::attention_policy;

namespace {

// Drives the policy at the 1 Hz status rate.
struct Sim {
    ap::Tracker t{};
    ap::Inputs in{};
    long long now = 1000000;
    int shows = 0;
    int ends = 0;
    ap::Kind last = ap::Kind::None;

    Sim() {
        in.gates_open = true;
        in.odom_fresh = true;
        in.pose = {0.0, 0.0, 0.0, true};
        in.swept = 10.0;
    }

    ap::Output tick() {
        in.now_ms = now;
        const ap::Output out = ap::step(t, in);
        t = out.next;
        shows += out.show ? 1 : 0;
        ends += out.ended ? 1 : 0;
        if (out.show) {
            last = out.kind;
        }
        now += 1000;
        return out;
    }

    void run(int seconds) {
        for (int i = 0; i < seconds; ++i) {
            tick();
        }
    }

    void dismiss() { t = ap::onDismissed(t, now); }
};

}  // namespace

TEST(AttentionPolicy, StationaryRobotPromptsAtStallTimeout) {
    Sim s;
    s.run(15);
    EXPECT_EQ(s.shows, 0);
    s.run(1);
    EXPECT_EQ(s.shows, 1);
    EXPECT_EQ(s.last, ap::Kind::Stalled);
}

// Field log 2026-10-11: handover read STALE_INPUT / obligation_done, robot
// drove again 3 s later. A short stationary gap never prompts.
TEST(AttentionPolicy, ShortHandoverGapNeverPrompts) {
    Sim s;
    for (int leg = 0; leg < 10; ++leg) {
        s.run(8);  // stopped, planning the next route
        s.in.pose.x += 1.0;
        s.run(1);
    }
    EXPECT_EQ(s.shows, 0);
}

TEST(AttentionPolicy, MovingRobotNeverPrompts) {
    Sim s;
    for (int i = 0; i < 120; ++i) {
        s.in.pose.x += 0.3;
        s.tick();
    }
    EXPECT_EQ(s.shows, 0);
}

TEST(AttentionPolicy, SmallJitterIsNotProgress) {
    Sim s;
    for (int i = 0; i < 16; ++i) {
        s.in.pose.x = (i % 2) ? 0.1 : 0.0;
        s.tick();
    }
    EXPECT_EQ(s.shows, 1);
}

TEST(AttentionPolicy, TurningInPlaceIsProgress) {
    Sim s;
    for (int i = 0; i < 60; ++i) {
        s.in.pose.yaw += 0.3;
        s.tick();
    }
    EXPECT_EQ(s.shows, 0);
}

TEST(AttentionPolicy, SweptRisingIsProgress) {
    Sim s;
    for (int i = 0; i < 60; ++i) {
        s.in.swept += 0.5;
        s.tick();
    }
    EXPECT_EQ(s.shows, 0);
}

TEST(AttentionPolicy, YawWrapIsAShortTurn) {
    EXPECT_NEAR(ap::angleDiff(3.1, -3.1), 0.083, 1e-3);
}

TEST(AttentionPolicy, ClosedGatesRestartTheClock) {
    Sim s;
    s.run(10);
    s.in.gates_open = false;  // paused / E-Stop / radio gap
    s.run(30);
    s.in.gates_open = true;
    s.run(15);
    EXPECT_EQ(s.shows, 0);
    s.run(2);
    EXPECT_EQ(s.shows, 1);
}

TEST(AttentionPolicy, RobotHoldRestartsTheClock) {
    Sim s;
    s.in.robot_hold = true;  // DC paused, rollover, topple latch
    s.run(60);
    EXPECT_EQ(s.shows, 0);
}

TEST(AttentionPolicy, StaleOdomRestartsTheClock) {
    Sim s;
    s.in.odom_fresh = false;
    s.run(60);
    EXPECT_EQ(s.shows, 0);
}

TEST(AttentionPolicy, ProgressEndsTheEpisode) {
    Sim s;
    s.run(16);
    ASSERT_EQ(s.shows, 1);
    s.in.pose.x += 1.0;
    const ap::Output out = s.tick();
    EXPECT_TRUE(out.ended);
    EXPECT_TRUE(out.progressed);
    EXPECT_EQ(s.t.kind, ap::Kind::None);
}

TEST(AttentionPolicy, NewEpisodeNeedsAFreshStall) {
    Sim s;
    s.run(16);
    s.in.pose.x += 1.0;
    s.tick();
    s.run(14);
    EXPECT_EQ(s.shows, 1);
    s.run(2);
    EXPECT_EQ(s.shows, 2);
}

TEST(AttentionPolicy, OpenPromptIsNotRepeated) {
    Sim s;
    s.run(200);
    EXPECT_EQ(s.shows, 1);
}

TEST(AttentionPolicy, DismissRemindsEveryMinute) {
    Sim s;
    s.run(16);
    s.dismiss();
    s.run(59);
    EXPECT_EQ(s.shows, 1);
    s.run(2);
    EXPECT_EQ(s.shows, 2);
    s.dismiss();
    s.run(61);
    EXPECT_EQ(s.shows, 3);
}

TEST(AttentionPolicy, DismissOutsideAnEpisodeIsIgnored) {
    ap::Tracker t{};
    t = ap::onDismissed(t, 5);
    EXPECT_FALSE(t.dismissed);
}

TEST(AttentionPolicy, TakeoverPromptsAfterHoldConfirm) {
    Sim s;
    s.in.takeover = true;
    s.run(2);
    EXPECT_EQ(s.shows, 0);
    s.run(1);
    EXPECT_EQ(s.shows, 1);
    EXPECT_EQ(s.last, ap::Kind::Takeover);
}

TEST(AttentionPolicy, OneSampleTakeoverIsIgnored) {
    Sim s;
    for (int i = 0; i < 10; ++i) {
        s.in.takeover = (i % 2) == 0;
        s.in.pose.x += 0.3;
        s.tick();
    }
    EXPECT_EQ(s.shows, 0);
}

TEST(AttentionPolicy, PoseHoldPrompts) {
    Sim s;
    s.in.pose_hold = true;
    s.run(3);
    EXPECT_EQ(s.last, ap::Kind::PoseHold);
}

TEST(AttentionPolicy, FaultOutranksTakeover) {
    Sim s;
    s.in.fault = true;
    s.in.takeover = true;
    s.run(3);
    EXPECT_EQ(s.last, ap::Kind::Fault);
}

TEST(AttentionPolicy, TakeoverClearingIntoStallKeepsTheDismiss) {
    Sim s;
    s.in.takeover = true;
    s.run(3);
    s.dismiss();
    s.in.takeover = false;  // still not moving
    s.run(20);
    EXPECT_EQ(s.shows, 1);
    EXPECT_EQ(s.t.kind, ap::Kind::Stalled);
}

TEST(AttentionPolicy, FlappingTakeoverWhileStuckPromptsOnce) {
    Sim s;
    for (int i = 0; i < 50; ++i) {
        s.in.takeover = (i / 3) % 2 == 0;
        s.tick();
    }
    EXPECT_EQ(s.shows, 1);
}

TEST(AttentionPolicy, RobotStalledVerdictIgnoresOcuProgress) {
    Sim s;
    s.in.robot_speaks = true;
    s.in.robot_kind = ap::Kind::Stalled;
    s.in.robot_seq = 7;
    for (int i = 0; i < 10; ++i) {
        s.in.pose.x += 1.0;
        s.tick();
    }
    EXPECT_EQ(s.shows, 1);
    EXPECT_EQ(s.ends, 0);
}

TEST(AttentionPolicy, NoPromptWhileGatesClosed) {
    Sim s;
    s.in.gates_open = false;
    s.in.takeover = true;
    s.in.fault = true;
    s.run(30);
    EXPECT_EQ(s.shows, 0);
}

TEST(AttentionPolicy, ClosingGatesEndsAnOpenEpisode) {
    Sim s;
    s.run(16);
    s.in.gates_open = false;
    const ap::Output out = s.tick();
    EXPECT_TRUE(out.ended);
    EXPECT_FALSE(ap::acceptStillValid(s.t));
}

TEST(AttentionPolicy, AcceptValidOnlyDuringEpisode) {
    Sim s;
    EXPECT_FALSE(ap::acceptStillValid(s.t));
    s.run(16);
    EXPECT_TRUE(ap::acceptStillValid(s.t));
}

TEST(AttentionPolicy, RobotVerdictWins) {
    Sim s;
    s.in.robot_speaks = true;
    s.in.robot_kind = ap::Kind::None;
    s.run(60);  // stationary, but the robot says it is fine
    EXPECT_EQ(s.shows, 0);
    s.in.robot_kind = ap::Kind::Stalled;
    s.in.robot_seq = 4;
    s.tick();
    EXPECT_EQ(s.shows, 1);
}

TEST(AttentionPolicy, RobotNewSeqIsANewEpisode) {
    Sim s;
    s.in.robot_speaks = true;
    s.in.robot_kind = ap::Kind::Stalled;
    s.in.robot_seq = 1;
    s.tick();
    s.dismiss();
    s.run(5);
    EXPECT_EQ(s.shows, 1);
    s.in.robot_seq = 2;
    s.tick();
    EXPECT_EQ(s.shows, 2);
    EXPECT_EQ(s.ends, 1);
}

// A Microhard fade closes the gates for a few seconds. The robot is still
// reporting the episode the operator dismissed: no re-prompt until reminder.
TEST(AttentionPolicy, RadioGapKeepsTheDismissOfTheSameRobotEpisode) {
    Sim s;
    s.in.robot_speaks = true;
    s.in.robot_kind = ap::Kind::Stalled;
    s.in.robot_seq = 3;
    s.tick();
    ASSERT_EQ(s.shows, 1);
    s.dismiss();
    s.in.gates_open = false;
    s.run(5);
    s.in.gates_open = true;
    s.run(50);
    EXPECT_EQ(s.shows, 1);
    s.run(10);  // 60 s after the dismiss
    EXPECT_EQ(s.shows, 2);
}

TEST(AttentionPolicy, RobotClearingEndsTheQuiet) {
    Sim s;
    s.in.robot_speaks = true;
    s.in.robot_kind = ap::Kind::Stalled;
    s.in.robot_seq = 3;
    s.tick();
    s.dismiss();
    s.in.robot_kind = ap::Kind::None;  // robot moving again
    s.tick();
    EXPECT_EQ(s.t.quiet_seq, -1);
    s.in.robot_kind = ap::Kind::Stalled;
    s.in.robot_seq = 4;
    s.tick();
    EXPECT_EQ(s.shows, 2);
}

TEST(AttentionPolicy, NewRobotSeqAfterAGapPrompts) {
    Sim s;
    s.in.robot_speaks = true;
    s.in.robot_kind = ap::Kind::Stalled;
    s.in.robot_seq = 3;
    s.tick();
    s.dismiss();
    s.in.gates_open = false;
    s.run(5);
    s.in.gates_open = true;
    s.in.robot_seq = 4;
    s.tick();
    EXPECT_EQ(s.shows, 2);
}

TEST(AttentionPolicy, OcuModeGapDoesNotInheritAQuiet) {
    Sim s;
    s.run(16);
    s.dismiss();
    s.in.gates_open = false;
    s.tick();
    s.in.gates_open = true;
    s.run(16);
    EXPECT_EQ(s.shows, 2);
}

TEST(AttentionPolicy, RobotVerdictStillRespectsGates) {
    Sim s;
    s.in.robot_speaks = true;
    s.in.robot_kind = ap::Kind::Takeover;
    s.in.gates_open = false;
    s.run(10);
    EXPECT_EQ(s.shows, 0);
}
