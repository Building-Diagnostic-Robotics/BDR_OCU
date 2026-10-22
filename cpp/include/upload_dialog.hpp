/**
 * @file upload_dialog.hpp
 * @brief Stage 3 Upload Data dialog — cloud upload from the RDATA_EXT stick.
 *
 * The robot's offload worker copies every finalized mission to a USB
 * stick labelled `RDATA_EXT`. The operator carries the stick to the
 * laptop, and this dialog drives `uploader.py` locally against it: the
 * script asks the BDR backend for a presigned URL per file and PUTs to S3.
 * The laptop holds no AWS credentials — only the per-robot
 * `cloud_client_id` / `cloud_device_token` from `robots.json`. Upload
 * state is written on the stick next to the data, so a resume works from
 * any laptop.
 *
 * `UploadSource::RobotSsh` — the original design, same script run on the
 * robot over SSH — is still compiled behind `setSource()` but is not
 * reachable from the UI. Fallback only.
 *
 * Lifecycle:
 *  1. `setSource()` + `setCloudAuth()` + `setRobotId()` from
 *     `AppShellWindow::onUploadDataRequested()` (called whenever the
 *     dashboard "Upload Data" quick-action fires).
 *  2. `showEvent` arms `ThumbDriveWatcher`. Once the stick is mounted,
 *     `UploadStateProbe` walks it and the dialog lists one row per
 *     building (operator, newest scan, summed size, rolled-up status).
 *     Every Mission_* and Section_* folder of that building, on any
 *     date, sits on the same row. The only source is the mounted
 *     RDATA_EXT stick — there is no folder picker.
 *  3. Operator selects buildings, presses **Upload Data**. The dialog
 *     expands each ticked row into the folders that are not already
 *     uploaded (oldest visit first) and feeds that queue to
 *     `UploadRunner`, which spawns one `python3 -u uploader.py …`
 *     subprocess per folder.
 *  4. Per-file events stream in via `UploadRunner::fileUploaded`;
 *     the building row's status and the progress bar update live.
 *  5. **Pause** touches `pause.flag`; the script exits cleanly at the
 *     next file boundary. Resume = re-press Upload.
 *  6. **Cancel** SIGTERMs the subprocess. State on disk is preserved
 *     so the next run picks up where the kill landed.
 *  7. Closing the dialog hides it. An upload that is already running
 *     keeps running; the drive watcher stays armed so pulling the stick
 *     still pauses. Quitting the OCU is the path that pauses — see
 *     `pauseForShutdown()`. Pulling the stick mid-upload pauses too.
 *
 * Visual style follows the existing frameless dialog family
 * (`OfflineFinalizeDialog`, `MissionMetadataDialog`,
 * `MissionFinalizeDialog`).
 */

#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QHash>
#include <QPair>
#include <QSet>
#include <QList>
#include <QPoint>
#include <functional>

#include <QPointer>
#include <QString>

#include "cloud_verifier.hpp"
#include "robot_reachability_probe.hpp"
#include "thumb_drive_watcher.hpp"
#include "upload_runner.hpp"

class QCheckBox;
class QFrame;
class QHBoxLayout;
class QLabel;
class QProcess;
class QProgressBar;
class QPushButton;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;
class QShowEvent;
class QCloseEvent;
class QMouseEvent;

namespace f2c_cpp {

class UploadStateProbe;
class UploadRunner;

/// The hover card on a building row, as rich text in the theme's colors:
/// a bold section count, one colored dot per state, then a list of the
/// sections the operator has to act on.
QString uploadRowTooltipHtml(const UploadRowSummary& summary, bool dark_mode);

class UploadDialog : public QDialog {
    Q_OBJECT
public:
    explicit UploadDialog(QWidget* parent = nullptr);
    ~UploadDialog() override;

    void setDarkMode(bool dark);

    /// Defaults to ThumbDrive. Ignored while an upload is running.
    void setSource(UploadSource source);
    UploadSource source() const { return source_; }

