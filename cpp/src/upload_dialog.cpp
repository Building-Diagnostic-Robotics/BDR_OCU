/**
 * @file upload_dialog.cpp
 * @brief Implementation of UploadDialog.
 */

#include "upload_dialog.hpp"

#include <algorithm>

#include <utility>

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QMouseEvent>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

#include "components/bdr_message_box.hpp"
#include "ui_theme_constants.hpp"

namespace f2c_cpp {

namespace {

constexpr int kBuildingKeyRole = Qt::UserRole + 1;
// One /verify request per folder. A batch keeps a slow stick from
// starting hundreds of listings at once; the rest follow oldest-first.
constexpr int kCloudVerifyBatch = 10;
// Quit waits this long for the files already in flight to finish
// before SIGTERM. One file can be up to 5 GB, so this is a ceiling,
// not a guarantee that every in-flight PUT completes.
constexpr int kShutdownPauseMs = 10000;

const QString kHoldDriveRemoved = QStringLiteral("Upload paused — drive removed");
const QString kHoldResume = QStringLiteral("Upload paused — press Upload to resume");
const QString kHoldFailed = QStringLiteral("Upload failed — open Upload Data");

QString scansFoundText(int count) {
    return QStringLiteral("Found %1 scan%2.")
        .arg(count)
        .arg(count == 1 ? QString() : QStringLiteral("s"));
}

QColor badgeColor(UploadRowBadge badge, bool dark) {
    switch (badge) {
        case UploadRowBadge::NeedsRobot:
            return QColor(uiThemeTokens(dark).danger);
        case UploadRowBadge::Missing:
        case UploadRowBadge::Partial:
            return QColor(QStringLiteral("#F59E0B"));
        case UploadRowBadge::Uploaded:
        case UploadRowBadge::Checking:
        case UploadRowBadge::Verified:
            return QColor(QStringLiteral("#10B981"));
        case UploadRowBadge::Pending:
            break;
    }
    return dark ? QColor(QStringLiteral("#A1A1AA"))
                : QColor(QStringLiteral("#52525B"));
}

struct ManifestBatch {
    int generation = 0;
    QHash<QString, LoadedManifest> by_run;
};

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

UploadDialog::UploadDialog(QWidget* parent) : QDialog(parent) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::Dialog);
    setAttribute(Qt::WA_TranslucentBackground);
    setModal(true);
    setMinimumSize(980, 620);

    probe_ = new UploadStateProbe(this);
    runner_ = new UploadRunner(this);
    cloud_verifier_ = new CloudVerifier(this);
    connect(cloud_verifier_, &CloudVerifier::runFinished,
            this, &UploadDialog::onCloudRunFinished);

    connect(probe_, &UploadStateProbe::targetsReady,
            this, &UploadDialog::onProbeReady);

    connect(runner_, &UploadRunner::targetStarted,
            this, &UploadDialog::onTargetStarted);
    connect(runner_, &UploadRunner::fileUploaded,
            this, &UploadDialog::onFileUploaded);
    connect(runner_, &UploadRunner::fileSkipped,
            this, &UploadDialog::onFileSkipped);
    connect(runner_, &UploadRunner::logLine,
            this, &UploadDialog::onLogLine);
    connect(runner_, &UploadRunner::targetPaused,
            this, &UploadDialog::onTargetPaused);
    connect(runner_, &UploadRunner::targetFailed,
            this, &UploadDialog::onTargetFailed);
    connect(runner_, &UploadRunner::targetCompleted,
            this, &UploadDialog::onTargetCompleted);
    connect(runner_, &UploadRunner::targetRetryScheduled,
            this, &UploadDialog::onTargetRetryScheduled);
    connect(runner_, &UploadRunner::queueFinished,
            this, &UploadDialog::onQueueFinished);

    reachability_probe_ = new RobotReachabilityProbe(this);
    connect(reachability_probe_, &RobotReachabilityProbe::reachabilityChanged,
            this, &UploadDialog::onReachabilityChanged);

    drive_watcher_ = new ThumbDriveWatcher(this);
    connect(drive_watcher_, &ThumbDriveWatcher::stateChanged,
            this, &UploadDialog::onDriveStateChanged);

    cloud_probe_timer_ = new QTimer(this);
    cloud_probe_timer_->setInterval(2000);
    cloud_probe_timer_->setSingleShot(false);
    connect(cloud_probe_timer_, &QTimer::timeout,
            this, &UploadDialog::onCloudProbeTick);

    buildUi();
    applyStyle();
    refreshHeaderSubtitle();
    refreshConnectivityBanner();
    refreshButtonStates();
}

UploadDialog::~UploadDialog() {
    shutting_down_ = true;
    cancelCloudCheck(false);
    if (runner_ && runner_->isBusy()) {
        runner_->requestCancel();
    }
    disarmConnectivityProbes();
}

void UploadDialog::setDarkMode(bool dark) {
    dark_mode_ = dark;
    applyStyle();
}

void UploadDialog::setSource(UploadSource source) {
    if (runner_ && runner_->isBusy()) {
        qWarning("[UploadDialog] setSource while uploading: ignored.");
        return;
    }
    source_ = source;
    if (probe_) probe_->setSource(source_);
    if (runner_) runner_->setSource(source_);
    refreshHeaderSubtitle();
    refreshButtonStates();
}

void UploadDialog::setDriveLabel(const QString& label) {
    if (drive_watcher_) drive_watcher_->setLabel(label);
    refreshHeaderSubtitle();
}

void UploadDialog::setRemote(const QString& host, const QString& ssh_user) {
    remote_host_ = host.trimmed();
    ssh_user_ = ssh_user.trimmed();
    if (probe_) probe_->setRemote(remote_host_, ssh_user_);
    if (runner_) runner_->setRemote(remote_host_, ssh_user_);
}

void UploadDialog::setCloudAuth(const QString& api_base,
                                const QString& client_id,
                                const QString& device_token) {
    cloud_api_base_ = api_base.trimmed();
    cloud_client_id_ = client_id.trimmed();
    cloud_device_token_ = device_token.trimmed();
    if (runner_) {
        runner_->setCloudAuth(cloud_api_base_, cloud_client_id_,
                              cloud_device_token_);
    }
    if (cloud_verifier_) {
        cloud_verifier_->setAuth(cloud_api_base_, cloud_client_id_,
                                 cloud_device_token_);
    }
    refreshHeaderSubtitle();
}

void UploadDialog::setRobotId(const QString& robot_id) {
    robot_id_ = robot_id.trimmed();
    if (runner_) runner_->setRobotId(robot_id_);
    refreshHeaderSubtitle();
}

void UploadDialog::setDataRoot(const QString& root) {
    if (!root.trimmed().isEmpty()) {
        data_root_ = root.trimmed();
    }
    // The probe's root is set at probe time (ThumbDrive uses the live
    // mount); nothing more to push here.
}

bool UploadDialog::isSourceReady() const {
    return isThumbDrive() ? drive_mounted_ : robot_reachable_;
}

