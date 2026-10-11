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