    /// RobotSsh only.
    void setRemote(const QString& host, const QString& ssh_user);
    void setCloudAuth(const QString& api_base,
                      const QString& client_id,
                      const QString& device_token);
    void setRobotId(const QString& robot_id);
    /// RobotSsh only — the robot's data root, defaults to "/R_DATA".
    /// ThumbDrive derives the root from the mount point.
    void setDataRoot(const QString& root);
    /// ThumbDrive: filesystem label to wait for. Defaults to RDATA_EXT.
    void setDriveLabel(const QString& label);

    /// True while `uploader.py` is running or a retry is waiting.
    /// Closing the dialog does not change this.
    bool isUploading() const;

    /// Empty when a new upload is allowed. AppShell supplies the scan,
    /// copy, and profile gate with "uploading" forced false, so the
    /// button inside an already-open dialog follows the same rule as
    /// the Dashboard card. Consulted only while the runner is idle.
    void setUploadBlockReason(std::function<QString()> reason);

    /// Drops the Dashboard card notice (drive removed, resume, failed).
    /// Stops the drive watcher when this dialog is hidden and nothing
    /// is uploading, so a cleared notice does not keep polling. The
    /// progress line inside the dialog is left as it is.
    void clearPendingNotice();

    /// Quit path. Asks the script to pause and waits up to 10 s for the
    /// files already in progress to finish, then SIGTERMs whatever is
    /// still running. A retry wait has no process, so that case cancels
    /// immediately. Already-uploaded files stay in `upload_state.json`.
    void pauseForShutdown();

signals:
    /// Empty when nothing is running and nothing is waiting on the
    /// operator. Otherwise the Dashboard Upload card subtitle
    /// ("Uploading · 271 / 11640 files", "Upload paused — drive removed").
    void backgroundStatusChanged(const QString& status);

    /// One-shot operator notice while the dialog is hidden (stick pulled,
    /// upload failed). The shell shows it as a toast.
    void uploadAttentionRequested(const QString& message);

public slots:
    /// Esc hides the dialog through `close()`. A running upload keeps
    /// going. `QDialog::closeEvent` calls `reject()` again; `in_close_`
    /// makes that second call the plain hide.
    void reject() override;

protected:
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private slots:
    void onProbeReady(bool ok, const QList<UploadTarget>& targets,
                      const QString& error);
    void onRefreshClicked();
    void onSelectAllClicked();
    void onTreeItemChanged(QTreeWidgetItem* item, int column);
    void onUploadClicked();
    void onPauseClicked();
    void onCancelClicked();
    void onCloseClicked();

    void onTargetStarted(int index, int total, const UploadTarget& target);
    void onFileUploaded(const UploadTarget& target, const QString& relpath);
    void onFileSkipped(const UploadTarget& target, const QString& relpath);
    void onLogLine(const QString& line);
    void onTargetPaused(const UploadTarget& target, const QString& reason);
    void onTargetFailed(const UploadTarget& target, const QString& error);
    void onTargetCompleted(const UploadTarget& target);
    void onTargetRetryScheduled(const UploadTarget& target,
                                int attempt, int wait_ms,
                                const QString& reason);
    void onQueueFinished(bool cancelled);

    // Connectivity gating.
    void onReachabilityChanged(RobotReachabilityProbe::State old_state,
                               RobotReachabilityProbe::State new_state);
    void onDriveStateChanged(ThumbDriveWatcher::State state,
                             const QString& mount_path);
    void onCloudProbeTick();
    void onCloudProbeFinished(int exit_code, int /*QProcess::ExitStatus*/ status);

private:
    struct Selection {
        int buildings = 0;
        QList<UploadTarget> folders;
    };

    void buildUi();
    void applyStyle();
    void repopulateTree();
    QString formatWhen(const QDateTime& when, const QString& date_folder) const;
    void refreshSelectionSummary();
    void refreshButtonStates();
    void refreshHeaderSubtitle();
    void resetProgress();
    void applyBuildingRow(QTreeWidgetItem* row, const UploadBuildingRow& view,
                          bool tick_if_missing);
    void refreshRowForTarget(const UploadTarget& target, bool tick_if_missing);
    QTreeWidgetItem* findBuildingRow(const QString& key) const;
    QString formatBytes(qint64 bytes) const;
    Selection currentSelection() const;
    int listedBuildingCount() const;
    void startProbe();