QString UploadDialog::sourceNoun() const {
    return isThumbDrive() ? QStringLiteral("drive") : QStringLiteral("robot");
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

void UploadDialog::buildUi() {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* card = new QFrame(this);
    card->setObjectName("UploadDialogCard");
    outer->addWidget(card);

    auto* root = new QVBoxLayout(card);
    root->setContentsMargins(28, 24, 28, 22);
    root->setSpacing(16);

    // Header row (drag handle).
    header_ = new QWidget(card);
    header_->setObjectName("UploadDialogHeader");
    auto* header_row = new QHBoxLayout(header_);
    header_row->setContentsMargins(0, 0, 0, 0);
    header_row->setSpacing(8);
    {
        auto* col = new QVBoxLayout();
        col->setContentsMargins(0, 0, 0, 0);
        col->setSpacing(2);
        lbl_title_ = new QLabel(QStringLiteral("Upload Data"), header_);
        lbl_title_->setObjectName("UploadDialogTitle");
        lbl_subtitle_ = new QLabel(header_);
        lbl_subtitle_->setObjectName("UploadDialogSubtitle");
        col->addWidget(lbl_title_);
        col->addWidget(lbl_subtitle_);
        header_row->addLayout(col, 1);
    }
    btn_close_x_ = new QPushButton(QStringLiteral("\u2715"), header_);
    btn_close_x_->setObjectName("UploadDialogCloseX");
    btn_close_x_->setCursor(Qt::PointingHandCursor);
    btn_close_x_->setFixedSize(28, 28);
    btn_close_x_->setFlat(true);
    connect(btn_close_x_, &QPushButton::clicked,
            this, &UploadDialog::onCloseClicked);
    header_row->addWidget(btn_close_x_, 0, Qt::AlignTop);
    root->addWidget(header_);

    // Connectivity banner (Decision #3 + #4).  Hidden by default;
    // surfaces an amber strip when robot OR cloud is unreachable.
    offline_banner_ = new QFrame(card);
    offline_banner_->setObjectName("UploadDialogOfflineBanner");
    auto* banner_layout = new QHBoxLayout(offline_banner_);
    banner_layout->setContentsMargins(12, 8, 12, 8);
    banner_layout->setSpacing(8);
    offline_banner_label_ = new QLabel(offline_banner_);
    offline_banner_label_->setObjectName("UploadDialogOfflineBannerLabel");
    offline_banner_label_->setWordWrap(true);
    banner_layout->addWidget(offline_banner_label_, 1);
    offline_banner_->setVisible(false);
    root->addWidget(offline_banner_);

    // Filter row.
    auto* filter_row = new QHBoxLayout();
    filter_row->setContentsMargins(0, 0, 0, 0);
    filter_row->setSpacing(10);

    btn_refresh_ = new QPushButton(QStringLiteral("Refresh"), card);
    btn_refresh_->setObjectName("UploadDialogSecondary");
    btn_refresh_->setCursor(Qt::PointingHandCursor);
    btn_refresh_->setMinimumHeight(34);
    connect(btn_refresh_, &QPushButton::clicked,
            this, &UploadDialog::onRefreshClicked);
    filter_row->addWidget(btn_refresh_, 0);

    filter_row->addStretch(1);

    lbl_probe_status_ = new QLabel(card);
    lbl_probe_status_->setObjectName("UploadDialogProbeStatus");
    lbl_probe_status_->setMinimumHeight(34);
    filter_row->addWidget(lbl_probe_status_, 0);

    root->addLayout(filter_row);

    // Tree + select-all row.
    tree_ = new QTreeWidget(card);
    tree_->setObjectName("UploadDialogTree");
    tree_->setColumnCount(5);
    tree_->setHeaderLabels({QStringLiteral("Building"),
                            QStringLiteral("Operator"),
                            QStringLiteral("Date"),
                            QStringLiteral("Size"),
                            QStringLiteral("Status")});
    tree_->setRootIsDecorated(false);
    tree_->setItemsExpandable(false);
    tree_->setIndentation(12);
    tree_->setUniformRowHeights(true);
    tree_->setSelectionMode(QAbstractItemView::NoSelection);
    tree_->setFocusPolicy(Qt::NoFocus);
    tree_->header()->setStretchLastSection(true);
    tree_->setColumnWidth(0, 240);
    tree_->setColumnWidth(1, 160);
    tree_->setColumnWidth(2, 180);
    tree_->setColumnWidth(3, 90);
    connect(tree_, &QTreeWidget::itemChanged,
            this, &UploadDialog::onTreeItemChanged);
    root->addWidget(tree_, 1);

    lbl_tree_status_ = new QLabel(card);
    lbl_tree_status_->setObjectName("UploadDialogTreeStatus");
    lbl_tree_status_->setVisible(false);
    root->addWidget(lbl_tree_status_);

    auto* sel_row = new QHBoxLayout();
    sel_row->setContentsMargins(0, 0, 0, 0);
    sel_row->setSpacing(10);
    btn_select_all_ = new QPushButton(QStringLiteral("Select All Pending"), card);
    btn_select_all_->setObjectName("UploadDialogSecondary");
    btn_select_all_->setCursor(Qt::PointingHandCursor);
    btn_select_all_->setMinimumHeight(32);
    connect(btn_select_all_, &QPushButton::clicked,
            this, &UploadDialog::onSelectAllClicked);
    sel_row->addWidget(btn_select_all_, 0);
    sel_row->addStretch(1);
    lbl_selection_summary_ = new QLabel(card);
    lbl_selection_summary_->setObjectName("UploadDialogSummary");
    sel_row->addWidget(lbl_selection_summary_, 0);
    root->addLayout(sel_row);

    // Progress block (hidden until upload begins).
    progress_block_ = new QWidget(card);
    progress_block_->setObjectName("UploadDialogProgressBlock");
    progress_block_->setVisible(false);
    auto* prog_col = new QVBoxLayout(progress_block_);
    prog_col->setContentsMargins(0, 6, 0, 0);
    prog_col->setSpacing(6);
    lbl_progress_caption_ = new QLabel(progress_block_);
    lbl_progress_caption_->setObjectName("UploadDialogProgressCaption");
    progress_bar_ = new QProgressBar(progress_block_);
    progress_bar_->setObjectName("UploadDialogProgressBar");
    progress_bar_->setRange(0, 100);
    progress_bar_->setValue(0);
    progress_bar_->setTextVisible(true);
    progress_bar_->setMinimumHeight(18);
    lbl_progress_detail_ = new QLabel(progress_block_);
    lbl_progress_detail_->setObjectName("UploadDialogProgressDetail");
    lbl_progress_detail_->setWordWrap(true);
    prog_col->addWidget(lbl_progress_caption_);
    prog_col->addWidget(progress_bar_);
    prog_col->addWidget(lbl_progress_detail_);
    root->addWidget(progress_block_);

    // Buttons.
    auto* button_row = new QHBoxLayout();
    button_row->setContentsMargins(0, 8, 0, 0);
    button_row->setSpacing(10);

    btn_close_footer_ = new QPushButton(QStringLiteral("Close"), card);
    btn_close_footer_->setObjectName("UploadDialogCancelBtn");
    btn_close_footer_->setCursor(Qt::PointingHandCursor);
    btn_close_footer_->setMinimumHeight(40);
    connect(btn_close_footer_, &QPushButton::clicked,
            this, &UploadDialog::onCloseClicked);
    button_row->addWidget(btn_close_footer_, 1);

    btn_pause_ = new QPushButton(QStringLiteral("Pause"), card);
    btn_pause_->setObjectName("UploadDialogPauseBtn");
    btn_pause_->setCursor(Qt::PointingHandCursor);
    btn_pause_->setMinimumHeight(40);
    btn_pause_->setVisible(false);
    connect(btn_pause_, &QPushButton::clicked,
            this, &UploadDialog::onPauseClicked);
    button_row->addWidget(btn_pause_, 1);

    btn_cancel_ = new QPushButton(QStringLiteral("Cancel"), card);
    btn_cancel_->setObjectName("UploadDialogStopBtn");
    btn_cancel_->setCursor(Qt::PointingHandCursor);
    btn_cancel_->setMinimumHeight(40);
    btn_cancel_->setVisible(false);
    connect(btn_cancel_, &QPushButton::clicked,
            this, &UploadDialog::onCancelClicked);
    button_row->addWidget(btn_cancel_, 1);

    btn_upload_ = new QPushButton(QStringLiteral("Upload Data"), card);
    btn_upload_->setObjectName("UploadDialogPrimaryBtn");
    btn_upload_->setCursor(Qt::PointingHandCursor);
    btn_upload_->setMinimumHeight(40);
    btn_upload_->setDefault(true);
    connect(btn_upload_, &QPushButton::clicked,
            this, &UploadDialog::onUploadClicked);
    button_row->addWidget(btn_upload_, 2);

    root->addLayout(button_row);
}

void UploadDialog::applyStyle() {
    const auto t = uiThemeTokens(dark_mode_);
    const QString dialog_bg = dark_mode_ ? QStringLiteral("#18181B")
                                         : QStringLiteral("#FFFFFF");
    const QString border = dark_mode_ ? QStringLiteral("#3F3F46")
                                      : QStringLiteral("#E5E7EB");
    const QString text = dark_mode_ ? QStringLiteral("#F4F4F5")
                                    : QStringLiteral("#111827");
    const QString muted = dark_mode_ ? QStringLiteral("#A1A1AA")
                                     : QStringLiteral("#52525B");
    const QString row_alt = dark_mode_ ? QStringLiteral("#1F1F23")
                                       : QStringLiteral("#F9FAFB");
    const QString row_hover = dark_mode_ ? QStringLiteral("#27272A")
                                         : QStringLiteral("#F3F4F6");
    const QString control_bg = dark_mode_ ? QStringLiteral("#27272A")
                                          : QStringLiteral("#F9FAFB");

    setStyleSheet(QStringLiteral(R"CSS(
        QFrame#UploadDialogCard {
            background: %1;
            border: 1px solid %2;
            border-radius: 14px;
        }
        QLabel#UploadDialogTitle {
            color: %3;
            font-family: 'Arimo';
            font-size: 22px;
            font-weight: 700;
            background: transparent;
        }
        QLabel#UploadDialogSubtitle {
            color: %4;
            font-family: 'Arimo';
            font-size: 13px;
            font-weight: 500;
            background: transparent;
        }
        QPushButton#UploadDialogCloseX {
            background: transparent;
            border: none;
            color: %4;
            font-family: 'Arimo';
            font-size: 18px;
            font-weight: 700;
        }
        QPushButton#UploadDialogCloseX:hover { color: %5; }
        QLabel#UploadDialogFieldLabel {
            color: %4;
            font-family: 'Arimo';
            font-size: 13px;
            font-weight: 600;
            background: transparent;
        }
        QPushButton#UploadDialogSecondary {
            background: %6;
            border: 1px solid %2;
            border-radius: 8px;
            color: %3;
            font-family: 'Arimo';
            font-size: 13px;
            font-weight: 600;
            padding: 0 14px;
        }
        QPushButton#UploadDialogSecondary:hover { background: %7; }
        QPushButton#UploadDialogSecondary:disabled {
            color: %4;
            background: transparent;
        }
        QLabel#UploadDialogProbeStatus {
            color: %4;
            font-family: 'Arimo';
            font-size: 12px;
            font-weight: 500;
            background: transparent;
            padding: 0 4px;
        }
        QTreeWidget#UploadDialogTree {
            background: %1;
            border: 1px solid %2;
            border-radius: 8px;
            color: %3;
            font-family: 'Arimo';
            font-size: 13px;
            outline: none;
            padding: 4px;
        }
        QTreeWidget#UploadDialogTree::item {
            padding: 8px 4px;
            min-height: 28px;
            border: none;
        }
        QTreeWidget#UploadDialogTree::item:hover { background: %7; }
        QTreeWidget#UploadDialogTree::item:disabled { color: %4; }
        QTreeWidget#UploadDialogTree::indicator {
            width: 18px;
            height: 18px;
        }
        QTreeWidget#UploadDialogTree::indicator:unchecked {
            border: 2px solid %11;
            border-radius: 4px;
            background: %6;
        }
        QTreeWidget#UploadDialogTree::indicator:checked {
            border: 2px solid %8;
            border-radius: 4px;
            background: %8;
        }
        QTreeWidget#UploadDialogTree::indicator:disabled {
            border: 2px solid %2;
            border-radius: 4px;
            background: transparent;
        }
        QHeaderView::section {
            background: %6;
            border: none;
            border-bottom: 1px solid %2;
            color: %4;
            font-family: 'Arimo';
            font-size: 12px;
            font-weight: 600;
            padding: 6px 4px;
        }
        QLabel#UploadDialogTreeStatus {
            color: %5;
            font-family: 'Arimo';
            font-size: 13px;
            font-weight: 600;
            background: transparent;
            padding: 8px;
        }
        QLabel#UploadDialogSummary {
            color: %3;
            font-family: 'Arimo';
            font-size: 13px;
            font-weight: 600;
            background: transparent;
        }
        QWidget#UploadDialogProgressBlock {
            background: transparent;
        }
        QLabel#UploadDialogProgressCaption {
            color: %3;
            font-family: 'Arimo';
            font-size: 13px;
            font-weight: 700;
            background: transparent;
        }
        QLabel#UploadDialogProgressDetail {
            color: %4;
            font-family: 'Arimo';
            font-size: 12px;
            font-weight: 500;
            background: transparent;
        }
        QProgressBar#UploadDialogProgressBar {
            background: %6;
            border: 1px solid %2;
            border-radius: 8px;
            color: %3;
            font-family: 'Arimo';
            font-size: 11px;
            font-weight: 600;
            text-align: center;
        }
        QProgressBar#UploadDialogProgressBar::chunk {
            background: %8;
            border-radius: 7px;
        }
        QPushButton#UploadDialogPrimaryBtn {
            background: %8;
            border: none;
            border-radius: 8px;
            color: #FFFFFF;
            font-family: 'Arimo';
            font-size: 14px;
            font-weight: 700;
            padding: 0 18px;
        }
        QPushButton#UploadDialogPrimaryBtn:hover { background: %9; }
        QPushButton#UploadDialogPrimaryBtn:disabled {
            background: %6;
            color: %4;
        }
        QPushButton#UploadDialogPauseBtn {
            background: #B45309;
            border: none;
            border-radius: 8px;
            color: #FFFBEB;
            font-family: 'Arimo';
            font-size: 14px;
            font-weight: 700;
            padding: 0 18px;
        }
        QPushButton#UploadDialogPauseBtn:hover { background: #D97706; }
        QPushButton#UploadDialogStopBtn {
            background: transparent;
            border: 1px solid %5;
            border-radius: 8px;
            color: %5;
            font-family: 'Arimo';
            font-size: 14px;
            font-weight: 700;
            padding: 0 18px;
        }
        QPushButton#UploadDialogStopBtn:hover {
            background: rgba(239,68,68,0.12);
        }
        QPushButton#UploadDialogCancelBtn {
            background: transparent;
            border: 1px solid %2;
            border-radius: 8px;
            color: %3;
            font-family: 'Arimo';
            font-size: 14px;
            font-weight: 600;
            padding: 0 18px;
        }
        QPushButton#UploadDialogCancelBtn:hover { background: %7; }
        QFrame#UploadDialogOfflineBanner {
            background: rgba(245,158,11,0.18);
            border: 1px solid rgba(245,158,11,0.55);
            border-radius: 8px;
        }
        QLabel#UploadDialogOfflineBannerLabel {
            color: %10;
            font-family: 'Arimo';
            font-size: 13px;
            font-weight: 600;
            background: transparent;
        }
    )CSS")
                  .arg(dialog_bg, border, text, muted, t.danger,
                       control_bg, row_hover, t.accent, t.accent_hover)
                  .arg(t.warning)
                  .arg(dark_mode_ ? QStringLiteral("#E4E4E7")
                                  : QStringLiteral("#3F3F46")));

    Q_UNUSED(row_alt);

    if (header_) {
        auto* shadow = qobject_cast<QGraphicsDropShadowEffect*>(header_->graphicsEffect());
        if (!shadow) {
            shadow = new QGraphicsDropShadowEffect(this);
            shadow->setBlurRadius(28);
            shadow->setOffset(0, 8);
            shadow->setColor(QColor(0, 0, 0, dark_mode_ ? 160 : 80));
            // Apply to the whole frame so the drop shadow sits behind
            // the rounded card. Effects on QFrame parent the right way.
            if (auto* card = findChild<QFrame*>(QStringLiteral("UploadDialogCard"))) {
                card->setGraphicsEffect(shadow);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Show / close
// ---------------------------------------------------------------------------

void UploadDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    // The operator is looking at the dialog now. The card notice has
    // done its job. A running upload has no notice to clear, and
    // clearing one would wipe the progress subtitle.
    if (!(runner_ && runner_->isBusy())) {
        clearPendingNotice();
    }
    refreshHeaderSubtitle();
    refreshButtonStates();
    armConnectivityProbes();
    // A running upload keeps the listing and the progress panel the
    // operator hid. A fresh walk would clear the tree underneath it.
    if (runner_ && runner_->isBusy()) {
        return;
    }
    // Kick a probe each time the dialog opens. Operators expect a fresh
    // listing every time they hit the dashboard quick-action. (Arming
    // the drive watcher may already have started one via
    // onDriveStateChanged — don't walk the stick twice.)
    if (!probe_in_progress_) {
        startProbe();
    }
}

void UploadDialog::closeEvent(QCloseEvent* event) {
    cancelCloudCheck(false);
    if (runner_ && runner_->isBusy()) {
        // Hide, don't pause. The runner is a child of this dialog and
        // the dialog stays alive on the shell, so the process keeps
        // uploading. The drive watcher stays armed: pulling the stick
        // still pauses. The cloud probe stops — leaving it running is
        // what used to treat "dialog hidden" as "cloud down".
        stopCloudProbe();
        // A stale failure count must not turn the first curl after
        // reopen into "cloud down" and pause the upload we just hid.
        cloud_probe_failures_ = 0;
        if (reachability_probe_) reachability_probe_->disarm();
    } else {
        disarmConnectivityProbes();
    }
    // QDialog::closeEvent calls reject() while the dialog is still
    // visible. The flag stops that from calling close() again.
    in_close_ = true;
    QDialog::closeEvent(event);
    in_close_ = false;
}

void UploadDialog::reject() {
    if (in_close_ || shutting_down_) {
        QDialog::reject();
        return;
    }
    close();
}

void UploadDialog::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && header_ &&
        header_->geometry().contains(event->pos())) {
        dragging_ = true;
        // QMouseEvent::globalPosition() is Qt 6; this codebase targets
        // Qt 5.15 so we use globalPos() (QPoint, not QPointF).
        drag_offset_ = event->globalPos() - frameGeometry().topLeft();
        event->accept();
        return;
    }
    QDialog::mousePressEvent(event);
}

