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
#include <QHash>
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
 *  - `Partial` — `upload_state.json` exists, whether or not a manifest
 *                was also written. The script re-PUTs a file whose
 *                recorded size no longer matches, and a manifest left
 *                next to the state file was not confirmed.
 *  - `Done`    — `manifest.json` exists and `upload_state.json` does
 *                not. The script no-ops when local sizes still match
 *                `size_bytes`, and re-uploads when they do not.
 */
enum class UploadStatus {
    None = 0,
    Partial,
    Done,
};

/// Result of checking a `Done` section against S3. Separate from
/// `UploadStatus`, which is only what the stick sentinels say.
enum class CloudCheck {
    Unchecked = 0,  ///< Not checked, or the check could not run.
    Checking,       ///< `/verify` is in flight.
    Verified,       ///< Listing matches the stick manifest.
    Missing,        ///< S3 is missing the section or a file disagrees.
};

/**
 * What the stick holds for a folder, checked against the robot's
 * per-file copy record (`thumbsync_manifest.json` schema 2) before any
 * upload. Only folders not yet uploaded are checked.
 */
enum class StickCopyProblem {
    None = 0,     ///< Matches the record, or the stick predates records.
    Unfinished,   ///< No record lists the folder: the robot's copy never
                  ///  finished (the record is written last).
    Damaged,      ///< Files missing or the wrong size, or the record
                  ///  itself is unreadable. Plugging the stick into the
                  ///  robot repairs it (`needs_recopy.json`).
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
    /// Cloud check over a `Done` row. `Unchecked` is the on-disk badge
    /// ("Uploaded") and is also where a failed or offline check leaves
    /// the row. Only `Missing` is selectable, and it uploads with
    /// `force_reupload`.
    CloudCheck cloud_check = CloudCheck::Unchecked;
    /// A `Verified` folder whose proof is being re-checked. It keeps its
    /// Verified badge meanwhile; a failed or offline check leaves it
    /// Verified, a mismatch makes it `Missing`.
    bool cloud_recheck = false;
    /// Pass `--force` on the next launch of this target only. The
    /// runner clears it once that launch is built, so a connection
    /// retry or a later Upload press resumes the state file instead of
    /// deleting it.
    bool force_reupload = false;
    int completed_files = 0;
    int total_files = 0;
    qint64 total_bytes = 0;
    /// Thumb drive only. A folder with a problem is never queued; the
    /// row tells the operator to plug the stick into the robot.
    StickCopyProblem copy_problem = StickCopyProblem::None;
    /// Files missing or the wrong size (Damaged only; 0 when the record
    /// itself could not be read).
    int copy_bad_files = 0;

    bool isMission() const { return section_name.startsWith(QStringLiteral("Mission_")); }
};

/// Arguments after `python3` for one `uploader.py` launch. `--force`
/// is appended only when `force` is true.
QStringList uploaderArguments(const QString& script_path,
                              const QString& data_path,
                              const QString& robot_id,
                              const QString& run_id,
                              bool force);

// ---------------------------------------------------------------------------
// One list row per building
// ---------------------------------------------------------------------------

/**
 * Badge for every folder that shares a building slug, across dates.
 * `Uploaded` / `Checking` / `Verified` apply only when every folder is
 * already uploaded. A later scan of the same building stays `Pending`
 * (or `Partial`) until its own folders finish.
 */
enum class UploadRowBadge {
    Pending = 0,
    Partial,
    Uploaded,
    Checking,
    Verified,
    Missing,
    /// Every folder still to upload has a stick copy problem. Not
    /// selectable; the operator plugs the stick into the robot.
    NeedsRobot,
};

/**
 * One upload-dialog row. `run_ids` is the upload order: oldest visit
 * first, and on an equal timestamp the Mission folder before a Section.
 * The runner still launches one folder at a time.
 */
/// One entry in a building's hover card: "Section 2" and when it was
/// scanned ("Oct 13, 8:40 PM"; empty when unknown). Mission folders show as
/// "Other scan files" with no time. Never a folder name.
struct UploadRowItem {
    QString name;
    QString when;
};

