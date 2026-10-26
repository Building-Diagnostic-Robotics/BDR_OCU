#include "mission_finalize_policy.hpp"

#include <gtest/gtest.h>

using f2c_cpp::finalize_policy::kAbortOfferTicks;
using f2c_cpp::finalize_policy::offlineFinalizeLanded;
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

TEST(FinalizePolicy, OfflineFinalizeNeedsTheFinalizedFlag) {
    EXPECT_TRUE(offlineFinalizeLanded(
        0, R"({"finalized": true, "mission_folder": "/R_DATA/x", "section_count": 2})"));
    EXPECT_TRUE(offlineFinalizeLanded(0, R"({"finalized":true})"));
    // SSH banner noise ahead of the JSON line does not hide it.
    EXPECT_TRUE(offlineFinalizeLanded(
        0, "Warning: Permanently added 'roofus'\n{\"finalized\": true}\n"));
}

TEST(FinalizePolicy, OfflineFinalizeNoOpOrFailureDoesNotLand) {
    // The script exits 0 when there is no open mission. That is the
    // never-reached-the-robot launch, and it must not stamp COMPLETED.
    EXPECT_FALSE(offlineFinalizeLanded(
        0, R"({"finalized": false, "reason": "no open mission found"})"));
    EXPECT_FALSE(offlineFinalizeLanded(0, ""));
    EXPECT_FALSE(offlineFinalizeLanded(3, R"({"finalized": true})"));
    EXPECT_FALSE(offlineFinalizeLanded(-1, ""));
}

// --- Settle: never stop the robot mid-save (Ansys 2645 Zanker Rd, 2026-10-14)
// The director sets copy=pending when /dc/end_and_save is issued. The OCU
// counted pending as saved and stopped the stack 0.4 s later; a 77-minute
// scan's map save was killed and the mission never reached the stick.

using f2c_cpp::finalize_policy::kLinkLostSettleTicks;
using f2c_cpp::finalize_policy::kSaveSettleCeilingTicks;
using f2c_cpp::finalize_policy::saveInFlight;
using f2c_cpp::finalize_policy::saveReportedFailed;
using f2c_cpp::finalize_policy::saveSettled;
using f2c_cpp::finalize_policy::settleStep;
using f2c_cpp::finalize_policy::SettleStep;

TEST(FinalizePolicy, PendingIsNotSaved) {
    EXPECT_FALSE(saveSettled(false, false, "pending"));
    EXPECT_TRUE(saveInFlight(false, "pending"));
    EXPECT_FALSE(saveSettled(false, false, "idle"));
    EXPECT_FALSE(saveSettled(false, false, ""));
}

TEST(FinalizePolicy, HandedOffCopyStatesAreSaved) {
    for (const char* copy : {"queued", "waiting_for_drive", "copying", "done",
                             "skipped", "failed", "stick_full"}) {
        EXPECT_TRUE(saveSettled(false, false, copy)) << copy;
        EXPECT_FALSE(saveInFlight(false, copy)) << copy;
    }
    EXPECT_TRUE(saveSettled(false, true, "pending"));
}

TEST(FinalizePolicy, DirectorSavingFlagOverridesEverything) {
    // Copy disabled: the director latches complete as soon as the save is
    // issued, and copy stays idle. Only the saving flag shows the truth.
    EXPECT_FALSE(saveSettled(true, true, "idle"));
    EXPECT_FALSE(saveSettled(true, true, "done"));
    EXPECT_TRUE(saveInFlight(true, "idle"));
    EXPECT_TRUE(saveSettled(false, true, "idle"));
}

TEST(FinalizePolicy, LinkLostMidSaveLeavesTheRobotRunning) {
    EXPECT_EQ(settleStep(false, true, false, 500, kLinkLostSettleTicks),
              SettleStep::LeaveRobotRunning);
    EXPECT_EQ(settleStep(false, false, false, 500, kLinkLostSettleTicks),
              SettleStep::ProceedUnconfirmed);
}

TEST(FinalizePolicy, DirectorSaveErrorIsRecognised) {
    EXPECT_TRUE(saveReportedFailed(
        "coverage data is NOT saved: /dc/end_and_save failed"));
    EXPECT_FALSE(saveReportedFailed(""));
    EXPECT_FALSE(saveReportedFailed("GPR rollover failed: timeout"));
}

TEST(FinalizePolicy, LongSaveKeepsTheStackUpWhileTheRobotReports) {
    // Ansys timeline: copy stays pending for minutes, status keeps arriving.
    for (int ticks : {1, 4, kLinkLostSettleTicks, 600, kSaveSettleCeilingTicks - 1}) {
        EXPECT_EQ(settleStep(false, true, false, ticks, 0), SettleStep::Wait)
            << ticks;
    }
    EXPECT_EQ(settleStep(false, true, false, kSaveSettleCeilingTicks, 0),
              SettleStep::ProceedUnconfirmed);
}

TEST(FinalizePolicy, ProceedsTheTickTheSaveLands) {
    EXPECT_EQ(settleStep(true, false, false, 3, 0), SettleStep::Proceed);
    EXPECT_EQ(settleStep(true, false, false, 900, 0), SettleStep::Proceed);
}

TEST(FinalizePolicy, LostLinkStopsWaiting) {
    EXPECT_EQ(settleStep(false, true, false, 500, kLinkLostSettleTicks - 1),
              SettleStep::Wait);
    EXPECT_NE(settleStep(false, true, false, 500, kLinkLostSettleTicks),
              SettleStep::Wait);
}

TEST(FinalizePolicy, NoSaveInFlightKeepsThirtySeconds) {
    // Conclude hung or refused: nothing is being written, keep the old ceiling.
    EXPECT_EQ(settleStep(false, false, false, kLinkLostSettleTicks - 1, 0),
              SettleStep::Wait);
    EXPECT_EQ(settleStep(false, false, false, kLinkLostSettleTicks, 0),
              SettleStep::ProceedUnconfirmed);
}

TEST(FinalizePolicy, FailedSaveKeepsThirtySeconds) {
    EXPECT_EQ(settleStep(false, true, true, kLinkLostSettleTicks - 1, 0),
              SettleStep::Wait);
    EXPECT_EQ(settleStep(false, true, true, kLinkLostSettleTicks, 0),
              SettleStep::ProceedUnconfirmed);
}

TEST(FinalizePolicy, NoAbortOfferWhileTheRobotSaves) {
    // The settle loop passes save_done || saving.
    EXPECT_FALSE(shouldOfferAbort(false || saveInFlight(false, "pending"),
                                  kAbortOfferTicks + 10, false));
}