void UploadDialog::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_ && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPos() - drag_offset_);
        event->accept();
        return;
    }
    QDialog::mouseMoveEvent(event);
}

void UploadDialog::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && dragging_) {
        dragging_ = false;
        event->accept();
        return;
    }
    QDialog::mouseReleaseEvent(event);
}

// ---------------------------------------------------------------------------
// Probe
// ---------------------------------------------------------------------------

void UploadDialog::startProbe() {
    cancelCloudCheck(false);
    if (isThumbDrive()) {
        if (!drive_mounted_ || drive_mount_.isEmpty()) {
            const QString label = drive_watcher_ ? drive_watcher_->label()
                                                 : QString::fromLatin1(
                                                       ThumbDriveWatcher::kDefaultLabel);
            const QString msg =
                drive_state_ == ThumbDriveWatcher::State::PresentUnmounted
                    ? QStringLiteral("%1 drive detected — mounting…").arg(label)
                    : QStringLiteral("Insert the %1 thumb drive.").arg(label);
            if (lbl_probe_status_) lbl_probe_status_->setText(msg);
            if (lbl_tree_status_) {
                lbl_tree_status_->setText(msg);
                lbl_tree_status_->setVisible(true);
            }
            if (tree_) tree_->clear();
            all_targets_.clear();
            refreshSelectionSummary();
            refreshButtonStates();
            return;
        }
    } else {
        if (probe_in_progress_) {
            return;
        }
        if (remote_host_.isEmpty() || ssh_user_.isEmpty()) {
            if (lbl_probe_status_) {
                lbl_probe_status_->setText(
                    QStringLiteral("No robot connection — set robot in Setup."));
            }
            return;
        }
    }
    if (tree_) {
        // Keep the operator's ticks across the rescan (a finished queue
        // re-probes the stick); repopulateTree re-ticks the rows that are
        // still selectable.
        for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
            QTreeWidgetItem* row = tree_->topLevelItem(i);
            if (row && row->checkState(0) == Qt::Checked) {
                sticky_checked_.insert(row->data(0, kBuildingKeyRole).toString());
            }
        }
    }
    probe_in_progress_ = true;
    last_probe_error_.clear();
    const QString scanning = QStringLiteral("Scanning %1…").arg(sourceNoun());
    if (lbl_probe_status_) lbl_probe_status_->setText(scanning);
    if (lbl_tree_status_) {
        lbl_tree_status_->setText(scanning);
        lbl_tree_status_->setVisible(true);
    }
    if (tree_) tree_->clear();
    refreshButtonStates();
    probe_->setSource(source_);
    if (isThumbDrive()) {
        probe_->setDataRoot(drive_mount_);
    } else {
        probe_->setDataRoot(data_root_);
        probe_->setRemote(remote_host_, ssh_user_);
    }
    probe_->start();
}

void UploadDialog::onProbeReady(bool ok, const QList<UploadTarget>& targets,
                                const QString& error) {
    probe_in_progress_ = false;

    if (!ok) {
        last_probe_error_ = error;
        if (lbl_probe_status_) {
            lbl_probe_status_->setText(
                QStringLiteral("Scan failed."));
        }
        if (lbl_tree_status_) {
            lbl_tree_status_->setText(
                QStringLiteral("Could not read the %1:\n%2")
                    .arg(sourceNoun(), error));
            lbl_tree_status_->setVisible(true);
        }
        if (tree_) tree_->clear();
        all_targets_.clear();
        refreshSelectionSummary();
        refreshButtonStates();
        return;
    }

    all_targets_.clear();
    ++probe_serial_;
    for (const UploadTarget& t : targets) {
        if (!all_targets_.contains(t.run_id)) {
            all_targets_.insert(t.run_id, t);
        }
    }
    repopulateTree();
    refreshSelectionSummary();

    if (lbl_probe_status_) {
        if (all_targets_.isEmpty()) {
            lbl_probe_status_->setText(
                QStringLiteral("No scans recorded yet."));
        } else {
            lbl_probe_status_->setText(scansFoundText(listedBuildingCount()));
        }
    }
    refreshButtonStates();
    maybeStartCloudCheck();
}

void UploadDialog::onRefreshClicked() {
    if (isThumbDrive() && drive_watcher_) {
        // Re-evaluate the mount first; the state-change slot re-probes
        // if the root moved, otherwise fall through to a plain rescan.
        drive_watcher_->poll();
        if (probe_in_progress_) return;
    }
    startProbe();
}

QString UploadDialog::formatWhen(const QDateTime& when,
                                const QString& date_folder) const {
    if (when.isValid()) {
        return QLocale().toString(when, QStringLiteral("MMM d, yyyy  h:mm AP"));
    }
    return date_folder.isEmpty() ? QStringLiteral("—") : date_folder;
}

void UploadDialog::repopulateTree() {
    if (!tree_) return;
    QSignalBlocker blocker(tree_);
    tree_->clear();

    if (all_targets_.isEmpty()) {
        if (lbl_tree_status_) {
            lbl_tree_status_->setText(
                last_probe_error_.isEmpty()
                    ? QStringLiteral("No scans to upload.")
                    : QStringLiteral("Could not read the %1:\n%2")
                          .arg(sourceNoun(), last_probe_error_));
            lbl_tree_status_->setVisible(true);
        }
        return;
    }
    if (lbl_tree_status_) lbl_tree_status_->setVisible(false);

    const QList<UploadBuildingRow> rows = groupUploadTargets(all_targets_);
    for (const UploadBuildingRow& view : rows) {
        auto* row = new QTreeWidgetItem(tree_);
        row->setText(0, view.building_label);
        row->setData(0, kBuildingKeyRole, view.key);
        row->setText(1, view.operator_name.isEmpty() ? QStringLiteral("—")
                                                     : view.operator_name);
        row->setText(2, formatWhen(view.newest, view.newest_date_folder));
        row->setText(3, formatBytes(view.total_bytes));
        row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
        row->setCheckState(0, Qt::Unchecked);
        applyBuildingRow(row, view, false);
        if (view.selectable && sticky_checked_.contains(view.key)) {
            row->setCheckState(0, Qt::Checked);
        }
    }
    sticky_checked_.clear();
}