    // S3 check of every finished folder that does not already carry a
    // valid cloud_verified.json. Oldest first, in batches. A failure or
    // an offline cloud leaves the unchecked folders Uploaded.
    void maybeStartCloudCheck();
    void launchNextCloudBatch();
    /// True when a batch was handed to the verifier.
    bool startCloudCheck(const QList<UploadTarget>& batch);
    void cancelCloudCheck(bool reschedule);
    void revertCheckingRows();
    void onCloudRunFinished(int generation, const QString& run_id, bool http_ok,
                            const QList<CloudObject>& files, const QString& error);
    void onManifestsLoaded(int generation,
                           const QHash<QString, LoadedManifest>& by_run);
    void applyCloudResult(const QString& run_id);
    void presentCloudRow(const UploadTarget& target, bool tick_if_missing);
    void noteCloudVerify(const QString& line);

    // Connectivity helpers.
    void armConnectivityProbes();
    void disarmConnectivityProbes();
    /// Stops the cloud `curl` probe without clearing `cloud_reachable_`.
    /// Hiding a running upload uses this so the next drive event does
    /// not look like "cloud went down" and pause the runner.
    void stopCloudProbe();
    void publishBackgroundActivity();
    void noteDriveRemoved();
    void refreshConnectivityBanner();
    void handleConnectivityTransition();
    /// The data source is usable: stick mounted (ThumbDrive) or robot
    /// reachable (RobotSsh).
    bool isSourceReady() const;
    bool isFullyOnline() const { return isSourceReady() && cloud_reachable_; }
    bool isThumbDrive() const { return source_ == UploadSource::ThumbDrive; }
    /// Human name of the source for status strings ("drive" / "robot").
    QString sourceNoun() const;

    bool dark_mode_ = false;
    bool in_close_ = false;
    bool shutting_down_ = false;
    /// Rows have their checkboxes locked because an upload is running.
    bool rows_locked_ = false;
    /// Re-apply every row's flags (checkable or not) after `rows_locked_`
    /// changes.
    void relockRows();
    /// Cancel (unlike pause or a finished upload) used to skip the
    /// rescan, so a `--force` that was killed before it deleted the
    /// manifest stayed "Uploaded" with the force flag already cleared.
    bool recheck_after_cancel_ = false;
    UploadSource source_ = UploadSource::ThumbDrive;
    QString remote_host_;
    QString ssh_user_;
    QString robot_id_;
    QString data_root_ = QStringLiteral("/R_DATA");   // RobotSsh root
    QString drive_mount_;                             // ThumbDrive root (live)
    bool drive_mounted_ = false;
    ThumbDriveWatcher::State drive_state_ = ThumbDriveWatcher::State::Absent;
    QString cloud_api_base_;
    QString cloud_client_id_;
    QString cloud_device_token_;

    // All section/mission rows discovered by the probe, keyed by run_id.
    QHash<QString, UploadTarget> all_targets_;
    bool probe_in_progress_ = false;
    QString last_probe_error_;

    // Per-target file counters during an active upload.
    int active_files_done_ = 0;
    int active_files_total_ = 0;
    int active_queue_index_ = -1;
    int active_queue_total_ = 0;
    /// What the bar shows: the whole building being uploaded, every
    /// folder of it, not just the active folder.
    BuildingUploadProgress building_progress_;
    /// "Uploading <building> · folder 3 of 10"; showBuildingProgress()
    /// appends " · 45%". The bar itself carries no text: dark text on the
    /// blue fill was unreadable.
    QString progress_caption_base_;
    void showBuildingProgress();
    int buildingPercent() const;

    UploadStateProbe* probe_ = nullptr;
    UploadRunner* runner_ = nullptr;
    CloudVerifier* cloud_verifier_ = nullptr;