/// What the hover card on a building row says. Counts are sections only;
/// the lists hold just what the operator must act on.
struct UploadRowSummary {
    int sections = 0;
    int uploaded = 0;
    int left = 0;         ///< not uploaded yet, or partly
    int need_robot = 0;   ///< copy damaged or unfinished on the drive
    int missing = 0;      ///< uploaded, but gone from the cloud
    QList<UploadRowItem> fix_on_robot;
    QList<UploadRowItem> upload_again;
};

struct UploadBuildingRow {
    QString key;
    QString building_label;
    QString operator_name;
    QDateTime newest;
    QString newest_date_folder;
    QStringList run_ids;
    qint64 total_bytes = 0;
    UploadRowBadge badge = UploadRowBadge::Pending;
    QString badge_text;
    int partial_percent = 0;
    bool selectable = false;
    UploadRowSummary summary;
    /// Plain-text form of `summary` (tests, logs); the dialog renders
    /// `summary` as a formatted card.
    QString tooltip;
};

/// Slug, or a key that cannot collide with a slug when the folder has
/// none. Empty `run_id` still returns a key; callers skip those targets.
QString uploadBuildingKey(const UploadTarget& target);

/// One row per building, newest scan first. Targets with an empty
/// `run_id` are dropped.
/// At most this many sections per list on the hover card, then "… and N more".
constexpr int kUploadRowListCap = 8;

/// Plain-text hover card: the summary line, then each action list.
QString uploadRowTooltipText(const UploadRowSummary& summary);

QList<UploadBuildingRow> groupUploadTargets(
    const QHash<QString, UploadTarget>& targets);

/// Folders a ticked row should hand the runner. Skips folders that are
/// already uploaded and folders whose stick copy has a problem. A folder
/// the cloud check marked missing is included with `force_reupload` set.
QList<UploadTarget> uploadTargetsForRow(const QHash<QString, UploadTarget>& targets,
                                        const UploadBuildingRow& row);

/**
 * Progress of one upload queue, counted per building rather than per
 * folder: the bar covers every folder of the building being uploaded
 * (a building with 10 sections fills once, not 10 times), and starts
 * again for the next building in the queue. Units are files.
 *
 * The queue is grouped by `uploadBuildingKey` in queue order; the
 * dialog queues a building's folders together. A folder's files are
 * `max(total_files, completed_files)`; files already uploaded before
 * this run count as done from the start. A target that starts again
 * (a connection retry) keeps the files it already sent.
 */
class BuildingUploadProgress {
public:
    void reset(const QList<UploadTarget>& queue);
    void targetStarted(const UploadTarget& target);
    void fileDone();
    /// The active folder ended (uploaded, failed, or skipped): its files
    /// count as processed so the building can still reach 100%.
    void targetFinished();

    int done() const { return base_done_ + current_done_; }
    int total() const;
    /// 1-based position of the active building / folder; 0 before start.
    int buildingNumber() const { return group_ + 1; }
    int buildingCount() const { return groups_.size(); }
    int folderNumber() const { return folder_in_group_; }
    int folderCount() const;
    QString buildingLabel() const;

private:
    struct Group {
        QString key;
        QString label;
        int files = 0;
        int folders = 0;
    };
    QList<Group> groups_;
    int group_ = -1;
    int folder_in_group_ = 0;
    int base_done_ = 0;
    int current_done_ = 0;
    int current_total_ = 0;
    QString current_run_id_;
};

/// Every `Done` folder: those without a proof first, then the
/// `CloudCheck::Verified` ones again (re-checked on every listing, so a
/// later deletion in S3 is found while the stick still has the data).
/// Oldest visit first, and on an equal timestamp the Mission folder
/// before a Section. The caller checks them in batches; a folder the
/// probe already marked Verified (a valid `cloud_verified.json`) is
/// left out so a repeat check does not call the cloud again.
QList<UploadTarget> cloudCheckTargets(const QHash<QString, UploadTarget>& targets);