void UploadDialog::applyBuildingRow(QTreeWidgetItem* row,
                                   const UploadBuildingRow& view,
                                   bool tick_if_missing) {
    if (!row) return;
    row->setText(4, view.badge_text);
    row->setForeground(4, badgeColor(view.badge, dark_mode_));
    for (int column = 0; column < 5; ++column) {
        row->setToolTip(column, view.tooltip);
    }

    QSignalBlocker blocker(tree_);
    // While an upload runs the list stays scrollable (tooltips show each
    // folder's status), but ticks cannot change: the running queue was
    // fixed when Upload was pressed.
    const bool locked = runner_ && runner_->isBusy();
    const Qt::ItemFlags enabled =
        locked ? ((row->flags() | Qt::ItemIsEnabled) & ~Qt::ItemIsUserCheckable)
               : (row->flags() | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
    const Qt::ItemFlags disabled = (row->flags() & ~Qt::ItemIsEnabled);
    if (!view.selectable) {
        if (row->flags() != disabled) row->setFlags(disabled);
        if (row->checkState(0) != Qt::Unchecked) row->setCheckState(0, Qt::Unchecked);
        return;
    }
    if (row->flags() != enabled) row->setFlags(enabled);
    if (tick_if_missing && view.badge == UploadRowBadge::Missing &&
        row->checkState(0) != Qt::Checked) {
        row->setCheckState(0, Qt::Checked);
    }
}

void UploadDialog::refreshRowForTarget(const UploadTarget& target,
                                      bool tick_if_missing) {
    const QString key = uploadBuildingKey(target);
    QTreeWidgetItem* row = findBuildingRow(key);
    if (!row) return;
    for (const UploadBuildingRow& view : groupUploadTargets(all_targets_)) {
        if (view.key == key) {
            applyBuildingRow(row, view, tick_if_missing);
            return;
        }
    }
}

void UploadDialog::relockRows() {
    if (!tree_) return;
    QHash<QString, UploadBuildingRow> by_key;
    for (const UploadBuildingRow& view : groupUploadTargets(all_targets_)) {
        by_key.insert(view.key, view);
    }
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* row = tree_->topLevelItem(i);
        if (!row) continue;
        const auto it = by_key.constFind(row->data(0, kBuildingKeyRole).toString());
        if (it != by_key.constEnd()) applyBuildingRow(row, *it, false);
    }
}

QTreeWidgetItem* UploadDialog::findBuildingRow(const QString& key) const {
    if (!tree_) return nullptr;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* row = tree_->topLevelItem(i);
        if (row && row->data(0, kBuildingKeyRole).toString() == key) return row;
    }
    return nullptr;
}

void UploadDialog::onTreeItemChanged(QTreeWidgetItem* /*item*/, int /*column*/) {
    refreshSelectionSummary();
    refreshButtonStates();
}

void UploadDialog::onSelectAllClicked() {
    if (!tree_) return;
    QSignalBlocker blocker(tree_);
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* row = tree_->topLevelItem(i);
        if (row && (row->flags() & Qt::ItemIsEnabled)) {
            row->setCheckState(0, Qt::Checked);
        }
    }
    refreshSelectionSummary();
    refreshButtonStates();
}

UploadDialog::Selection UploadDialog::currentSelection() const {
    Selection out;
    if (!tree_) return out;
    QHash<QString, UploadBuildingRow> by_key;
    for (const UploadBuildingRow& row : groupUploadTargets(all_targets_)) {
        by_key.insert(row.key, row);
    }
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = tree_->topLevelItem(i);
        if (!item || item->checkState(0) != Qt::Checked) continue;
        const auto it = by_key.constFind(item->data(0, kBuildingKeyRole).toString());
        if (it == by_key.constEnd()) continue;
        const QList<UploadTarget> folders = uploadTargetsForRow(all_targets_, *it);
        if (folders.isEmpty()) continue;
        ++out.buildings;
        out.folders += folders;
    }
    return out;
}

int UploadDialog::listedBuildingCount() const {
    return groupUploadTargets(all_targets_).size();
}

void UploadDialog::refreshSelectionSummary() {
    const Selection sel = currentSelection();
    qint64 size = 0;
    int files = 0;
    for (const UploadTarget& folder : sel.folders) {
        size += folder.total_bytes;
        files += folder.total_files;
    }
    if (!lbl_selection_summary_) return;
    if (sel.buildings == 0) {
        lbl_selection_summary_->setText(QStringLiteral("No buildings selected."));
    } else {
        lbl_selection_summary_->setText(
            QStringLiteral("Selected: %1 building%2 · %3 folder%4 · %5 files · %6")
                .arg(sel.buildings)
                .arg(sel.buildings == 1 ? QString() : QStringLiteral("s"))
                .arg(sel.folders.size())
                .arg(sel.folders.size() == 1 ? QString() : QStringLiteral("s"))
                .arg(files)
                .arg(formatBytes(size)));
    }
}

namespace {

/// The scan/copy/profile gate applies only once the runner is idle.
/// While an upload is running the button is hidden and asking the
/// gate would read "a scan is active" over a dialog we opened on purpose.
QString idleUploadGate(bool busy, const std::function<QString()>& gate) {
    if (busy || !gate) return {};
    return gate();
}

}  // namespace

void UploadDialog::setUploadBlockReason(std::function<QString()> reason) {
    upload_block_reason_ = std::move(reason);
}

void UploadDialog::clearPendingNotice() {
    if (background_hold_.isEmpty()) return;
    background_hold_.clear();
    publishBackgroundActivity();
    if (!isVisible() && !(runner_ && runner_->isBusy()) && drive_watcher_) {
        drive_watcher_->stop();
    }
}

void UploadDialog::refreshButtonStates() {
    const bool busy = runner_ && runner_->isBusy();
    const bool any_selected = currentSelection().buildings > 0;
    const bool any_targets = !all_targets_.isEmpty();
    const bool has_source = isThumbDrive() ? drive_mounted_
                                           : !remote_host_.isEmpty();
    const bool has_creds = !cloud_client_id_.isEmpty() &&
                           !cloud_device_token_.isEmpty();
    const bool online = isFullyOnline();
    const QString blocked = idleUploadGate(busy, upload_block_reason_);

    if (btn_upload_) {
        btn_upload_->setEnabled(blocked.isEmpty() && !busy && any_selected &&
                                has_source && has_creds && online);
        btn_upload_->setVisible(!busy);
        if (!blocked.isEmpty()) {
            btn_upload_->setToolTip(blocked);
        } else if (!online && any_selected && has_source && has_creds) {
            btn_upload_->setToolTip(
                QStringLiteral("Upload disabled — see banner for connectivity status."));
        } else {
            btn_upload_->setToolTip(QString());
        }
    }
    if (btn_pause_) {
        btn_pause_->setVisible(busy);
        btn_pause_->setEnabled(busy);
    }
    if (btn_cancel_) {
        btn_cancel_->setVisible(busy);
        btn_cancel_->setEnabled(busy);
    }
    if (btn_close_footer_) {
        btn_close_footer_->setEnabled(true);
        btn_close_footer_->setText(
            busy ? QStringLiteral("Hide (keeps uploading)")
                 : QStringLiteral("Close"));
    }
    if (btn_select_all_) {
        btn_select_all_->setEnabled(!busy && any_targets);
    }
    if (btn_refresh_) {
        btn_refresh_->setEnabled(!busy && !probe_in_progress_);
    }
    if (tree_) {
        // Never disable the tree: a disabled QTreeWidget cannot scroll,
        // and a long upload is when the operator wants to look through
        // the list. applyBuildingRow drops the checkbox instead.
        tree_->setEnabled(true);
        if (busy != rows_locked_) {
            rows_locked_ = busy;
            relockRows();
        }
    }

    if (busy) {
        if (lbl_probe_status_) lbl_probe_status_->setText(QString());
    } else {
        // Restore probe summary when idle.
        if (lbl_probe_status_ && !probe_in_progress_ &&
            !all_targets_.isEmpty()) {
            lbl_probe_status_->setText(scansFoundText(listedBuildingCount()));
        }
    }
}

void UploadDialog::refreshHeaderSubtitle() {
    if (!lbl_subtitle_) return;
    const QString robot = robot_id_.isEmpty() ? QStringLiteral("(no robot)")
                                              : robot_id_;
    const QString creds = cloud_client_id_.isEmpty()
                              ? QStringLiteral("creds missing")
                              : QStringLiteral("client %1").arg(cloud_client_id_);
    QString text = QStringLiteral("Robot: %1 · %2").arg(robot, creds);

    if (isThumbDrive()) {
        // Every upload from this dialog lands under `robot_id`'s S3 key,
        // so the header names the drive AND the robot it is assumed to
        // belong to. One stick per robot is the fleet rule.
        const QString label = drive_watcher_ ? drive_watcher_->label()
                                             : QString::fromLatin1(
                                                   ThumbDriveWatcher::kDefaultLabel);
        if (drive_mounted_) {
            QString drive = QStringLiteral("Drive: %1 at %2").arg(label, drive_mount_);
            const qint64 free = drive_watcher_ ? drive_watcher_->freeBytes() : -1;
            if (free >= 0) {
                drive += QStringLiteral(" · %1 free").arg(formatBytes(free));
            }
            text += QStringLiteral("\n") + drive;
        } else {
            text += QStringLiteral("\nDrive: %1 — not detected").arg(label);
        }
    }
    lbl_subtitle_->setText(text);
}

void UploadDialog::resetProgress() {
    active_files_done_ = 0;
    active_files_total_ = 0;
    if (progress_bar_) {
        progress_bar_->setRange(0, 100);
        progress_bar_->setValue(0);
        progress_bar_->setFormat(QStringLiteral("%p%"));
    }
    if (lbl_progress_caption_) lbl_progress_caption_->setText(QString());
    if (lbl_progress_detail_) lbl_progress_detail_->setText(QString());
    if (progress_block_) progress_block_->setVisible(false);
}

QString UploadDialog::formatBytes(qint64 bytes) const {
    if (bytes < 0) return QStringLiteral("—");
    if (bytes == 0) return QStringLiteral("0 B");
    const double KB = 1024.0;
    const double MB = KB * 1024.0;
    const double GB = MB * 1024.0;
    const double TB = GB * 1024.0;
    QLocale loc;
    double v = static_cast<double>(bytes);
    if (v >= TB) return QStringLiteral("%1 TB").arg(loc.toString(v / TB, 'f', 2));
    if (v >= GB) return QStringLiteral("%1 GB").arg(loc.toString(v / GB, 'f', 2));
    if (v >= MB) return QStringLiteral("%1 MB").arg(loc.toString(v / MB, 'f', 1));
    if (v >= KB) return QStringLiteral("%1 KB").arg(loc.toString(v / KB, 'f', 1));
    return QStringLiteral("%1 B").arg(static_cast<qint64>(v));
}

