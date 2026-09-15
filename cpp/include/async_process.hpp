/**
 * @file async_process.hpp
 * @brief Non-blocking QProcess helpers — the GUI thread must never wait.
 *
 * Launches and sweeps used to be `start()` followed by `waitForFinished()`,
 * which freezes the window for as long as the remote takes (the Stage 6
 * teardown could hold it for ~40 s). These two helpers are the replacement:
 * the deadline and the SIGTERM -> SIGKILL escalation are driven by timers,
 * and the callback lands on the GUI thread exactly once, always.
 */

#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <functional>

namespace f2c_cpp {
namespace async_proc {

/** Merged stdout+stderr of the run, trimmed. */
using Finished = std::function<void(int exit_code, const QString& output)>;

/**
 * Spawns `program args` as a child of `parent` and returns it.
 *
 * `on_done` runs exactly once: on exit, on a spawn failure (exit code -1),
 * or when `timeout_ms` expires — SIGTERM, then SIGKILL, then a final
 * backstop, the same escalation the blocking code used. The process deletes
 * itself afterwards, so do not hold the returned pointer past the callback.
 */
QProcess* run(QObject* parent, const QString& program, const QStringList& args,
              int timeout_ms, Finished on_done);

/**
 * Waits for an already-running `proc` to exit without blocking.
 *
 * SIGKILLs it once `grace_ms` passes (callers are expected to have asked
 * nicely first) and calls `on_done` exactly once — on the next event-loop
 * turn if the process is already gone. Only the connection this makes is
 * dropped afterwards, so a long-lived process keeps its other subscribers.
 */
void reap(QProcess* proc, int grace_ms, std::function<void()> on_done);

}  // namespace async_proc
}  // namespace f2c_cpp
