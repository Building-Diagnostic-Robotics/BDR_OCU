#include "link_health_monitor.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QThread>

namespace {

using f2c_cpp::LinkHealthMonitor;

TEST(LinkHealthMonitor, ArmStartsConnectingAndAStampMakesItHealthy) {
    LinkHealthMonitor monitor;
    EXPECT_FALSE(monitor.isArmed());
    EXPECT_EQ(monitor.state(), LinkHealthMonitor::State::Idle);

    monitor.arm();
    EXPECT_TRUE(monitor.isArmed());
    EXPECT_EQ(monitor.state(), LinkHealthMonitor::State::Connecting);
    EXPECT_EQ(monitor.msSinceLastSeen(), -1);
    EXPECT_GT(monitor.armedAtMs(), 0);

    monitor.stamp(LinkHealthMonitor::Source::Odom);
    EXPECT_EQ(monitor.state(), LinkHealthMonitor::State::Healthy);
    EXPECT_GE(monitor.msSinceLastSeen(), 0);
}

TEST(LinkHealthMonitor, ArmAgainDropsThePreviousSession) {
    LinkHealthMonitor monitor;
    monitor.arm();
    monitor.stamp(LinkHealthMonitor::Source::Odom);
    monitor.setReachability(true, true);
    ASSERT_EQ(monitor.state(), LinkHealthMonitor::State::Healthy);
    const qint64 first_arm = monitor.armedAtMs();

    // A second mission must not inherit Healthy, the old stamp, or the
    // old reachability. Offline-with-a-running-timer was that bug.
    while (QDateTime::currentMSecsSinceEpoch() <= first_arm) {
        QThread::msleep(1);
    }
    monitor.arm();
    EXPECT_EQ(monitor.state(), LinkHealthMonitor::State::Connecting);
    EXPECT_EQ(monitor.msSinceLastSeen(), -1);
    EXPECT_GT(monitor.armedAtMs(), first_arm);

    // A probe that is already Reachable must not end Connecting. There
    // has still been no stamp this session.
    monitor.setReachability(true, true);
    EXPECT_EQ(monitor.state(), LinkHealthMonitor::State::Connecting);
}

TEST(LinkHealthMonitor, StampWhileDisarmedDoesNothing) {
    LinkHealthMonitor monitor;
    monitor.stamp(LinkHealthMonitor::Source::Odom);
    EXPECT_EQ(monitor.state(), LinkHealthMonitor::State::Idle);
    EXPECT_FALSE(monitor.isArmed());
}

TEST(LinkHealthMonitor, ConnectingExpiresToDisconnectedEvenIfReachable) {
    using State = LinkHealthMonitor::State;
    constexpr qint64 armed = 1'000'000;
    constexpr qint64 limit = LinkHealthMonitor::kConnectingTimeoutMs;

    EXPECT_EQ(LinkHealthMonitor::deriveLinkState(armed + limit - 1, armed, 0,
                                                 true, true),
              State::Connecting);
    EXPECT_EQ(LinkHealthMonitor::deriveLinkState(armed + limit, armed, 0, true,
                                                 true),
              State::Disconnected);
    EXPECT_EQ(LinkHealthMonitor::deriveLinkState(armed + limit, armed, 0,
                                                 false, false),
              State::Disconnected);

    // A stamp ends the wait. The timeout does not apply once this
    // session has spoken.
    EXPECT_EQ(LinkHealthMonitor::deriveLinkState(armed + limit + 5'000, armed,
                                                 armed + limit + 4'000, true,
                                                 true),
              State::Healthy);
    // After contact, a stale stamp with a live host is Reconnecting.
    EXPECT_EQ(LinkHealthMonitor::deriveLinkState(armed + limit + 30'000, armed,
                                                 armed + 1'000, true, true),
              State::Reconnecting);
}

TEST(LinkHealthMonitor, SilenceCountsFromArmUntilFirstContact) {
    LinkHealthMonitor monitor;
    EXPECT_EQ(monitor.msWithoutContact(), -1);
    EXPECT_FALSE(monitor.hadContact());

    monitor.arm();
    EXPECT_FALSE(monitor.hadContact());
    const qint64 armed = monitor.armedAtMs();
    while (QDateTime::currentMSecsSinceEpoch() < armed + 5) {
        QThread::msleep(1);
    }
    // No stamp yet: silence is measured from arm(), not reported as 0.
    EXPECT_GE(monitor.msWithoutContact(), 5);

    monitor.stamp(LinkHealthMonitor::Source::Odom);
    EXPECT_TRUE(monitor.hadContact());
    EXPECT_GE(monitor.msWithoutContact(), 0);
    EXPECT_LT(monitor.msWithoutContact(), 1000);

    monitor.disarm();
    EXPECT_EQ(monitor.msWithoutContact(), -1);
    EXPECT_FALSE(monitor.hadContact());
}

TEST(LinkHealthMonitor, DisarmReturnsToIdle) {
    LinkHealthMonitor monitor;
    int emissions = 0;
    QObject::connect(
        &monitor, &LinkHealthMonitor::linkStateChanged,
        [&](LinkHealthMonitor::State, LinkHealthMonitor::State next, qint64) {
            ++emissions;
            if (emissions == 2) {
                EXPECT_EQ(next, LinkHealthMonitor::State::Idle);
            }
        });
    monitor.arm();
    EXPECT_EQ(emissions, 1);
    monitor.disarm();
    EXPECT_EQ(emissions, 2);
    EXPECT_FALSE(monitor.isArmed());
    EXPECT_EQ(monitor.state(), LinkHealthMonitor::State::Idle);
    EXPECT_EQ(monitor.armedAtMs(), 0);
    monitor.disarm();
    EXPECT_EQ(emissions, 2);
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