// ---------------------------------------------------------------------------
// Upload action
// ---------------------------------------------------------------------------

void UploadDialog::onUploadClicked() {
    const QString blocked = idleUploadGate(runner_ && runner_->isBusy(),
                                           upload_block_reason_);
    if (!blocked.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Upload unavailable"), blocked);
        refreshButtonStates();
        return;
    }
    const Selection sel = currentSelection();
    if (sel.folders.isEmpty()) return;
    // Stop the check before launch. Rows already marked missing keep
    // their selection; the copies in `sel` still carry force_reupload.
    cancelCloudCheck(false);
    if (cloud_client_id_.isEmpty() || cloud_device_token_.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Upload Data"),
                             QStringLiteral(
                                 "Cloud credentials are missing for this robot. "
                                 "Update robots.json with cloud_client_id and "
                                 "cloud_device_token before uploading."));
        return;
    }
    if (!runner_) return;
    if (isThumbDrive() && UploadRunner::resolveLocalScriptPath().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Upload Data"),
                             QStringLiteral(
                                 "uploader.py is missing from this laptop "
                                 "(expected /usr/share/bdr-coverage-planner/). "
                                 "Reinstall the OCU package."));
        return;
    }

    background_hold_.clear();
    if (flush_proc_) {
        // A new queue owns the caption now; the old flush may still
        // finish in the background, it just must not relabel this run.
        QProcess* old = flush_proc_;
        old->disconnect(this);
        connect(old, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                old, &QObject::deleteLater);
        flush_proc_ = nullptr;
    }
    failed_this_run_.clear();
    queue_paused_ = false;
    upload_started_.start();
    last_file_progress_.start();
    runner_->setSource(source_);
    runner_->setQueue(sel.folders);
    building_progress_.reset(sel.folders);
    active_queue_index_ = -1;
    active_queue_total_ = sel.folders.size();
    if (progress_block_) progress_block_->setVisible(true);
    if (lbl_progress_caption_) {
        lbl_progress_caption_->setText(
            QStringLiteral("Preparing upload (%1 folder%2)…")
                .arg(sel.folders.size())
                .arg(sel.folders.size() == 1 ? QString() : QStringLiteral("s")));
    }
    if (lbl_progress_detail_) lbl_progress_detail_->setText(QString());
    if (progress_bar_) {
        progress_bar_->setRange(0, 0);  // indeterminate until first file
        progress_bar_->setValue(0);
    }
    runner_->start();
    refreshButtonStates();
    publishBackgroundActivity();
}

void UploadDialog::onPauseClicked() {
    // Pause appears where Upload was. A second click of a double click,
    // or a remote-desktop click that arrives late, must not pause the
    // upload it just started.
    if (upload_started_.isValid() && upload_started_.elapsed() < 1000) {
        return;
    }
    if (runner_) runner_->requestPause(QStringLiteral("Paused"));
    if (lbl_progress_detail_) {
        lbl_progress_detail_->setText(
            QStringLiteral("Pause requested — waiting for current file to finish…"));
    }
}

void UploadDialog::onCancelClicked() {
    // Mid-upload confirmation (Decision #1).  Cancel kills the SSH
    // process and clears the queue, so a misclick at hour 2 of a
    // 47-section session would force the operator to re-select and
    // re-walk every remaining target. The script's upload_state.json
    // is preserved, so completed files are NOT lost — the cost is
    // re-clicking, not data — but the prompt is still cheap.
    const bool busy = runner_ && runner_->isBusy();
    if (busy) {
        const int rc = BdrMessageBox::question(
            this,
            QStringLiteral("Cancel upload?"),
            QStringLiteral(
                "The active file will stop and the remaining selection "
                "will be discarded. Already-uploaded files stay in the "
                "cloud — your progress is not lost. Continue?"),
            BdrMessageBox::No);
        if (rc != BdrMessageBox::Yes) {
            return;
        }
    }
    // The rescan classifies whatever --force managed to delete and
    // checks the rows that are still Done. Without it the force flag
    // is already cleared and the next Upload no-ops on the old manifest.
    recheck_after_cancel_ = true;
    if (runner_) runner_->requestCancel();
    if (lbl_progress_detail_) {
        lbl_progress_detail_->setText(
            QStringLiteral("Stop requested — aborting current file…"));
    }
}

void UploadDialog::onCloseClicked() {
    close();
}

bool UploadDialog::isUploading() const {
    return runner_ && runner_->isBusy();
}

