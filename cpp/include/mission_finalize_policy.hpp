/**
 * @file mission_finalize_policy.hpp
 * @brief Pure Complete Mission settle decisions — no Qt, no ROS.
 *
 * The Stage 6 settle loop in satellite_screen.cpp owns the timers and
 * RPCs. These two predicates are the only policy that loop applies, so
 * they live here and the tests compile them without a QWidget.
 */

#pragma once

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

}  // namespace finalize_policy
}  // namespace f2c_cpp
