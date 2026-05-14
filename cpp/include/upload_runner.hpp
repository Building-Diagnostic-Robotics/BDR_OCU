/**
 * @file upload_runner.hpp
 * @brief OCU-side driver for the robot-resident `uploader.py` script.
 *
 * The OCU never touches AWS directly — uploads are a robot-side workflow
 * (presigned URL + single PUT to S3) that the OCU initiates and observes
 * over SSH. This file declares two collaborating helpers:
 *
 *   - `UploadStateProbe` — one-shot SSH `find` walk over /R_DATA on the
 *     robot to enumerate every section/mission folder and classify its
 *     upload state (`None`, `Partial`, `Done`) by checking for
 *     `manifest.json` and `upload_state.json` sentinels.  Also returns
 *     total file count and total byte size so the dialog can show
 *     "X of Y files" + a "Selected: 1.2 GB" summary without an extra
 *     round-trip.
 *
 *   - `UploadRunner` — long-running `QProcess` that streams
 *     `python3 -u uploader.py <data_root> <robot_id> <run_id>` over
 *     SSH and emits Qt signals as the script's stdout lines arrive.
 *     Sequentially walks a queue of `RunTarget`s (one per
 *     section/mission); the script's `UPLOAD_WORKERS=12` provides the
 *     parallelism *within* a section.
 *
 * Auth + endpoint config are pushed through the `env(1)` prefix on the
 * remote command so per-robot creds from `RobotRegistry` flow without
 * editing `uploader.py` per laptop. See `cpp/src/upload_runner.cpp`
 * for the exact remote command shape.
 */

#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QtGlobal>

class QTimer;

namespace f2c_cpp {

/**
 * Single-section upload status as derived from on-robot sentinels.
 *
 * State definitions (match `uploader.py` runtime contract):
 *  - `None`    — neither `upload_state.json` nor `manifest.json` exist.
 *  - `Partial` — `upload_state.json` exists; `manifest.json` does not.
 *                Resume re-uses the on-disk completed set.
 *  - `Done`    — `manifest.json` exists. Re-runs are a no-op (script
 *                short-circuits with "Upload complete: noop").
 */
enum class UploadStatus {
    None = 0,
    Partial,
    Done,
};

/**
 * Robot-side enumeration of one section or mission folder under /R_DATA.
 *
 * `run_id` is the slash-separated relative path from /R_DATA, exactly
 * what `uploader.py` expects on the command line. The dialog passes
 * this through unchanged so the S3 layout
 * `<client_id>/<robot_id>/<run_id>/<relpath>` mirrors on-disk reality
 * 1:1.
 */
struct UploadTarget {
    QString date_folder;     // e.g. "January_27_2026"
    QString building_slug;   // e.g. "Acme_HQ"
    QString section_name;    // e.g. "Section_1_093045" or "Mission_093020"
    QString run_id;          // e.g. "January_27_2026/Acme_HQ/Section_1_093045"
    QString remote_path;     // absolute path on robot, e.g. "/R_DATA/January_27_2026/..."
    UploadStatus status = UploadStatus::None;
    int completed_files = 0;
    int total_files = 0;
    qint64 total_bytes = 0;

    bool isMission() const { return section_name.startsWith(QStringLiteral("Mission_")); }
};

// ---------------------------------------------------------------------------
// SSH state probe
// ---------------------------------------------------------------------------

/**
 * One-shot SSH probe that walks /R_DATA on the robot and emits a
 * `targetsReady` signal with every section/mission found.
 *
 * Lifecycle: parented; safe to delete while the SSH subprocess is in
 * flight (the destructor kills the process and skips the signal).
 */
class UploadStateProbe : public QObject {
    Q_OBJECT
public:
    explicit UploadStateProbe(QObject* parent = nullptr);
    ~UploadStateProbe() override;

    /// Robot SSH connection. Both must be non-empty before `start()`.
    void setRemote(const QString& host, const QString& ssh_user);

    /// Robot data root. Defaults to `/R_DATA`.
    void setDataRoot(const QString& data_root);

    /// Spawn the SSH process. Idempotent — a second call while a probe
    /// is already running is a no-op.
    void start();