void UploadDialog::pauseForShutdown() {
    if (!runner_ || !runner_->isBusy()) return;
    // Stops onQueueFinished from rescanning the stick, and onTargetFailed
    // from raising a modal, while this wait is on the stack.
    shutting_down_ = true;
    published_background_status_ = QStringLiteral("Pausing upload…");
    emit backgroundStatusChanged(published_background_status_);
    // A retry wait has no process. requestPause() is a no-op then, and
    // sitting out the 10 s ceiling would freeze quit for nothing.
    if (runner_->isRetryScheduled()) {
        runner_->requestCancel();
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    runner_->requestPause(QStringLiteral("OCU closing"));
    if (runner_->isBusy()) {
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        connect(runner_, &UploadRunner::queueFinished, &loop, &QEventLoop::quit);
        timer.start(kShutdownPauseMs);
        // Process the script's exit, but don't deliver clicks. The OCU is
        // on its way out and a second Upload press here would start another
        // queue under the wait.
        loop.exec(QEventLoop::ExcludeUserInputEvents);

        if (runner_->isBusy()) {
            // The in-flight files did not finish inside the ceiling. The
            // state file already lists everything that completed; the rest
            // upload again from the start on the next resume.
            runner_->requestCancel();
        }
    }
    QApplication::restoreOverrideCursor();
}

void UploadDialog::noteDriveRemoved() {
    const bool first = background_hold_ != kHoldDriveRemoved;
    background_hold_ = kHoldDriveRemoved;
    if (first && !isVisible()) {
        emit uploadAttentionRequested(QStringLiteral(
            "Thumb drive removed. Upload paused — plug it back in and press Upload."));
    }
}

void UploadDialog::publishBackgroundActivity() {
    // The quit wait owns the card text until the process is gone.
    if (shutting_down_) return;
    QString status;
    if (!background_hold_.isEmpty()) {
        status = background_hold_;
    } else if (runner_ && runner_->isBusy()) {
        const int total = building_progress_.total();
        status = total > 0
                     ? QStringLiteral("Uploading %1 · %2 / %3 files")
                           .arg(building_progress_.buildingLabel())
                           .arg(building_progress_.done())
                           .arg(total)
                     : QStringLiteral("Uploading…");
    }
    if (status == published_background_status_) return;
    published_background_status_ = status;
    emit backgroundStatusChanged(status);
}

// ---------------------------------------------------------------------------
// Runner signal handlers
// ---------------------------------------------------------------------------

void UploadDialog::onTargetStarted(int index, int total,
                                   const UploadTarget& target) {
    active_queue_index_ = index;
    active_queue_total_ = total;
    active_files_done_ = target.completed_files;
    active_files_total_ = qMax(target.total_files, target.completed_files);
    building_progress_.targetStarted(target);

    if (lbl_progress_caption_) {
        QString text = QStringLiteral("Uploading %1 · folder %2 of %3")
                           .arg(building_progress_.buildingLabel())
                           .arg(building_progress_.folderNumber())
                           .arg(building_progress_.folderCount());
        if (building_progress_.buildingCount() > 1) {
            text += QStringLiteral(" · building %1 of %2")
                        .arg(building_progress_.buildingNumber())
                        .arg(building_progress_.buildingCount());
        }
        lbl_progress_caption_->setText(text);
    }
    if (lbl_progress_detail_) {
        lbl_progress_detail_->setText(
            isThumbDrive() ? QStringLiteral("Starting uploader… (%1)").arg(target.section_name)
                           : QStringLiteral("Connecting to robot…"));
    }
    if (progress_bar_) progress_bar_->setFormat(QStringLiteral("%v / %m files"));
    showBuildingProgress();
    if (all_targets_.contains(target.run_id)) {
        // The launch already captured force_reupload. A pause and a
        // later Upload press must not pass --force again.
        UploadTarget& cached = all_targets_[target.run_id];
        cached.force_reupload = false;
        cached.cloud_check = CloudCheck::Unchecked;
        cached.status = UploadStatus::Partial;
    }
    refreshRowForTarget(target, false);
    refreshButtonStates();
    publishBackgroundActivity();
}

void UploadDialog::onFileUploaded(const UploadTarget& target,
                                  const QString& relpath) {
    last_file_progress_.start();
    active_files_done_ += 1;
    if (active_files_done_ > active_files_total_) {
        active_files_total_ = active_files_done_;
    }
    building_progress_.fileDone();
    showBuildingProgress();
    if (lbl_progress_detail_) {
        lbl_progress_detail_->setText(
            QStringLiteral("Last: %1").arg(relpath));
    }
    if (all_targets_.contains(target.run_id)) {
        UploadTarget& cached = all_targets_[target.run_id];
        cached.completed_files = active_files_done_;
        if (cached.total_files < active_files_total_) {
            cached.total_files = active_files_total_;
        }
        cached.status = UploadStatus::Partial;
        refreshRowForTarget(target, false);
    }
    publishBackgroundActivity();
}

void UploadDialog::onFileSkipped(const UploadTarget& target,
                                 const QString& relpath) {
    last_file_progress_.start();
    if (lbl_progress_detail_) {
        lbl_progress_detail_->setText(
            QStringLiteral("Skipped (already uploaded): %1").arg(relpath));
    }
    Q_UNUSED(target);
}

void UploadDialog::onLogLine(const QString& line) {
    if (lbl_progress_detail_) lbl_progress_detail_->setText(line);
}

void UploadDialog::onTargetPaused(const UploadTarget& target,
                                  const QString& reason) {
    queue_paused_ = true;
    if (lbl_progress_detail_) {
        lbl_progress_detail_->setText(
            QStringLiteral("Paused: %1").arg(reason));
    }
    if (all_targets_.contains(target.run_id)) {
        UploadTarget& cached = all_targets_[target.run_id];
        if (cached.status != UploadStatus::Done) {
            cached.status = UploadStatus::Partial;
        }
    }
    refreshRowForTarget(target, false);
}

void UploadDialog::onTargetFailed(const UploadTarget& target,
                                  const QString& error) {
    // The launch already marked the folder Partial. Leave that; the
    // rescan after the queue stops replaces it from disk.
    refreshRowForTarget(target, false);
    if (shutting_down_) return;
    // Pulling the stick SIGTERMs the script, which arrives here as a
    // crash. The hold was already set to "drive removed" (or "press
    // Upload to resume" if it came back). A failure modal or the
    // failed-hold would erase that.
    if (background_hold_ == kHoldDriveRemoved ||
        background_hold_ == kHoldResume) {
        if (lbl_progress_detail_) {
            lbl_progress_detail_->setText(background_hold_);
        }
        publishBackgroundActivity();
        return;
    }
    if (lbl_progress_detail_) {
        lbl_progress_detail_->setText(
            QStringLiteral("Failed: %1").arg(error));
    }
    // The folder is over: its files count as processed for the building
    // bar. The runner moves on to the next folder. One summary when the
    // queue ends, instead of a modal per folder stacked over the rest
    // of the upload.
    building_progress_.targetFinished();
    showBuildingProgress();
    QString brief = error;
    brief.remove(QStringLiteral("Unexpected error: "));
    const int nl = brief.indexOf(QLatin1Char('\n'));
    if (nl >= 0) brief.truncate(nl);
    if (brief.size() > 160) brief = brief.left(157) + QStringLiteral("…");
    failed_this_run_.append({QStringLiteral("%1 / %2").arg(target.building_slug,
                                                            target.section_name),
                             brief});
}

void UploadDialog::showBuildingProgress() {
    if (!progress_bar_) return;
    progress_bar_->setRange(0, qMax(1, building_progress_.total()));
    progress_bar_->setValue(qMin(building_progress_.done(), progress_bar_->maximum()));
}

void UploadDialog::onTargetCompleted(const UploadTarget& target) {
    building_progress_.targetFinished();
    showBuildingProgress();
    if (all_targets_.contains(target.run_id)) {
        UploadTarget& cached = all_targets_[target.run_id];
        cached.status = UploadStatus::Done;
        cached.cloud_check = CloudCheck::Unchecked;
        cached.force_reupload = false;
        cached.completed_files = qMax(cached.total_files, active_files_done_);
    }
    // The building row disables only once every folder on it is uploaded.
    refreshRowForTarget(target, false);
}

void UploadDialog::onQueueFinished(bool cancelled) {
    if (shutting_down_) return;
    if (cancelled) {
        background_hold_.clear();
    }
    const bool stopped_early = runner_ && runner_->stoppedEarly();
    const int failed = failed_this_run_.size();
    QString caption;
    if (cancelled) {
        caption = QStringLiteral("Upload cancelled.");
    } else if (!background_hold_.isEmpty() &&
               background_hold_ != kHoldFailed) {
        caption = background_hold_;
    } else if (queue_paused_) {
        caption = QStringLiteral("Upload paused. Press Upload Data to continue.");
    } else if (stopped_early) {
        caption = QStringLiteral("Upload stopped.");
    } else if (failed > 0) {
        caption = QStringLiteral("Upload finished — %1 folder%2 did not upload.")
                      .arg(failed)
                      .arg(failed == 1 ? QString() : QStringLiteral("s"));
    } else {
        caption = QStringLiteral("Upload finished.");
    }
    if (failed > 0 && !isVisible()) {
        background_hold_ = kHoldFailed;
        emit uploadAttentionRequested(
            QStringLiteral("%1 folder%2 did not upload. Open Upload Data.")
                .arg(failed)
                .arg(failed == 1 ? QString() : QStringLiteral("s")));
    }
    if (progress_bar_) {
        // The bar shows what was sent. Only a queue that dispatched every
        // folder without a pause or a failure reads full.
        progress_bar_->setRange(0, qMax(1, building_progress_.total()));
        if (!cancelled && !queue_paused_ && !stopped_early && failed == 0) {
            progress_bar_->setValue(progress_bar_->maximum());
        } else {
            progress_bar_->setValue(
                qMin(building_progress_.done(), progress_bar_->maximum()));
        }
    }
    flushStickThen(caption);
    publishBackgroundActivity();
    if (failed > 0 && isVisible()) {
        QStringList lines;
        constexpr int kShown = 6;
        for (int i = 0; i < qMin(kShown, failed); ++i) {
            lines << QStringLiteral("• %1\n   %2").arg(failed_this_run_.at(i).first,
                                                     failed_this_run_.at(i).second);
        }
        if (failed > kShown) {
            lines << QStringLiteral("… and %1 more").arg(failed - kShown);
        }
        const QString tail =
            stopped_early
                ? QStringLiteral("The other selected folders were not started.")
                : QStringLiteral("The other selected folders uploaded.");
        // Deferred: the rescan below must not wait behind the modal.
        const QString text =
            QStringLiteral("%1 folder%2 did not upload:\n\n%3\n\n%4")
                .arg(failed)
                .arg(failed == 1 ? QString() : QStringLiteral("s"))
                .arg(lines.join(QLatin1Char('\n')), tail);
        QTimer::singleShot(0, this, [this, text]() {
            BdrMessageBox::warning(this, QStringLiteral("Upload"), text);
        });
    }
    // Hidden: the dashboard card is the surface. Let the next open
    // rescan. A drive-removed pause keeps the watcher armed so plugging
    // the stick back can change the card from "drive removed" to
    // "press Upload to resume".
    if (!isVisible()) {
        const bool watch_for_stick =
            background_hold_ == kHoldDriveRemoved ||
            background_hold_ == kHoldResume;
        if (watch_for_stick) {
            stopCloudProbe();
            cloud_probe_failures_ = 0;
            if (reachability_probe_) reachability_probe_->disarm();
        } else {
            disarmConnectivityProbes();
        }
        refreshButtonStates();
        return;
    }
    refreshSelectionSummary();
    refreshButtonStates();
    // Re-probe so any newly Done sections refresh from on-disk truth
    // and partial counters get an authoritative value. Cancel does
    // this too: a killed re-upload has to be classified again.
    const bool recheck = recheck_after_cancel_;
    recheck_after_cancel_ = false;
    if (!cancelled || recheck) {
        startProbe();
    }
}

void UploadDialog::flushStickThen(const QString& final_caption) {
    auto show = [this](const QString& text) {
        if (lbl_progress_caption_) lbl_progress_caption_->setText(text);
    };
    if (!isThumbDrive() || !drive_mounted_ || drive_mount_.isEmpty()) {
        show(final_caption);
        return;
    }
    // The uploader and this dialog write to the stick (upload state,
    // manifests, the robot's re-copy list, OCU logs). Put it on the
    // flash before saying anything that sounds like "done": the
    // operator may pull the stick the moment the caption changes.
    if (flush_proc_) {
        flush_proc_->disconnect(this);
        flush_proc_->kill();
        flush_proc_->deleteLater();
    }
    show(QStringLiteral("Saving to the drive… do not unplug it yet."));
    auto* proc = new QProcess(this);
    flush_proc_ = proc;
    auto* guard = new QTimer(proc);
    guard->setSingleShot(true);
    connect(guard, &QTimer::timeout, proc, [proc]() { proc->kill(); });
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, proc, final_caption](int code, QProcess::ExitStatus status) {
                proc->deleteLater();
                if (flush_proc_ != proc) return;
                flush_proc_ = nullptr;
                const bool ok = status == QProcess::NormalExit && code == 0;
                if (lbl_progress_caption_) {
                    lbl_progress_caption_->setText(
                        ok ? final_caption
                           : final_caption +
                                 QStringLiteral(" The drive is still saving — wait "
                                                "a minute before unplugging it."));
                }
            });
    connect(proc, &QProcess::errorOccurred, this,
            [this, proc, final_caption](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart) return;
                proc->deleteLater();
                if (flush_proc_ != proc) return;
                flush_proc_ = nullptr;
                if (lbl_progress_caption_) lbl_progress_caption_->setText(final_caption);
            });
    guard->start(120000);
    proc->start(QStringLiteral("sync"), {QStringLiteral("-f"), drive_mount_});
}

void UploadDialog::onTargetRetryScheduled(const UploadTarget& target,
                                          int attempt, int wait_ms,
                                          const QString& reason) {
    Q_UNUSED(reason);
    if (lbl_progress_caption_) {
        lbl_progress_caption_->setText(
            QStringLiteral("Connection error — retrying %1 / %2 in %3 s "
                           "(attempt %4 of %5)…")
                .arg(target.building_slug,
                     target.section_name)
                .arg((wait_ms + 500) / 1000)
                .arg(attempt)
                .arg(UploadRunner::kMaxConnectionRetries));
    }
    if (lbl_progress_detail_) {
        lbl_progress_detail_->setText(reason.isEmpty()
                                          ? QStringLiteral("Network blip detected.")
                                          : reason);
    }
}

// ---------------------------------------------------------------------------
// Connectivity gate (Decision #3 + #4)
// ---------------------------------------------------------------------------

void UploadDialog::armConnectivityProbes() {
    if (isThumbDrive()) {
        // Drive presence stands in for robot reachability. `start()`
        // evaluates immediately, so a stick already plugged in flips
        // to Mounted (and kicks the probe) before the dialog paints.
        if (drive_watcher_) drive_watcher_->start();
    } else if (reachability_probe_ && !remote_host_.isEmpty()) {
        // Robot reachability probe — re-arm with the current SSH host.
        reachability_probe_->arm(remote_host_);
    }
    // Cloud connectivity probe — start ticking now (first tick fires
    // after the interval, so do an immediate one too so the banner
    // doesn't sit "checking…" for 2 s on open).
    if (cloud_probe_timer_) {
        cloud_probe_timer_->start();
        QTimer::singleShot(0, this, &UploadDialog::onCloudProbeTick);
    }
    refreshConnectivityBanner();
}

void UploadDialog::stopCloudProbe() {
    if (cloud_probe_timer_) {
        cloud_probe_timer_->stop();
    }
    if (cloud_probe_proc_ && cloud_probe_proc_->state() != QProcess::NotRunning) {
        cloud_probe_proc_->disconnect(this);
        cloud_probe_proc_->kill();
        cloud_probe_proc_->waitForFinished(200);
    }
    if (cloud_probe_proc_) {
        cloud_probe_proc_->deleteLater();
        cloud_probe_proc_ = nullptr;
    }
}

void UploadDialog::disarmConnectivityProbes() {
    if (reachability_probe_) {
        reachability_probe_->disarm();
    }
    if (drive_watcher_) {
        drive_watcher_->stop();
    }
    stopCloudProbe();
    cloud_probe_failures_ = 0;
    cloud_probe_seen_response_ = false;
    cloud_reachable_ = false;
    robot_reachable_ = false;
    // Drive state is left as the watcher last reported it: the mount
    // does not go away because the dialog closed, and the next
    // showEvent's `start()` re-evaluates and re-emits on change.
    offline_pause_active_ = false;
    last_offline_reason_.clear();
}