// ---------------------------------------------------------------------------
// cloud_verified.json — the robot's proof a folder is in the cloud
// ---------------------------------------------------------------------------

/**
 * `valid` is the robot's rule (`rdata_retention.folder_cloud_verified`):
 * `manifest.json` exists, `upload_state.json` does not, and
 * `manifest_sha256` is 64 hex characters equal to the sha256 of the
 * manifest file bytes. Anything else is ignored, including a file left
 * behind after the manifest was rewritten.
 */
struct CloudVerifiedProof {
    bool valid = false;
    QString manifest_sha256;
};

/// Lowercase hex sha256 of the file's bytes, or empty if it cannot be read.
QByteArray fileSha256Hex(const QString& path);

CloudVerifiedProof readCloudVerified(const QString& folder);

/// Hash `manifest.json` on disk and write `cloud_verified.json` beside
/// it (tmp file, flush, rename). False when the manifest cannot be read
/// or the rename fails — the caller must not treat the folder as verified.
bool writeCloudVerified(const QString& folder,
                        const QString& client_id,
                        const QString& robot_id,
                        int file_count);

/// Delete `cloud_verified.json` and a leftover `.tmp`. True when both
/// are gone, including when they were never there.
bool removeCloudVerified(const QString& folder);

/// Write the proof after a matching cloud check, or delete it when the
/// check says the folder is missing. `client_id` / `robot_id` /
/// `file_count` are recorded only on a match; the robot reads the hash.
bool settleCloudVerified(const QString& folder,
                         bool matches,
                         const QString& client_id,
                         const QString& robot_id,
                         int file_count);

// ---------------------------------------------------------------------------
// Stick copy check (robot record vs. what is on the stick)
// ---------------------------------------------------------------------------

/// Laptop -> robot request, written into a `Mission_*` folder on the
/// stick. Same file and format as `uploader.py` `write_needs_recopy`;
/// the robot re-copies the listed files and deletes it.
extern const char* const kNeedsRecopyFile;

/// The copy records of one building folder, read once per probe.
struct BuildingCopyRecords {
    struct Record {
        QString mission;          ///< `Mission_*` folder name.
        QString dir;              ///< Absolute path of that folder.
        bool readable = false;    ///< Parsed as a JSON object.
        bool per_file = false;    ///< Has a `files` map (schema 2).
        QStringList sections;
        /// `<folder>/<relpath>` -> size, from `files`.
        QHash<QString, qint64> sizes;
    };
    QList<Record> records;
    /// True when any record has a `files` map. On such a stick every
    /// copied folder is listed by one.
    bool any_per_file = false;
};

BuildingCopyRecords loadBuildingCopyRecords(const QString& building_dir);

struct StickCopyVerdict {
    StickCopyProblem problem = StickCopyProblem::None;
    /// Copy-record keys (`<folder>/<relpath>`) missing or the wrong size.
    QStringList bad_keys;
    /// Mission folder whose record covers the folder ("" when none).
    QString mission_dir;
};

/// Same rules as `uploader.py` (`find_thumb_manifest`, `bad_stick_files`,
/// the unfinished-copy check). `local_sizes` maps each file's path
/// relative to the folder to its size on the stick.
StickCopyVerdict checkStickCopy(const BuildingCopyRecords& records,
                                const QString& folder,
                                const QHash<QString, qint64>& local_sizes);

