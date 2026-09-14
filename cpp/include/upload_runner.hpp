/**
 * @file upload_runner.hpp
 * @brief OCU-side driver for `uploader.py` (presigned URL + single PUT).
 *
 * The OCU never holds AWS credentials — `uploader.py` asks the BDR
 * backend for a presigned URL per file and PUTs straight to S3. This
 * file declares two collaborating helpers, each of which works against
 * one of two data sources (`UploadSource`):
 *
 *   - `ThumbDrive` (production). The robot's offload worker copies every
 *     finalized mission onto the `RDATA_EXT` stick, mirroring
 *     `/R_DATA/<date>/<building>/{Mission_*,Section_*}`. The operator
 *     plugs the stick into the laptop; the probe walks it with
 *     `QDirIterator` off the GUI thread and the runner launches
 *     `python3 -u uploader.py` locally against the mounted folder.
 *     Upload state (`upload_state.json`, `manifest.json`) is written
 *     next to the data on the stick, so resume survives a laptop swap.
 *
 *   - `RobotSsh` (legacy, kept inactive). Same two helpers driven over
 *     SSH against the robot's `/R_DATA`: a `find` probe and a remote
 *     `env … python3 -u uploader.py` stream. Not reachable from the UI;
 *     retained so the fleet can fall back without a rebuild.
 *
 *   - `UploadStateProbe` — one-shot enumeration of every section/mission
 *     folder at depth 3 under the data root, classified as `None` /
 *     `Partial` / `Done` from the `upload_state.json` / `manifest.json`
 *     sentinels, plus file count and byte size for the dialog summary.
 *
 *   - `UploadRunner` — long-running `QProcess` that streams the script's
 *     stdout and emits Qt signals per line. Sequentially walks a queue of
 *     `UploadTarget`s (one per section/mission); the script's
 *     `UPLOAD_WORKERS=12` provides the parallelism *within* a section.
 *
 * Auth + endpoint config reach the script through its environment
 * (`QProcessEnvironment` locally, an `env(1)` prefix over SSH) so per-robot
 * creds from `RobotRegistry` flow without editing `uploader.py`.
 */

#pragma once

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QtGlobal>

class QTimer;

namespace f2c_cpp {

/// Where the section folders live. See the file comment.
enum class UploadSource {
    ThumbDrive = 0,   ///< Local mount of the RDATA_EXT stick (production).
    RobotSsh,         ///< Robot `/R_DATA` over SSH (legacy, inactive).
};

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
 * One section or mission folder at depth 3 under the data root.
 *
 * `run_id` is the slash-separated relative path from the data root,
 * exactly what `uploader.py` expects on the command line. The dialog
 * passes this through unchanged so the S3 layout
 * `<client_id>/<robot_id>/<run_id>/<relpath>` mirrors on-disk reality
 * 1:1 regardless of where the root is mounted.
 */
struct UploadTarget {
    QString date_folder;     // e.g. "January_27_2026"
    QString building_slug;   // e.g. "Acme_HQ"
    QString section_name;    // e.g. "Section_1_093045" or "Mission_093020"
    QString run_id;          // e.g. "January_27_2026/Acme_HQ/Section_1_093045"
    QString data_path;       // absolute folder path on the source: the stick
                             // mount ("/media/u/RDATA_EXT/January_27_2026/...")
                             // or the robot ("/R_DATA/January_27_2026/...")
    QString building_name;   // operator-typed site name from session/mission json
    QString operator_name;
    QDateTime captured_at;
    UploadStatus status = UploadStatus::None;
    int completed_files = 0;
    int total_files = 0;
    qint64 total_bytes = 0;

    bool isMission() const { return section_name.startsWith(QStringLiteral("Mission_")); }
};

// ---------------------------------------------------------------------------
// State probe
// ---------------------------------------------------------------------------

/**
 * One-shot probe that walks the data root and emits `targetsReady` with
 * every section/mission found.
 *
 * `ThumbDrive`: a `QDirIterator` walk on a worker thread (a full stick
 * can hold thousands of files; stat-ing them must not stall the GUI).
 * `RobotSsh`: an SSH `find` script on the robot.
 *
 * Lifecycle: parented; safe to delete while a probe is in flight (the
 * destructor kills the SSH process / detaches the worker and skips the
 * signal).
 */
class UploadStateProbe : public QObject {
    Q_OBJECT
public:
    explicit UploadStateProbe(QObject* parent = nullptr);
    ~UploadStateProbe() override;

    void setSource(UploadSource source) { source_ = source; }
    UploadSource source() const { return source_; }

    /// Robot SSH connection (RobotSsh only). Both must be non-empty
    /// before `start()`.
    void setRemote(const QString& host, const QString& ssh_user);

    /// Data root: the stick's mount point (ThumbDrive) or the robot's
    /// `/R_DATA` (RobotSsh, the default).
    void setDataRoot(const QString& data_root);
    QString dataRoot() const { return data_root_; }

    /// Kick the probe. Idempotent — a second call while a probe is
    /// already running is a no-op.
    void start();

