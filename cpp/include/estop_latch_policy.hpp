/**
 * @file estop_latch_policy.hpp
 * @brief Pure Stage 6 scan run-state / E-Stop latch — no Qt, no ROS.
 *
 * The run state doubles as the E-Stop latch. Before this existed, onEstop()
 * dropped autonomy and requested axis IDLE but left the state Running, so
 * the manual-override release path read "was running" and called
 * beginStartScan() — which re-armed CLOSED_LOOP and republished
 * autonomy_enable=true. On 2026-09-16 that fired three times in one run,
 * next to a roof edge, after the operator had pressed E-Stop.
 *
 * Modelling the stop as its own state is what fixes it: every gate that
 * tests for Running is false while latched, so no existing path can resume
 * a stopped robot. satellite_screen.cpp aliases ScanRunState to RunState,
 * so the two can never drift.
 */

#pragma once

namespace f2c_cpp {
namespace estop_latch_policy {

enum class RunState { Idle, Running, Paused, EmergencyStopped, Completed };

/** E-Stop wins from any state, including Completed: the axes are being
    dropped to IDLE either way, and the latch is what stops a later click
    from putting them back. */
constexpr RunState onEmergencyStop(RunState) noexcept {
    return RunState::EmergencyStopped;
}

/** Clearing the latch does NOT arm. It lands on Paused so the operator has
    to press Resume as a second, deliberate action — one reflexive click on
    the primary button must not spin the wheels back up. */
constexpr RunState clearEmergencyStop(RunState state) noexcept {
    return state == RunState::EmergencyStopped ? RunState::Paused : state;
}

/** Taking manual control, or disarming, pauses an autonomous run and is
    inert otherwise. In particular it must not lift the latch, which is why
    this tests for Running rather than assigning Paused outright. */
constexpr RunState pauseIfRunning(RunState state) noexcept {
    return state == RunState::Running ? RunState::Paused : state;
}

/** Handing control back never resumes autonomy, in any state. The operator
    presses Resume. This is a function rather than a deleted branch so the
    contract is visible at the call site and covered by a test. */
constexpr bool releaseResumesAutonomy(RunState) noexcept { return false; }

/** Consulted by beginStartScan() and every other path that requests
    CLOSED_LOOP with autonomy. Arm-for-teleop deliberately does not go
    through this — it never enables autonomy. */
constexpr bool armingAllowed(RunState state) noexcept {
    return state != RunState::EmergencyStopped;
}

/** While latched, the director's own state must not overwrite the E-STOP
    pill. /coverage/status is 1 Hz, so without this the operator's only
    confirmation that the stop is holding disappears within a second. */
constexpr bool holdsStatusPill(RunState state) noexcept {
    return state == RunState::EmergencyStopped;
}

}  // namespace estop_latch_policy
}  // namespace f2c_cpp