void UploadDialog::onReachabilityChanged(
    RobotReachabilityProbe::State /*old_state*/,
    RobotReachabilityProbe::State new_state) {
    robot_reachable_ = (new_state == RobotReachabilityProbe::State::Reachable);
    handleConnectivityTransition();
}

void UploadDialog::onDriveStateChanged(ThumbDriveWatcher::State state,
                                       const QString& mount_path) {
    const bool was_mounted = drive_mounted_;
    const QString old_mount = drive_mount_;
    drive_state_ = state;
    drive_mounted_ = (state == ThumbDriveWatcher::State::Mounted);
    drive_mount_ = drive_mounted_ ? mount_path : QString();

    refreshHeaderSubtitle();
    if (!isThumbDrive()) return;

    // Pauses the runner if the stick vanished mid-upload; refreshes the
    // banner and Upload gating either way. A busy runner keeps its
    // listing — startProbe() clears the tree, and the queue does not
    // need a new walk to finish the files already in flight.
    handleConnectivityTransition();
    if (runner_ && runner_->isBusy()) return;

    if (drive_mounted_ && (!was_mounted || drive_mount_ != old_mount)) {
        // Fresh stick (or a different one): list it.
        startProbe();
    } else if (!drive_mounted_ && was_mounted) {
        // Stick pulled: the listing refers to paths that no longer
        // exist, so drop it rather than let a click launch against them.
        if (runner_ && !runner_->isBusy()) {
            startProbe();   // renders the "Insert the drive" empty state
        }
    } else if (!drive_mounted_) {
        startProbe();       // Absent ↔ PresentUnmounted message update
    }
}

void UploadDialog::onCloudProbeTick() {
    // The scan and stick-copy gate can change while the dialog is open
    // (the Dashboard re-polls the robot's copy every 5 s) and nothing
    // here is told. Re-ask on this tick so Upload does not stay
    // disabled with a reason that no longer holds.
    if (isVisible()) refreshButtonStates();
    if (cloud_api_base_.isEmpty()) {
        cloud_reachable_ = false;
        cloud_probe_seen_response_ = true;
        handleConnectivityTransition();
        return;
    }
    if (cloud_probe_proc_ && cloud_probe_proc_->state() != QProcess::NotRunning) {
        // Previous tick still running — skip this one rather than
        // stack curls. A 4 s timeout means we self-recover next tick.
        return;
    }
    if (cloud_probe_proc_) {
        cloud_probe_proc_->deleteLater();
        cloud_probe_proc_ = nullptr;
    }
    cloud_probe_proc_ = new QProcess(this);
    connect(cloud_probe_proc_,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this](int code, QProcess::ExitStatus s) {
                onCloudProbeFinished(code, static_cast<int>(s));
            });

    QStringList args;
    args << QStringLiteral("--max-time") << QStringLiteral("4")
         << QStringLiteral("--connect-timeout") << QStringLiteral("3")
         << QStringLiteral("-s")
         << QStringLiteral("-o") << QStringLiteral("/dev/null")
         << QStringLiteral("-w") << QStringLiteral("%{http_code}")
         << cloud_api_base_;
    cloud_probe_proc_->setProgram(QStringLiteral("curl"));
    cloud_probe_proc_->setArguments(args);
    cloud_probe_proc_->start();
}

void UploadDialog::onCloudProbeFinished(int exit_code, int /*status*/) {
    cloud_probe_seen_response_ = true;
    bool reachable = false;
    // curl exit code 0 means it got an HTTP response of some kind.
    // Any HTTP status (200, 401, 403, 404 from API Gateway probes
    // without auth) confirms the endpoint is alive.  Non-zero exit
    // means DNS / connect / timeout / TLS — all of which we treat as
    // unreachable.
    if (exit_code == 0) {
        reachable = true;
    }

    if (reachable) {
        cloud_probe_failures_ = 0;
        if (!cloud_reachable_) {
            cloud_reachable_ = true;
            handleConnectivityTransition();
        }
    } else {
        cloud_probe_failures_ += 1;
        // Match RobotReachabilityProbe debounce: 2 consecutive
        // failures → unreachable. Fast recovery on success.
        if (cloud_probe_failures_ >= 2 && cloud_reachable_) {
            cloud_reachable_ = false;
            handleConnectivityTransition();
        } else if (cloud_probe_failures_ >= 2) {
            // Already false; just refresh banner text in case the
            // failure mode changed (timeout vs DNS, etc).
            refreshConnectivityBanner();
        }
    }
}

void UploadDialog::handleConnectivityTransition() {
    const bool now_online = isFullyOnline();
    // The cloud check is a 4 s curl racing the upload for the same
    // uplink; a busy upload can starve it. Files still landing prove the
    // cloud is reachable, and a real outage reaches the uploader as a
    // connection error it retries on its own.
    const bool upload_moving =
        runner_ && runner_->isBusy() && isSourceReady() &&
        last_file_progress_.isValid() && last_file_progress_.elapsed() < 30000;
    if (!now_online && upload_moving) {
        refreshConnectivityBanner();
        refreshButtonStates();
        publishBackgroundActivity();
        return;
    }
    if (!now_online) {
        // A check that cannot finish must not relabel rows. Coming back
        // online retries it (`reschedule`).
        cancelCloudCheck(true);
        // Offline transition.  Pause the runner gracefully if it's
        // mid-flight so it doesn't burn through retries against a
        // dead network.  No auto-resume — operator clicks Upload
        // again when the banner clears.
        if (runner_) runner_->setRetryEnabled(false);
        if (runner_ && runner_->isBusy() && !offline_pause_active_) {
            offline_pause_active_ = true;
            runner_->requestPause((isThumbDrive() && !drive_mounted_)
                                      ? QStringLiteral("Drive removed")
                                      : QStringLiteral("Cloud unreachable"));
            if (lbl_progress_detail_) {
                lbl_progress_detail_->setText(
                    (isThumbDrive() && !drive_mounted_)
                        ? QStringLiteral("Drive removed — pausing safely…")
                        : QStringLiteral("Connectivity lost — pausing safely…"));
            }
            if (isThumbDrive() && !drive_mounted_) {
                noteDriveRemoved();
            }
        }
    } else {
        // Online again.  Re-enable retries; do NOT auto-resume the
        // queue per the locked design decision.
        if (runner_) runner_->setRetryEnabled(true);
        offline_pause_active_ = false;
        maybeStartCloudCheck();
    }
    if (isThumbDrive() && drive_mounted_ &&
        background_hold_ == kHoldDriveRemoved) {
        background_hold_ = kHoldResume;
        // The card now says to press Upload. No need to keep polling
        // until the operator opens the dialog.
        if (!isVisible() && drive_watcher_) drive_watcher_->stop();
    }
    refreshConnectivityBanner();
    refreshButtonStates();
    publishBackgroundActivity();
}

void UploadDialog::refreshConnectivityBanner() {
    if (!offline_banner_ || !offline_banner_label_) return;
    if (isFullyOnline()) {
        offline_banner_->setVisible(false);
        last_offline_reason_.clear();
        return;
    }
    QString msg;
    const bool source_ready = isSourceReady();
    if (isThumbDrive()) {
        const QString label = drive_watcher_ ? drive_watcher_->label()
                                             : QString::fromLatin1(
                                                   ThumbDriveWatcher::kDefaultLabel);
        if (!source_ready) {
            msg = drive_state_ == ThumbDriveWatcher::State::PresentUnmounted
                      ? QStringLiteral("%1 drive detected but not mounted — "
                                       "mounting… If this persists, open it "
                                       "in Files.").arg(label)
                      : QStringLiteral("Insert the %1 thumb drive from the "
                                       "robot.").arg(label);
            if (!cloud_reachable_ && cloud_probe_seen_response_) {
                msg += QStringLiteral(" Cloud is also unreachable.");
            }
        } else if (!cloud_probe_seen_response_) {
            msg = QStringLiteral("Checking cloud connectivity…");
        } else {
            msg = QStringLiteral("Cloud unreachable — uploads paused until the "
                                 "connection returns.");
        }
    } else if (!source_ready && !cloud_reachable_ && cloud_probe_seen_response_) {
        msg = QStringLiteral("Robot and cloud both unreachable — uploads paused.");
    } else if (!source_ready) {
        msg = QStringLiteral("Robot offline — uploads paused until the radio reconnects.");
    } else if (!cloud_probe_seen_response_) {
        msg = QStringLiteral("Checking cloud connectivity…");
    } else {
        msg = QStringLiteral("Cloud unreachable — uploads paused until the connection returns.");
    }
    if (offline_pause_active_) {
        msg += QStringLiteral(" Press Upload again once the banner clears.");
    }
    offline_banner_label_->setText(msg);
    offline_banner_->setVisible(true);
    last_offline_reason_ = msg;
}

// ---------------------------------------------------------------------------
// Cloud check (every finished folder without a valid proof, oldest first)
// ---------------------------------------------------------------------------

void UploadDialog::maybeStartCloudCheck() {
    // Thumb drive only: the manifest is a local file. RobotSsh is the
    // unwired fallback and is not checked.
    if (!isThumbDrive() || !isSourceReady() || !cloud_reachable_) return;
    if (probe_in_progress_) return;
    if (runner_ && runner_->isBusy()) return;
    if (cloud_api_base_.isEmpty() || cloud_client_id_.isEmpty() ||
        cloud_device_token_.isEmpty() || robot_id_.isEmpty()) return;
    if (probe_serial_ == 0 || cloud_check_for_probe_ == probe_serial_) return;

    cloud_check_queue_ = cloudCheckTargets(all_targets_);
    cloud_check_for_probe_ = probe_serial_;
    if (cloud_check_queue_.isEmpty()) return;
    launchNextCloudBatch();
}

void UploadDialog::launchNextCloudBatch() {
    while (!cloud_check_queue_.isEmpty()) {
        if (!isThumbDrive() || !isSourceReady() || !cloud_reachable_) {
            cloud_check_queue_.clear();
            return;
        }
        // Never mix re-checks of proved folders into a batch of unproved
        // ones: one failed request voids its whole batch, and a re-check
        // must not cost a first check its result.
        const bool proved = cloud_check_queue_.first().cloud_check == CloudCheck::Verified;
        int take = 0;
        while (take < kCloudVerifyBatch && take < cloud_check_queue_.size() &&
               (cloud_check_queue_.at(take).cloud_check == CloudCheck::Verified) == proved) {
            ++take;
        }
        const QList<UploadTarget> batch = cloud_check_queue_.mid(0, take);
        cloud_check_queue_ = cloud_check_queue_.mid(take);
        if (startCloudCheck(batch)) return;
    }
}

