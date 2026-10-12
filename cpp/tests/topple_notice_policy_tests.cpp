/**
 * @file topple_notice_policy_tests.cpp
 * @brief Stage 6 topple-trip edge detection from /coverage/status tip_seq.
 */

#include "topple_notice_policy.hpp"

#include <gtest/gtest.h>

namespace policy = f2c_cpp::topple_notice_policy;

TEST(ToppleNoticePolicy, NoTripsNeverEngages) {
    // First status of a mission, and every quiet one after it.
    auto d = policy::onTripSeq(policy::kUnseen, 0);
    EXPECT_FALSE(d.engage);
    EXPECT_EQ(d.seen, 0);
    d = policy::onTripSeq(d.seen, 0);
    EXPECT_FALSE(d.engage);
}

TEST(ToppleNoticePolicy, EachNewTripEngagesExactlyOnce) {
    auto d = policy::onTripSeq(0, 1);
    EXPECT_TRUE(d.engage);
    EXPECT_EQ(d.seen, 1);
    // The same line repeats at 1 Hz for the rest of the run.
    for (int i = 0; i < 5; ++i) {
        d = policy::onTripSeq(d.seen, 1);
        EXPECT_FALSE(d.engage);
    }
    d = policy::onTripSeq(d.seen, 2);
    EXPECT_TRUE(d.engage);
}

TEST(ToppleNoticePolicy, ATripMissedBetweenStatusLinesStillEngages) {
    // 0 -> 2: two trips landed while the radio was quiet. One engage is
    // enough; the latch is already the right state for both.
    const auto d = policy::onTripSeq(0, 2);
    EXPECT_TRUE(d.engage);
    EXPECT_EQ(d.seen, 2);
}

TEST(ToppleNoticePolicy, ATripAlreadyCountedOnFirstStatusEngages) {
    // The screen joined a run that had tripped before it was listening.
    EXPECT_TRUE(policy::onTripSeq(policy::kUnseen, 3).engage);
}

TEST(ToppleNoticePolicy, DirectorRestartIsFollowedDownWithoutEngaging) {
    auto d = policy::onTripSeq(2, 0);
    EXPECT_FALSE(d.engage);
    EXPECT_EQ(d.seen, 0);
    // ...so the restarted director's first trip is not hidden behind the old
    // count.
    d = policy::onTripSeq(d.seen, 1);
    EXPECT_TRUE(d.engage);
}

TEST(ToppleNoticePolicy, GarbageIsIgnored) {
    const auto d = policy::onTripSeq(1, -5);
    EXPECT_FALSE(d.engage);
    EXPECT_EQ(d.seen, 1);
}

TEST(ToppleNoticePolicy, BlocksArmingOnlyOnAFreshLatch) {
    EXPECT_TRUE(policy::blocksArming(true, true));
    EXPECT_FALSE(policy::blocksArming(false, true));
    EXPECT_FALSE(policy::blocksArming(true, false));
    EXPECT_FALSE(policy::blocksArming(false, false));
}

TEST(ToppleNoticePolicy, RefusalNeedsASampleAfterTheEnable) {
    EXPECT_TRUE(policy::refusedWhileRunning(true, true, 2000, 1000));
    // No enable was sent this mission.
    EXPECT_FALSE(policy::refusedWhileRunning(true, true, 2000, 0));
    // This sample was already in flight when the enable went out.
    EXPECT_FALSE(policy::refusedWhileRunning(true, true, 1000, 1000));
    EXPECT_FALSE(policy::refusedWhileRunning(true, true, 999, 1000));
    EXPECT_FALSE(policy::refusedWhileRunning(false, true, 2000, 1000));
    EXPECT_FALSE(policy::refusedWhileRunning(true, false, 2000, 1000));
}

TEST(ToppleNoticePolicy, ANewTripIsNotAlsoARefusal) {
    // maybeHandleTopple() engages the E-Stop before the refusal check, and
    // that leaves Running. A sample that both counts a new trip and shows
    // the latch must not also take the refusal path.
    const auto trip = policy::onTripSeq(0, 1);
    ASSERT_TRUE(trip.engage);
    EXPECT_FALSE(policy::refusedWhileRunning(
        /*running=*/false, /*tip_latched=*/true, 2000, 1000));
}

TEST(ToppleNoticePolicy, GuardOfflineWarnsOnceAfterTheThreshold) {
    policy::GuardWatch watch;
    auto step = policy::onTipReady(watch, false, 0);
    EXPECT_FALSE(step.warn_now);
    EXPECT_EQ(step.next.not_ready_since_ms, 0);

    step = policy::onTipReady(step.next, false, 9999);
    EXPECT_FALSE(step.warn_now);

    step = policy::onTipReady(step.next, false, policy::kGuardOfflineWarnMs);
    EXPECT_TRUE(step.warn_now);
    EXPECT_FALSE(step.recovered_now);

    step = policy::onTipReady(step.next, false, 20000);
    EXPECT_FALSE(step.warn_now);

    step = policy::onTipReady(step.next, true, 21000);
    EXPECT_TRUE(step.recovered_now);
    EXPECT_FALSE(step.warn_now);
    EXPECT_EQ(step.next.not_ready_since_ms, -1);
    EXPECT_FALSE(step.next.warned);

    step = policy::onTipReady(step.next, true, 22000);
    EXPECT_FALSE(step.recovered_now);
}

TEST(ToppleNoticePolicy, AShortGuardBlipResetsSilently) {
    auto step = policy::onTipReady({}, false, 0);
    step = policy::onTipReady(step.next, true, 5000);
    EXPECT_FALSE(step.warn_now);
    EXPECT_FALSE(step.recovered_now);
    EXPECT_EQ(step.next.not_ready_since_ms, -1);

    step = policy::onTipReady(step.next, false, 5000);
    step = policy::onTipReady(step.next, false, 14999);
    EXPECT_FALSE(step.warn_now);
    EXPECT_FALSE(step.recovered_now);
}