    /// True iff a probe is in flight.
    bool isRunning() const;

    /// Synchronous local walk — the ThumbDrive engine, exposed for
    /// tests. `data_root` must be the mount point (or any folder laid
    /// out `<date>/<building>/<section>`).
    static QList<UploadTarget> scanLocalDataRoot(const QString& data_root);

signals:
    /// Emitted exactly once per `start()` invocation, on the GUI thread.
    /// `ok` is false when the walk could not run at all (unreachable
    /// robot, missing mount); `error` carries the reason for the inline
    /// message.
    void targetsReady(bool ok, const QList<UploadTarget>& targets,
                      const QString& error);

private slots:
    void onProcessFinished(int exit_code, QProcess::ExitStatus status);
    void onProcessError(QProcess::ProcessError error);

private:
    void startLocal();
    void startSsh();

    UploadSource source_ = UploadSource::ThumbDrive;
    QString remote_host_;
    QString ssh_user_;
    QString data_root_ = QStringLiteral("/R_DATA");
    QPointer<QProcess> proc_;
    QByteArray stdout_buf_;
    QByteArray stderr_buf_;
    QString last_error_;

    // ThumbDrive: generation counter so a walk started before the root
    // changed (stick swapped mid-scan) is discarded when it lands.
    int local_generation_ = 0;
    bool local_running_ = false;
};

// ---------------------------------------------------------------------------
// Sequential upload runner
// ---------------------------------------------------------------------------

/**
 * Drives `uploader.py` for a queue of `UploadTarget`s.
 *
 * Sequential by design: the script's internal `UPLOAD_WORKERS=12`
 * parallelizes within a section, but we run sections back-to-back so
 * the dialog always shows a single active stream + a single progress
 * story. Concurrent section uploads would force operators to mentally
 * interleave two progress logs.
 *
 * Pause / cancel:
 *  - `requestPause()` touches `<data_path>/pause.flag` on the active
 *    target (locally, or via SSH in RobotSsh); the running script
 *    finishes its in-flight files then exits cleanly. Resume = call
 *    `start()` again with the same queue (the script re-reads
 *    `upload_state.json` and skips completed files). The runner clears
 *    a stale `pause.flag` before every launch — the script never
 *    removes it itself.
 *  - `requestCancel()` SIGTERMs the process. Files in-flight may or may
 *    not finish; whichever finished are recorded in `upload_state.json`
 *    so the next run picks up cleanly.
 *  - Closing the dialog routes through `requestPause()` (graceful) by
 *    convention — see `UploadDialog::closeEvent`.
 */
class UploadRunner : public QObject {
    Q_OBJECT
public:
    explicit UploadRunner(QObject* parent = nullptr);
    ~UploadRunner() override;

    void setSource(UploadSource source);
    UploadSource source() const { return source_; }

    /// Robot SSH connection (RobotSsh only).
    void setRemote(const QString& host, const QString& ssh_user);

    /// Cloud auth handed to the script through its environment.
    void setCloudAuth(const QString& api_base,
                      const QString& client_id,
                      const QString& device_token);

    /// Identifier sent as the `<robot_id>` arg to `uploader.py` (and
    /// therefore embedded in S3 keys + manifest.json).
    void setRobotId(const QString& robot_id);

    /// RobotSsh: absolute path of the installed `uploader.py` on the
    /// robot. Falls back to the default location under the SSH user's
    /// home; override only for dev installs in non-standard prefixes.
    void setRemoteScriptPath(const QString& path);
    QString remoteScriptPath() const { return remote_script_path_; }

    /// ThumbDrive: absolute path of `uploader.py` on this laptop. Empty
    /// (default) resolves through `resolveLocalScriptPath()`.
    void setLocalScriptPath(const QString& path);

    /// The script the ThumbDrive launch will use: the explicit override,
    /// else the .deb install location, else the source tree relative to
    /// the running binary (dev builds). Empty when none exists — the
    /// dialog surfaces that before the operator selects anything.
    static QString resolveLocalScriptPath(const QString& override_path = QString());

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

    /// Hard cancel: SIGTERM the uploader process and clear the queue.
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
    // The script auto-pauses on `requests.exceptions.RequestException`
    // and exits rc=0 (so the OCU never sees a hard error for a wifi
    // blip).  We retry the same target
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
    void configureLocalProcess(QProcess* proc, const UploadTarget& target);
    void configureSshProcess(QProcess* proc, const UploadTarget& target);
    void processStdoutLine(const QString& raw_line);
    QStringList sshBaseArgs() const;
    QString buildRemoteCommand(const UploadTarget& target) const;
    void clearPauseFlag(const UploadTarget& target);
    void resetQueueState();
    void cancelPendingRetry();

    UploadSource source_ = UploadSource::ThumbDrive;

    // SSH config (RobotSsh).
    QString remote_host_;
    QString ssh_user_;
    QString remote_script_path_;

    // Local script (ThumbDrive).
    QString local_script_path_;

    // Cloud config (script environment).
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