    // `probe_serial_` advances on each successful stick listing.
    // `cloud_check_for_probe_` is that serial once a check has been
    // started for it, so opening the dialog checks once per listing
    // and a later cloud-up retries a check that never started.
    int probe_serial_ = 0;
    int cloud_check_for_probe_ = -1;
    int verify_generation_ = 0;
    bool manifests_ready_ = false;
    /// Finished folders still waiting for a batch. Oldest first.
    QList<UploadTarget> cloud_check_queue_;
    QStringList cloud_check_ids_;
    QHash<QString, QList<CloudObject>> cloud_listings_;
    QHash<QString, LoadedManifest> loaded_manifests_;

    // Connectivity gating (Decision #3 + #4).  Robot reachability uses
    // the existing layered ICMP→TCP-22 probe; cloud reachability uses
    // a 2 s `curl --max-time 4` probe against `<api>/`.  Both must be
    // green before Upload is enabled; either flipping red mid-upload
    // pauses the runner gracefully (no auto-resume on recovery —
    // operator clicks Upload again).
    RobotReachabilityProbe* reachability_probe_ = nullptr;   // RobotSsh
    ThumbDriveWatcher* drive_watcher_ = nullptr;             // ThumbDrive
    QTimer* cloud_probe_timer_ = nullptr;
    QProcess* cloud_probe_proc_ = nullptr;
    bool robot_reachable_ = false;
    bool cloud_reachable_ = false;
    int cloud_probe_failures_ = 0;
    bool cloud_probe_seen_response_ = false;
    bool offline_pause_active_ = false;
    QString last_offline_reason_;
    /// Card text that outlives the runner: drive pulled, or a failure
    /// while the dialog was hidden. Cleared when Upload is pressed again
    /// or the operator cancels.
    QString background_hold_;
    QString published_background_status_;

    /// Folders that failed in the current queue, "<building> / <folder>"
    /// with the reason. Shown once, when the queue ends.
    QList<QPair<QString, QString>> failed_this_run_;
    /// The queue ended on a pause (Pause, offline, drive removed), not
    /// because every folder was dispatched.
    bool queue_paused_ = false;
    /// Started when Upload is pressed. A Pause press inside the first
    /// second lands where the Upload button just was (double click,
    /// remote-desktop lag) and is ignored.
    QElapsedTimer upload_started_;
    /// Restarted on every uploaded or skipped file. While it is fresh,
    /// the upload itself proves the cloud is reachable.
    QElapsedTimer last_file_progress_;
    /// Building rows ticked before a rescan, re-ticked after it when
    /// still selectable.
    QSet<QString> sticky_checked_;
    /// `sync -f` on the stick after a queue stops, so whatever the
    /// uploader and the OCU wrote there is on the flash before the
    /// card says the operator can take it.
    QPointer<QProcess> flush_proc_;
    void flushStickThen(const QString& final_caption);
    /// See `setUploadBlockReason`. Empty function means no extra gate.
    std::function<QString()> upload_block_reason_;

    QPoint drag_offset_;
    bool dragging_ = false;

    // Header / chrome.
    QWidget* header_ = nullptr;
    QLabel* lbl_title_ = nullptr;
    QLabel* lbl_subtitle_ = nullptr;
    QPushButton* btn_close_x_ = nullptr;

    // Connectivity banner (Decision #3 + #4).  Hidden when both
    // reachability + cloud probes are green; switches to amber when
    // either flips red.
    QFrame* offline_banner_ = nullptr;
    QLabel* offline_banner_label_ = nullptr;

    // Filter row.
    QPushButton* btn_refresh_ = nullptr;
    QLabel* lbl_probe_status_ = nullptr;

    // Tree + summary.
    QTreeWidget* tree_ = nullptr;
    QLabel* lbl_tree_status_ = nullptr;
    QPushButton* btn_select_all_ = nullptr;
    QLabel* lbl_selection_summary_ = nullptr;

    // Progress block.
    QWidget* progress_block_ = nullptr;
    QProgressBar* progress_bar_ = nullptr;
    QLabel* lbl_progress_caption_ = nullptr;
    QLabel* lbl_progress_detail_ = nullptr;

    // Buttons.
    QPushButton* btn_upload_ = nullptr;
    QPushButton* btn_pause_ = nullptr;
    QPushButton* btn_cancel_ = nullptr;
    QPushButton* btn_close_footer_ = nullptr;
};

}  // namespace f2c_cpp
