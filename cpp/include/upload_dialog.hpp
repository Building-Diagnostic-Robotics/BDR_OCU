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
 *     `UploadStateProbe` walks it and the dialog lists every section
 *     as a flat row (building, operator, date/time, size, status),
 *     classified None/Partial/Done from the sentinel files beside the data.
 *     Browse… lets the operator point at an unlabelled stick or a
 *     local copy.
 *  3. Operator selects sections, presses **Upload Data**. The dialog
 *     builds a queue of `UploadTarget`s (sequential per design
 *     decision) and feeds it to `UploadRunner`, which spawns one
 *     `python3 -u uploader.py …` subprocess per target.
 *  4. Per-file events stream in via `UploadRunner::fileUploaded`;
 *     the tree row's per-section count + progress bar update live.
 *  5. **Pause** touches `pause.flag`; the script exits cleanly at the
 *     next file boundary. Resume = re-press Upload.
 *  6. **Cancel** SIGTERMs the subprocess. State on disk is preserved
 *     so the next run picks up where the kill landed.
 *  7. Closing the dialog while busy implicitly pauses (graceful) — see
 *     `closeEvent`. Pulling the stick mid-upload does the same.
 *
 * Visual style follows the existing frameless dialog family
 * (`OfflineFinalizeDialog`, `MissionMetadataDialog`,
 * `TiltCalibrationDialog`).
 */

#pragma once

#include <QDialog>
#include <QHash>
#include <QList>
#include <QPoint>
#include <QPointer>
#include <QString>

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
    void onBrowseClicked();
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
    void buildUi();
    void applyStyle();
    void repopulateTree();
    QString formatWhen(const UploadTarget& target) const;
    void refreshSelectionSummary();
    void refreshButtonStates();
    void refreshHeaderSubtitle();
    void resetProgress();
    void setSectionRowStatus(QTreeWidgetItem* row, const UploadTarget& target);
    QTreeWidgetItem* findSectionRow(const UploadTarget& target) const;
    QString formatBytes(qint64 bytes) const;
    QList<UploadTarget> selectedTargets() const;
    void startProbe();

    // Connectivity helpers.
    void armConnectivityProbes();
    void disarmConnectivityProbes();
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

    UploadStateProbe* probe_ = nullptr;
    UploadRunner* runner_ = nullptr;

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
    QPushButton* btn_browse_ = nullptr;   // ThumbDrive only
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
