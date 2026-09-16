#include "stop_prompt_policy.hpp"

#include <gtest/gtest.h>

using f2c_cpp::stop_prompt_policy::advanceStopDwell;
using f2c_cpp::stop_prompt_policy::kStopDwellSamples;
using f2c_cpp::stop_prompt_policy::stopPromptDue;

TEST(StopPromptPolicy, StoppedSamplesAccumulate) {
    EXPECT_EQ(advanceStopDwell(0, true), 1);
    EXPECT_EQ(advanceStopDwell(1, true), 2);
    EXPECT_EQ(advanceStopDwell(2, true), kStopDwellSamples);
}

TEST(StopPromptPolicy, HealthySampleZeroes) {
    EXPECT_EQ(advanceStopDwell(kStopDwellSamples, false), 0);
    EXPECT_EQ(advanceStopDwell(99, false), 0);
}

TEST(StopPromptPolicy, NotDueBelowThreshold) {
    EXPECT_FALSE(stopPromptDue(0, false, false));
    EXPECT_FALSE(stopPromptDue(kStopDwellSamples - 1, false, false));
}

TEST(StopPromptPolicy, DueAtExactlyThreshold) {
    EXPECT_TRUE(stopPromptDue(kStopDwellSamples, false, false));
    EXPECT_TRUE(stopPromptDue(kStopDwellSamples + 4, false, false));
}

TEST(StopPromptPolicy, NoRepromptOnceExplained) {
    EXPECT_FALSE(stopPromptDue(kStopDwellSamples, true, false));
}

TEST(StopPromptPolicy, NoPromptWhileOpen) {
    EXPECT_FALSE(stopPromptDue(kStopDwellSamples, false, true));
}

// A one-tick degenerate_path at the end of every sweep: stopped sample,
// healthy sample, repeat. This is the false positive the dwell exists for.
TEST(StopPromptPolicy, SweepEndBlipsNeverPrompt) {
    int samples = 0;
    for (int i = 0; i < 8; ++i) {
        samples = advanceStopDwell(samples, i % 2 == 0);
        EXPECT_FALSE(stopPromptDue(samples, false, false)) << "sample " << i;
    }
}

// One obstruction whose reason wanders as the executor retries. Keying the
// dwell to a matching reason used to reset the count here forever.
TEST(StopPromptPolicy, WanderingReasonStillPrompts) {
    int samples = advanceStopDwell(0, true);          // blocked_persistent
    samples = advanceStopDwell(samples, true);        // route_invalid
    EXPECT_FALSE(stopPromptDue(samples, false, false));
    samples = advanceStopDwell(samples, true);        // replan_deferred
    EXPECT_TRUE(stopPromptDue(samples, false, false));
}
