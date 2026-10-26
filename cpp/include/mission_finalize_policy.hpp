/**
 * @file mission_finalize_policy.hpp
 * @brief Pure Complete Mission settle decisions — no Qt, no ROS.
 *
 * The Stage 6 settle loop and SSH-offline path in satellite_screen.cpp
 * own the timers, RPCs and processes. These predicates are the only
 * policy they apply, so they live here and the tests compile them
 * without a QWidget.
 */

#pragma once

#include <cctype>
#include <string>

namespace f2c_cpp {
namespace finalize_policy {

/** 250 ms × 40 = 10 s. Conclude can refuse while work is still
    active; abort-and-save is the door that still reaches disk. */
constexpr int kAbortOfferTicks = 40;

/** Offer abort-and-save once the wait has aged and the save is not
    already done. A consumed click stays dead for the rest of the settle. */
inline bool shouldOfferAbort(bool save_done, int ticks, bool consumed) {
    return !save_done && !consumed && ticks >= kAbortOfferTicks;
}

/** A fresh /coverage/status is at most this old. */
constexpr int kStatusFreshMs = 3000;

/** 250 ms × 120 = 30 s without a fresh status: the link is gone and no
    confirmation can arrive, so waiting longer only strands the operator. */
constexpr int kLinkLostSettleTicks = 120;

/** 250 ms × 2400 = 10 min. The ceiling while the robot is still reporting
    and still saving. A 77-minute scan's map took longer than the old 0.4 s
    the OCU allowed (Ansys 2645 Zanker Rd, 2026-10-14: the stack was
    stopped mid-save and the scan never reached the stick). */
constexpr int kSaveSettleCeilingTicks = 2400;

/** After this many ticks of saving, tell the operator why it is taking a
    while. */
constexpr int kSlowSaveNoticeTicks = 20;

/** The robot is still closing the section: bag, map, rename, finalize.
    `saving` is the director's own flag (new robots); `pending` is the copy
    state it sets the moment /dc/end_and_save is issued (all robots). */
inline bool saveInFlight(bool saving, const std::string& copy) {
    return saving || copy == "pending";
}

/** The save is accounted for: the section is closed and the copy has been
    handed to the offload worker (or there is no copy to make). Only now may
    the robot stack be stopped. A robot still reporting `saving` is never
    settled, even if `complete` latched early (copying disabled). */
inline bool saveSettled(bool saving, bool status_complete,
                        const std::string& copy) {
    if (saving) {
        return false;
    }
    return status_complete || copy == "queued" ||
           copy == "waiting_for_drive" || copy == "copying" ||
           copy == "done" || copy == "skipped" || copy == "failed" ||
           copy == "stick_full";
}

/** The director's error string when /dc/end_and_save failed. */
inline bool saveReportedFailed(const std::string& error) {
    return error.find("NOT saved") != std::string::npos;
}

enum class SettleStep {
    Wait,                // keep the stack up; the robot is still saving
    Proceed,             // save accounted for; stop the robot
    ProceedUnconfirmed,  // give up waiting; the save was not confirmed
    LeaveRobotRunning,   // link lost mid-save: do not stop the robot
};

/** One tick of the Complete Mission settle loop. `ticks` counts every tick
    of this settle; `stale_ticks` counts consecutive ticks without a fresh
    status. While the robot reports a save in flight it is never stopped
    before the 10 min ceiling. If the link drops while a save is in flight,
    the stack is left running: the coordinator finishes on its own and the
    watchdog closes the mission. With no save in flight (conclude hung or
    refused) the old 30 s settle stands. A save error only shortens the
    wait when no save is in flight (a retry clears it on the robot). */
inline SettleStep settleStep(bool settled, bool saving, bool save_failed,
                             int ticks, int stale_ticks) {
    if (settled) {
        return SettleStep::Proceed;
    }
    if (stale_ticks >= kLinkLostSettleTicks) {
        return saving ? SettleStep::LeaveRobotRunning
                      : SettleStep::ProceedUnconfirmed;
    }
    if (ticks >= kSaveSettleCeilingTicks) {
        return SettleStep::ProceedUnconfirmed;
    }
    if ((!saving || save_failed) && ticks >= kLinkLostSettleTicks) {
        return SettleStep::ProceedUnconfirmed;
    }
    return SettleStep::Wait;
}

/** COMPLETED means data is on disk from a full sweep. An abort-and-save
    leaves the plan PLANNED so the partial sweep can be re-run. */
inline bool shouldStampCompleted(bool save_done, bool status_complete,
                                 bool abort_save_used) {
    return (save_done || status_complete) && !abort_save_used;
}

/** finalize_mission_local.py exits 0 for "no open mission found" as well
    as for a real finalize, so rc alone cannot stamp COMPLETED: a launch
    that never reached the robot would mark a plan done with nothing on
    disk. Only `"finalized": true` in its JSON line proves the write. */
inline bool offlineFinalizeLanded(int exit_code, const std::string& output) {
    if (exit_code != 0) {
        return false;
    }
    static const std::string kKey = "\"finalized\"";
    std::string::size_type pos = 0;
    while ((pos = output.find(kKey, pos)) != std::string::npos) {
        std::string::size_type i = pos + kKey.size();
        while (i < output.size() &&
               std::isspace(static_cast<unsigned char>(output[i]))) {
            ++i;
        }
        if (i < output.size() && output[i] == ':') {
            ++i;
            while (i < output.size() &&
                   std::isspace(static_cast<unsigned char>(output[i]))) {
                ++i;
            }
            if (output.compare(i, 4, "true") == 0) {
                return true;
            }
        }
        pos += kKey.size();
    }
    return false;
}

}  // namespace finalize_policy
}  // namespace f2c_cpp
