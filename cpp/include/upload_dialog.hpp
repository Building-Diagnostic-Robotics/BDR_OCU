/**
 * @file upload_dialog.hpp
 * @brief Stage 3 Upload Data dialog — robot-side cloud upload UI.
 *
 * Replaces the legacy `CloudUploadDialog` + `DataTransferDialog` pair.
 * Where the legacy dialogs orchestrated `aws s3 sync` from the laptop
 * (with IAM credentials baked into `~/.aws`), this dialog is a thin
 * driver for `pilot_control/scripts/uploader.py` running on the robot
 * over SSH. The robot holds a presigned-URL `device_token`, the laptop
 * has zero AWS state, and progress streams back via stdout regex.
 *
 * Lifecycle:
 *  1. `setRemote()` + `setCloudAuth()` + `setRobotId()` from
 *     `AppShellWindow::onUploadDataRequested()` (called whenever the
 *     dashboard "Upload Data" quick-action fires).
 *  2. `showEvent` triggers `UploadStateProbe` → the dialog populates
 *     a date → building → section/mission tree, classifying each
 *     section as None/Partial/Done from sentinel files on the robot.
 *  3. Operator selects sections, presses **Upload Data**. The dialog
 *     builds a queue of `UploadTarget`s (sequential per design
 *     decision) and feeds it to `UploadRunner`, which spawns one SSH
 *     `python3 -u uploader.py …` subprocess per target.
 *  4. Per-file events stream in via `UploadRunner::fileUploaded`;
 *     the tree row's per-section count + progress bar update live.
 *  5. **Pause** SSH-touches `pause.flag`; the script exits cleanly at
 *     the next file boundary. Resume = re-press Upload.
 *  6. **Cancel** SIGTERMs the subprocess. State on disk is preserved
 *     so the next run picks up where the kill landed.
 *  7. Closing the dialog while busy implicitly pauses (graceful) — see
 *     `closeEvent`.
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
#include "upload_runner.hpp"

class QCheckBox;
class QComboBox;
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
    void setRemote(const QString& host, const QString& ssh_user);
    void setCloudAuth(const QString& api_base,
                      const QString& client_id,
                      const QString& device_token);
    void setRobotId(const QString& robot_id);
    void setDataRoot(const QString& root);  // defaults to "/R_DATA"

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
    void onDateChanged(int index);
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
    void onCloudProbeTick();
    void onCloudProbeFinished(int exit_code, int /*QProcess::ExitStatus*/ status);

private:
    void buildUi();
    void applyStyle();
    void rebuildDateCombo();
    void repopulateTree();
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
    bool isFullyOnline() const { return robot_reachable_ && cloud_reachable_; }

    bool dark_mode_ = false;
    QString remote_host_;
    QString ssh_user_;
    QString robot_id_;
    QString data_root_ = QStringLiteral("/R_DATA");
    QString cloud_api_base_;
    QString cloud_client_id_;
    QString cloud_device_token_;

    // All section/mission rows discovered by the probe, keyed by run_id.
    QHash<QString, UploadTarget> all_targets_;
    QStringList ordered_dates_;
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
    RobotReachabilityProbe* reachability_probe_ = nullptr;
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
    QComboBox* combo_date_ = nullptr;
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
