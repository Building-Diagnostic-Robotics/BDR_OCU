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
