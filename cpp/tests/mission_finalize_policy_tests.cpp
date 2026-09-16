#include "mission_finalize_policy.hpp"

#include <gtest/gtest.h>

using f2c_cpp::finalize_policy::kAbortOfferTicks;
using f2c_cpp::finalize_policy::shouldOfferAbort;
using f2c_cpp::finalize_policy::shouldStampCompleted;

TEST(FinalizePolicy, NoOfferBeforeTickForty) {
    EXPECT_FALSE(shouldOfferAbort(false, kAbortOfferTicks - 1, false));
}

TEST(FinalizePolicy, OfferAtExactlyForty) {
    EXPECT_TRUE(shouldOfferAbort(false, kAbortOfferTicks, false));
}

TEST(FinalizePolicy, NoOfferOnceSaveDone) {
    EXPECT_FALSE(shouldOfferAbort(true, kAbortOfferTicks, false));
    EXPECT_FALSE(shouldOfferAbort(true, kAbortOfferTicks + 80, false));
}

TEST(FinalizePolicy, NoOfferOnceConsumed) {
    EXPECT_FALSE(shouldOfferAbort(false, kAbortOfferTicks, true));
    EXPECT_FALSE(shouldOfferAbort(false, kAbortOfferTicks + 80, true));
}

TEST(FinalizePolicy, StampOnSaveDone) {
    EXPECT_TRUE(shouldStampCompleted(true, false, false));
}

TEST(FinalizePolicy, StampOnStatusCompleteAlone) {
    EXPECT_TRUE(shouldStampCompleted(false, true, false));
}

TEST(FinalizePolicy, NoStampWhenNeitherDone) {
    EXPECT_FALSE(shouldStampCompleted(false, false, false));
}

TEST(FinalizePolicy, AbortSaveSkipsStamp) {
    EXPECT_FALSE(shouldStampCompleted(true, true, true));
    EXPECT_FALSE(shouldStampCompleted(true, false, true));
    EXPECT_FALSE(shouldStampCompleted(false, true, true));
}
