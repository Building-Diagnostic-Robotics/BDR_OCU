/**
 * @file stop_prompt_policy.hpp
 * @brief Pure Stage 6 stop-modal dwell — no Qt, no ROS.
 *
 * maybePromptStop in satellite_screen.cpp owns the samples and the
 * dialog. These two predicates are the only policy that path applies,
 * so they live here and the tests compile them without a QWidget.
 */

#pragma once

namespace f2c_cpp {
namespace stop_prompt_policy {

/** /coverage/status is 1 Hz, and the executor latches a stop reason for
    as little as one 20 Hz tick — an end-of-sweep `degenerate_path` clears
    itself before the next sample. Three consecutive stopped samples is the
    line between a blip and a robot that needs the operator. */
constexpr int kStopDwellSamples = 3;

/** Counts consecutive stopped samples, NOT repeats of one reason: a robot
    failing to replan cycles blocked_* -> route_invalid -> replan_* across
    samples, and keying the dwell to a matching reason meant a genuinely
    stuck robot could reset the count forever and never prompt. Any healthy
    sample zeroes it, which is what discards the blips. */
inline int advanceStopDwell(int samples, bool stop_state) {
    return stop_state ? samples + 1 : 0;
}

inline bool stopPromptDue(int samples, bool already_explained,
                          bool prompt_open) {
    return !already_explained && !prompt_open &&
           samples >= kStopDwellSamples;
}

}  // namespace stop_prompt_policy
}  // namespace f2c_cpp