bool UploadDialog::startCloudCheck(const QList<UploadTarget>& batch) {
    if (!cloud_verifier_) return false;
    ++verify_generation_;
    const int generation = verify_generation_;
    cloud_listings_.clear();
    loaded_manifests_.clear();
    manifests_ready_ = false;
    cloud_check_ids_.clear();

    QList<QPair<QString, QString>> jobs;
    for (const UploadTarget& src : batch) {
        if (!all_targets_.contains(src.run_id)) continue;
        UploadTarget& cached = all_targets_[src.run_id];
        if (cached.status != UploadStatus::Done) continue;
        cached.force_reupload = false;
        cloud_check_ids_ << src.run_id;
        jobs.append(qMakePair(src.run_id,
                              src.data_path + QStringLiteral("/manifest.json")));
        if (cached.cloud_check == CloudCheck::Verified) {
            // Re-check quietly: the row stays "Uploaded · Verified".
            cached.cloud_recheck = true;
            continue;
        }
        cached.cloud_check = CloudCheck::Checking;
        presentCloudRow(cached, false);
    }
    if (cloud_check_ids_.isEmpty()) {
        if (cloud_verifier_) cloud_verifier_->cancel();
        return false;
    }

    noteCloudVerify(QStringLiteral("Checking %1 uploaded folder%2 against the cloud…")
                        .arg(cloud_check_ids_.size())
                        .arg(cloud_check_ids_.size() == 1 ? QString()
                                                         : QStringLiteral("s")));
    qInfo("[UploadDialog] cloud-verify: checking %d section(s)",
          cloud_check_ids_.size());
    refreshSelectionSummary();
    refreshButtonStates();

    auto* watcher = new QFutureWatcher<ManifestBatch>(this);
    connect(watcher, &QFutureWatcher<ManifestBatch>::finished, this,
            [this, watcher]() {
                const ManifestBatch batch = watcher->result();
                watcher->deleteLater();
                onManifestsLoaded(batch.generation, batch.by_run);
            });
    const QStringList ids = cloud_check_ids_;
    watcher->setFuture(QtConcurrent::run([generation, jobs]() {
        ManifestBatch batch;
        batch.generation = generation;
        for (const auto& job : jobs) {
            batch.by_run.insert(job.first, loadManifestAt(job.second));
        }
        return batch;
    }));
    cloud_verifier_->verify(robot_id_, ids, generation);
    return true;
}

void UploadDialog::cancelCloudCheck(bool reschedule) {
    cloud_check_queue_.clear();
    bool was_checking = false;
    for (const QString& run_id : cloud_check_ids_) {
        const UploadTarget t = all_targets_.value(run_id);
        if (t.cloud_check == CloudCheck::Checking || t.cloud_recheck) {
            was_checking = true;
            break;
        }
    }
    ++verify_generation_;
    if (cloud_verifier_) cloud_verifier_->cancel();
    revertCheckingRows();
    if (reschedule) cloud_check_for_probe_ = -1;
    if (!was_checking) return;
    // The row badges are back to Uploaded. Don't leave the status line
    // saying the check is still running.
    if (reschedule) {
        noteCloudVerify(QStringLiteral(
            "Cloud check stopped — cloud unreachable."));
    } else {
        noteCloudVerify(scansFoundText(listedBuildingCount()));
    }
}

void UploadDialog::revertCheckingRows() {
    const QStringList ids = cloud_check_ids_;
    for (const QString& run_id : ids) {
        if (!all_targets_.contains(run_id)) continue;
        UploadTarget& cached = all_targets_[run_id];
        // A re-check that could not finish keeps its proof.
        cached.cloud_recheck = false;
        if (cached.cloud_check != CloudCheck::Checking) continue;
        cached.cloud_check = CloudCheck::Unchecked;
        cached.force_reupload = false;
        presentCloudRow(cached, false);
    }
    refreshSelectionSummary();
    refreshButtonStates();
}

void UploadDialog::onCloudRunFinished(int generation, const QString& run_id,
                                      bool http_ok, const QList<CloudObject>& files,
                                      const QString& error) {
    if (generation != verify_generation_) return;
    if (!http_ok) {
        // One failed request means the check is unavailable. Rows that
        // were still "checking" go back to Uploaded; nothing is guessed.
        // Older batches stay queued for the next time the dialog checks.
        cloud_check_queue_.clear();
        const QString line =
            QStringLiteral("cloud-verify %1: unavailable (%2)").arg(run_id, error);
        qInfo("[UploadDialog] %s", qUtf8Printable(line));
        noteCloudVerify(line);
        revertCheckingRows();
        return;
    }
    cloud_listings_.insert(run_id, files);
    if (manifests_ready_ && cloud_listings_.size() >= cloud_check_ids_.size()) {
        // Copy: the last result starts the next batch, which replaces
        // cloud_check_ids_ while this loop is still walking it.
        const QStringList ids = cloud_check_ids_;
        for (const QString& id : ids) applyCloudResult(id);
    }
}

void UploadDialog::onManifestsLoaded(int generation,
                                     const QHash<QString, LoadedManifest>& by_run) {
    if (generation != verify_generation_) return;
    loaded_manifests_ = by_run;
    manifests_ready_ = true;
    if (cloud_listings_.size() < cloud_check_ids_.size()) return;
    // Copy: the last result starts the next batch, which replaces
    // cloud_check_ids_ while this loop is still walking it.
    const QStringList ids = cloud_check_ids_;
    for (const QString& id : ids) applyCloudResult(id);
}

void UploadDialog::applyCloudResult(const QString& run_id) {
    if (!all_targets_.contains(run_id)) return;
    UploadTarget& cached = all_targets_[run_id];
    if (cached.status != UploadStatus::Done) return;
    if (cached.cloud_check != CloudCheck::Checking && !cached.cloud_recheck) return;
    const bool recheck = cached.cloud_recheck;
    cached.cloud_recheck = false;
    // Inconclusive results leave a re-checked folder as it was.
    const CloudCheck inconclusive = recheck ? CloudCheck::Verified : CloudCheck::Unchecked;

    const LoadedManifest manifest = loaded_manifests_.value(run_id);
    QString summary;
    if (!manifest.ok) {
        cached.cloud_check = inconclusive;
        cached.force_reupload = false;
        summary = QStringLiteral("unavailable (%1)")
                      .arg(manifest.error.isEmpty()
                               ? QStringLiteral("manifest unreadable")
                               : manifest.error);
        presentCloudRow(cached, false);
    } else if (!manifestMatchesLogin(manifest.client_id, manifest.robot_id,
                                    cloud_client_id_, robot_id_)) {
        // Listing this under the logged-in robot came back empty or
        // different because the objects live in another prefix. Leave
        // the row Uploaded so Upload cannot move them.
        cached.cloud_check = inconclusive;
        cached.force_reupload = false;
        summary = QStringLiteral("skipped (belongs to %1 / %2)")
                      .arg(manifest.client_id.isEmpty() ? QStringLiteral("unknown client")
                                                       : manifest.client_id,
                           manifest.robot_id.isEmpty() ? QStringLiteral("unknown robot")
                                                      : manifest.robot_id);
        presentCloudRow(cached, false);
    } else {
        const CloudCompareResult result = compareManifestWithCloud(
            manifest.files, cloud_listings_.value(run_id));
        summary = cloudVerifySummary(result);
        if (result.matches) {
            const QString client = manifest.client_id.isEmpty() ? cloud_client_id_
                                                                : manifest.client_id;
            const QString robot = manifest.robot_id.isEmpty() ? robot_id_
                                                              : manifest.robot_id;
            // The hash is of the manifest file, and the rename finishes
            // before this returns, so a later batch and a stick unplug
            // both see the proof.
            if (settleCloudVerified(cached.data_path, true, client, robot,
                                    manifest.files.size())) {
                cached.cloud_check = CloudCheck::Verified;
                cached.force_reupload = false;
                presentCloudRow(cached, false);
            } else {
                cached.cloud_check = inconclusive;
                cached.force_reupload = false;
                summary = QStringLiteral(
                    "matched, but the stick could not record the cloud check");
                presentCloudRow(cached, false);
            }
        } else {
            if (!settleCloudVerified(cached.data_path, false, QString(), QString(), 0)) {
                qWarning("[UploadDialog] cloud-verify %s: cloud_verified.json "
                         "is still on the stick",
                         qUtf8Printable(run_id));
            }
            cached.cloud_check = CloudCheck::Missing;
            cached.force_reupload = true;
            presentCloudRow(cached, true);
        }
    }
    const QString line = QStringLiteral("cloud-verify %1: %2").arg(run_id, summary);
    qInfo("[UploadDialog] %s", qUtf8Printable(line));
    noteCloudVerify(line);

    int pending = 0;
    for (const QString& id : cloud_check_ids_) {
        if (!all_targets_.contains(id)) continue;
        const UploadTarget& t = all_targets_[id];
        if (t.cloud_check == CloudCheck::Checking || t.cloud_recheck) ++pending;
    }
    if (pending == 0 && !cloud_check_queue_.isEmpty()) {
        noteCloudVerify(QStringLiteral("Checking older scans against the cloud…"));
        refreshSelectionSummary();
        refreshButtonStates();
        launchNextCloudBatch();
        return;
    }
    // The queue is empty here. Count the whole stick, not this batch:
    // an older folder marked missing must not vanish behind a later
    // batch that all matched.
    if (pending == 0) {
        int missing_all = 0;
        int verified_all = 0;
        int done = 0;
        for (const UploadTarget& target : all_targets_) {
            if (target.status != UploadStatus::Done) continue;
            ++done;
            if (target.cloud_check == CloudCheck::Missing) ++missing_all;
            else if (target.cloud_check == CloudCheck::Verified) ++verified_all;
        }
        if (missing_all > 0) {
            noteCloudVerify(QStringLiteral("Cloud check: %1 section%2 missing in cloud.")
                                .arg(missing_all)
                                .arg(missing_all == 1 ? QString() : QStringLiteral("s")));
        } else if (done > 0 && verified_all == done) {
            noteCloudVerify(QStringLiteral("Cloud check: %1 section%2 verified.")
                                .arg(verified_all)
                                .arg(verified_all == 1 ? QString() : QStringLiteral("s")));
        }
    }
    refreshSelectionSummary();
    refreshButtonStates();
}

void UploadDialog::presentCloudRow(const UploadTarget& target, bool tick_if_missing) {
    refreshRowForTarget(target, tick_if_missing);
}

void UploadDialog::noteCloudVerify(const QString& line) {
    if (runner_ && runner_->isBusy()) return;
    if (lbl_probe_status_) lbl_probe_status_->setText(line);
}

}  // namespace f2c_cpp