    /// True iff the SSH subprocess is alive.
    bool isRunning() const;

signals:
    /// Emitted exactly once per `start()` invocation, on the GUI thread.
    /// `ok` is true when the SSH probe returned rc=0; failure surfaces
    /// `error` (stderr or pre-flight reason) so the dialog can render
    /// an inline message.
    void targetsReady(bool ok, const QList<UploadTarget>& targets,
                      const QString& error);

private slots:
    void onProcessFinished(int exit_code, QProcess::ExitStatus status);
    void onProcessError(QProcess::ProcessError error);

private:
    QString remote_host_;
    QString ssh_user_;
    QString data_root_ = QStringLiteral("/R_DATA");
    QPointer<QProcess> proc_;
    QByteArray stdout_buf_;
    QByteArray stderr_buf_;
    QString last_error_;
};

// ---------------------------------------------------------------------------
// Sequential upload runner
// ---------------------------------------------------------------------------

/**
 * Drives `uploader.py` over SSH for a queue of `UploadTarget`s.
 *
 * Sequential by design: the script's internal `UPLOAD_WORKERS=12`
 * parallelizes within a section, but we run sections back-to-back so
 * the dialog always shows a single active SSH stream + a single
 * progress story. Concurrent section uploads would force operators
 * to mentally interleave two progress logs and add SSH session churn.
 *
 * Pause / cancel:
 *  - `requestPause()` SSH-touches `<remote_path>/pause.flag` on the
 *    active target; the running script finishes its current files
 *    then exits cleanly. Resume = call `start()` again with the same
 *    queue (the script re-reads `upload_state.json` and skips
 *    completed files).
 *  - `requestCancel()` SIGTERMs the SSH process. Files in-flight may
 *    or may not finish; whichever finished are recorded in
 *    `upload_state.json` so the next run picks up cleanly.
 *  - Closing the dialog routes through `requestPause()` (graceful) by
 *    convention — see `UploadDialog::closeEvent`.
 */
class UploadRunner : public QObject {
    Q_OBJECT
public:
    explicit UploadRunner(QObject* parent = nullptr);
    ~UploadRunner() override;

    /// Robot SSH connection.
    void setRemote(const QString& host, const QString& ssh_user);

    /// Cloud auth pushed via `env(1)` on the remote command.
    void setCloudAuth(const QString& api_base,
                      const QString& client_id,
                      const QString& device_token);

    /// Identifier sent as the `<robot_id>` arg to `uploader.py` (and
    /// therefore embedded in S3 keys + manifest.json).
    void setRobotId(const QString& robot_id);

    /// Absolute path of the installed `uploader.py` on the robot. Falls
    /// back to the default location under the SSH user's home; override
    /// only for dev installs in non-standard prefixes.
    void setRemoteScriptPath(const QString& path);
    QString remoteScriptPath() const { return remote_script_path_; }

    /// Set the queue. Replaces any prior queue. No-op if the runner is
    /// currently busy — call `requestCancel()` first.
    void setQueue(const QList<UploadTarget>& targets);

    /// Start (or resume after pause) processing the queue from the
    /// current head. Idempotent.
    void start();

    /// Graceful pause for the *currently running* target (does nothing
    /// when idle). The active script finishes the in-flight file then
    /// exits.
    void requestPause();

    /// Toggle the auto-retry-on-connection-error policy.  Default true.
    /// The OCU-side reachability gate sets this to false on offline
    /// transitions so the runner won't burn through its retry budget
    /// while the network is known-bad — operator manually presses
    /// Upload again once reachability returns.
    void setRetryEnabled(bool enabled) { retry_enabled_ = enabled; }
    bool retryEnabled() const { return retry_enabled_; }

    /// Hard cancel: SIGTERM the SSH process and clear the queue.
    void requestCancel();

    /// True iff the script is mid-upload OR queue still has items OR a
    /// retry is scheduled and pending.
    bool isBusy() const { return busy_; }

    /// True iff a retry timer is currently armed waiting to re-launch
    /// the active target after a transient connection error.
    bool isRetryScheduled() const { return retry_pending_; }

    /// Snapshot of the queue head.
    UploadTarget activeTarget() const;

