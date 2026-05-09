/**
 * @file update_lockfile.hpp
 * @brief Cross-process update-runner lockfile (concern #2 / #3).
 *
 * Coordinates two related conditions:
 *   1. A single bdr-update-runner instance is running at any time. If a
 *      stray relaunch (operator double-clicks the desktop icon while the
 *      runner is mid-install) tried to spawn a second runner, both would
 *      race for dpkg and one would corrupt the other.
 *   2. The OCU's "spawn runner, wait for it to be ready, then quit" handoff
 *      uses the SAME lockfile as a ready signal. The runner takes the lock
 *      on startup; the OCU polls until the file exists AND is locked,
 *      proving the runner is up.
 *
 * Implementation: flock(2) LOCK_EX | LOCK_NB on a file at
 * <CacheLocation>/update_runner.lock. flock locks are owned by the file
 * descriptor and released on close() OR on process exit (kernel cleanup),
 * which means a runner crash never leaves a stale lock the next runner
 * can't acquire.
 *
 * RAII: hold a Lockfile object for the lifetime of the runner; lock is
 * released in the dtor (which closes the fd, which releases flock).
 *
 * Sharing: this header lives in the bdr_update_core static lib so both
 * binaries link the same implementation and agree on the path.
 */

#pragma once

#include <QString>

namespace f2c_cpp::update {

/**
 * @brief Returns the absolute path of the runner lockfile, e.g.
 *        ~/.cache/PilotControl/BDRCoveragePlanner/update_runner.lock.
 *        Path resolves to the same string in both OCU and runner provided
 *        each process has set QApplication::organizationName/applicationName
 *        to the project's settings constants. Empty if cache dir is
 *        unwritable.
 */
QString runnerLockfilePath();

/**
 * @brief RAII handle around an exclusive non-blocking flock on the runner
 *        lockfile. Constructed unowned; call tryAcquire() to take the lock.
 *
 * Usage (runner):
 *   Lockfile lf;
 *   if (!lf.tryAcquire()) {
 *       // another runner is already running — refuse to start.
 *       return 1;
 *   }
 *   // ... runner main loop ...
 *   // dtor releases the lock on process exit.
 *
 * Usage (OCU): does not own the lock; checks for it via
 * isRunnerLockfileHeld() instead.
 */
class Lockfile {
public:
    Lockfile() = default;
    ~Lockfile();

    Lockfile(const Lockfile&) = delete;
    Lockfile& operator=(const Lockfile&) = delete;

    /**
     * @brief Attempt to acquire an exclusive non-blocking flock.
     * @param path  Absolute path; if empty, runnerLockfilePath() is used.
     * @return true on success. false if another process holds the lock or
     *         if the file cannot be opened/created.
     *
     * Side effect on success: the file is overwritten with the calling
     * process's PID + a newline so postmortems (and the OCU's wait loop)
     * can see who owns it.
     */
    bool tryAcquire(const QString& path = QString());

    /// Releases the flock and closes the fd. Idempotent.
    void release();

    bool held() const { return fd_ >= 0; }
    QString path() const { return path_; }

private:
    int fd_ = -1;
    QString path_;
};

/**
 * @brief Probe whether the runner lockfile is currently held by some
 *        process. Used by the OCU's spawn-then-wait handoff and as a
 *        startup safety check.
 *
 * Implementation: tries flock(LOCK_EX | LOCK_NB) on the file; if it fails
 * with EWOULDBLOCK, the lock is held. The probe never holds the lock
 * itself — it acquires and immediately releases on success.
 *
 * @return true if lockfile exists AND another process holds an exclusive
 *         lock. false if the file does not exist, cannot be opened, or
 *         is unlocked.
 */
bool isRunnerLockfileHeld();

}  // namespace f2c_cpp::update