/// Merge `keys` into `<mission_dir>/needs_recopy.json` and flush it to
/// the stick. No write when every key is already listed. False on an
/// I/O error.
bool mergeNeedsRecopy(const QString& mission_dir, const QStringList& keys);

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
 *  - Hiding the dialog does not pause. Quitting the OCU calls
 *    `UploadDialog::pauseForShutdown()`, which is this method plus a
 *    10 s wait and then `requestCancel()`.
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

    /// ThumbDrive: `BDR_ROBOT_SSH=<user>@<host>` for uploader.py, so a
    /// stick file that does not match the robot's record is fetched from
    /// the robot and the upload continues. Empty host turns it off.
    void setRobotFetch(const QString& host, const QString& ssh_user);
    /// The `BDR_ROBOT_SSH` value, or empty when not set (tests).
    QString robotFetchTarget() const;

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

    /// ThumbDrive: where this laptop keeps its mission logs (tests only;
    /// empty = ocu_logs::storeDir()). Before a Mission_* folder uploads,
    /// the logs its mission_config.json lists are copied into
    /// <Mission>/logs/ocu/ on the stick; after it uploads they are marked
    /// uploaded. See ocu_log_store.hpp.
    void setOcuLogStoreDir(const QString& dir) { ocu_log_store_dir_ = dir; }

    /// Set the queue. Replaces any prior queue. No-op if the runner is
    /// currently busy — call `requestCancel()` first.
    void setQueue(const QList<UploadTarget>& targets);

    /// Start (or resume after pause) processing the queue from the
    /// current head. Idempotent.
    void start();

    /// Graceful pause for the *currently running* target (does nothing
    /// when idle). The active script finishes the in-flight file then
    /// exits. `reason` is what `targetPaused` reports ("Paused",
    /// "Cloud unreachable", "Drive removed"); empty means "Paused".
    void requestPause(const QString& reason = QString());

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

    /// True when the last queue stopped on an error that every folder
    /// would hit (`isQueueStoppingError`) with folders still unstarted.
    bool stoppedEarly() const { return stopped_early_; }

    /// Errors after which the rest of the queue is not attempted:
    /// the cloud refusing the robot's credentials (HTTP 401/403), a
    /// missing setting or interpreter, or the stick folder gone. Any
    /// other failure is that folder's alone and the queue moves on.
    static bool isQueueStoppingError(const QString& error);

    // ----- Retry policy (transient connection errors) -----
    //
    // The script auto-pauses on `requests.exceptions.RequestException`
    // and exits rc=0 (so the OCU never sees a hard error for a wifi
    // blip).  We retry the same target
    // up to `kMaxConnectionRetries` times with exponential backoff;
    // each retry re-launches the SSH command identically — the script
    // re-reads `upload_state.json` and skips the files it already
    // landed, so retries are idempotent and never duplicate uploads.
    // A `--force` launch clears that flag before the retry, so the
    // retry does not delete the state file the first launch wrote.
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

    /// Hard error from the script (5GB ceiling, damaged stick copy,
    /// etc.). The runner moves on to the next folder unless
    /// `isQueueStoppingError(error)`, in which case it stops the queue.
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
    /// Second half of launchActiveTarget, after the OCU logs are attached.
    void spawnUploader(const UploadTarget& target);
    QString ocuLogStoreDir() const;
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
    // ThumbDrive repair source (setRobotFetch).
    QString robot_fetch_host_;
    QString robot_fetch_user_;

    // Queue state.
    QList<UploadTarget> queue_;
    int current_index_ = -1;
    int queue_total_ = 0;
    bool busy_ = false;
    bool stopped_early_ = false;
    bool cancel_requested_ = false;
    bool paused_during_target_ = false;
    QString pause_reason_;
    QString fatal_error_;

    QPointer<QProcess> proc_;
    QString stdout_carry_;  // partial line buffer between readyRead chunks.

    // OCU logs being copied to the stick before the uploader starts (on a
    // worker thread). The seq drops a result that outlived its queue.
    QString ocu_log_store_dir_;
    bool attach_pending_ = false;
    int attach_seq_ = 0;

    // Per-target retry counter — bumped each time we re-launch the
    // current_index_ target after a transient connection error.
    // Reset whenever current_index_ advances.
    int retry_attempts_done_ = 0;
    bool retry_pending_ = false;
    bool retry_enabled_ = true;
    QTimer* retry_timer_ = nullptr;
};

}  // namespace f2c_cpp