    // ----- Retry policy (transient connection errors) -----
    //
    // The robot-side script auto-pauses on `requests.exceptions.
    // RequestException` and exits rc=0 (so the laptop side never sees
    // a hard error for a Microhard fade).  We retry the same target
    // up to `kMaxConnectionRetries` times with exponential backoff;
    // each retry re-launches the SSH command identically — the script
    // re-reads `upload_state.json` and skips the files it already
    // landed, so retries are idempotent and never duplicate uploads.
    //
    // Backoff sequence (attempt index → wait): 1 → 5 s, 2 → 15 s,
    // 3 → 45 s. After exhaustion the runner emits `targetFailed`
    // exactly as the non-retried hard-error path does.
    static constexpr int kMaxConnectionRetries = 3;
    static int retryBackoffMs(int attempt);

signals:
    /// Queue progress. `index` is 0-based; `total` is the queue length
    /// captured at `start()`. Fires at the start of each new target,
    /// including each automatic retry of that target.
    void targetStarted(int index, int total, const UploadTarget& target);

    /// Fired whenever a transient `Connection error` triggers an
    /// automatic retry. `attempt` is the *upcoming* attempt number
    /// (1-based, never exceeds `kMaxConnectionRetries`); `wait_ms`
    /// is the backoff delay before the relaunch fires. The dialog
    /// uses these to render a "Retrying in 12 s (2 of 3)…" caption
    /// without scraping log lines.
    void targetRetryScheduled(const UploadTarget& target,
                              int attempt, int wait_ms,
                              const QString& reason);

    /// Per-file events parsed from stdout. `relpath` is the section-
    /// relative path. `Skipped` fires for files already completed in
    /// `upload_state.json`.
    void fileUploaded(const UploadTarget& target, const QString& relpath);
    void fileSkipped(const UploadTarget& target, const QString& relpath);

    /// Free-form log line forwarded from stdout (e.g. "Generating
    /// manifest...", "State cleaned up."). The dialog appends these
    /// to a small log strip so operators see what the script is doing.
    void logLine(const QString& line);

    /// Raised when the script auto-paused due to connection loss OR
    /// the operator's explicit pause flag. The dialog drops back to
    /// the idle button row but keeps the section row tagged "paused".
    void targetPaused(const UploadTarget& target, const QString& reason);

    /// Hard error from the script (5GB ceiling, hash failure, etc.).
    /// The runner stops the queue so the operator sees the message
    /// before further work continues.
    void targetFailed(const UploadTarget& target, const QString& error);

    /// Per-target success.
    void targetCompleted(const UploadTarget& target);

    /// Queue finished (all targets dispatched, success or otherwise).
    /// `cancelled` reflects whether `requestCancel()` interrupted the
    /// run. The dialog uses this to flip back to the idle UI.
    void queueFinished(bool cancelled);

private slots:
    void onStdoutReady();
    void onStderrReady();
    void onProcessFinished(int exit_code, QProcess::ExitStatus status);
    void onProcessError(QProcess::ProcessError error);

private:
    void launchNext();
    void launchActiveTarget();   // common path between fresh-start and retry
    void processStdoutLine(const QString& raw_line);
    QStringList sshBaseArgs() const;
    QString buildRemoteCommand(const UploadTarget& target) const;
    void resetQueueState();
    void cancelPendingRetry();

    // SSH config.
    QString remote_host_;
    QString ssh_user_;
    QString remote_script_path_;

    // Cloud config (pushed via env(1) prefix on the remote command).
    QString cloud_api_base_;
    QString cloud_client_id_;
    QString cloud_device_token_;
    QString robot_id_;

    // Queue state.
    QList<UploadTarget> queue_;
    int current_index_ = -1;
    int queue_total_ = 0;
    bool busy_ = false;
    bool cancel_requested_ = false;
    bool paused_during_target_ = false;
    QString pause_reason_;
    QString fatal_error_;

    QPointer<QProcess> proc_;
    QString stdout_carry_;  // partial line buffer between readyRead chunks.

    // Per-target retry counter — bumped each time we re-launch the
    // current_index_ target after a transient connection error.
    // Reset whenever current_index_ advances.
    int retry_attempts_done_ = 0;
    bool retry_pending_ = false;
    bool retry_enabled_ = true;
    QTimer* retry_timer_ = nullptr;
};

}  // namespace f2c_cpp
