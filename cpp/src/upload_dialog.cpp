/**
 * @file upload_dialog.cpp
 * @brief Implementation of UploadDialog.
 */

#include "upload_dialog.hpp"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDebug>
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

#include "components/bdr_message_box.hpp"
#include "ui_theme_constants.hpp"

namespace f2c_cpp {

namespace {

constexpr int kRunIdRole = Qt::UserRole + 1;
constexpr int kIsBuildingRole = Qt::UserRole + 2;

QString statusBadgeText(UploadStatus s) {
    switch (s) {
        case UploadStatus::Done:    return QStringLiteral("Uploaded");
        case UploadStatus::Partial: return QStringLiteral("Partial");
        case UploadStatus::None:    return QStringLiteral("Pending");
    }
    return QString();
}

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

UploadDialog::UploadDialog(QWidget* parent) : QDialog(parent) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::Dialog);
    setAttribute(Qt::WA_TranslucentBackground);
    setModal(true);
    setMinimumSize(880, 620);

    probe_ = new UploadStateProbe(this);
    runner_ = new UploadRunner(this);

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
    if (runner_ && runner_->isBusy()) {
        runner_->requestCancel();
    }
    disarmConnectivityProbes();
}

void UploadDialog::setDarkMode(bool dark) {
    dark_mode_ = dark;
    applyStyle();
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
    if (probe_) probe_->setDataRoot(data_root_);
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

    auto* lbl_date = new QLabel(QStringLiteral("Date"), card);
    lbl_date->setObjectName("UploadDialogFieldLabel");
    filter_row->addWidget(lbl_date, 0);

    combo_date_ = new QComboBox(card);
    combo_date_->setObjectName("UploadDialogDateCombo");
    combo_date_->setMinimumWidth(220);
    combo_date_->setMinimumHeight(34);
    connect(combo_date_,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &UploadDialog::onDateChanged);
    filter_row->addWidget(combo_date_, 0);

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
    tree_->setColumnCount(4);
    tree_->setHeaderLabels({QStringLiteral("Section"),
                            QStringLiteral("Size"),
                            QStringLiteral("Files"),
                            QStringLiteral("Status")});
    tree_->setRootIsDecorated(true);
    tree_->setUniformRowHeights(false);
    tree_->setSelectionMode(QAbstractItemView::NoSelection);
    tree_->setFocusPolicy(Qt::NoFocus);
    tree_->header()->setStretchLastSection(true);
    tree_->setColumnWidth(0, 320);
    tree_->setColumnWidth(1, 110);
    tree_->setColumnWidth(2, 80);
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
        QComboBox#UploadDialogDateCombo {
            background: %6;
            border: 1px solid %2;
            border-radius: 8px;
            color: %3;
            font-family: 'Arimo';
            font-size: 13px;
            padding: 4px 10px;
        }
        QComboBox#UploadDialogDateCombo::drop-down {
            border: none;
            width: 22px;
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
            padding: 6px 4px;
            border: none;
        }
        QTreeWidget#UploadDialogTree::item:hover { background: %7; }
        QTreeWidget#UploadDialogTree::item:disabled { color: %4; }
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
                       control_bg, row_hover, t.accent, t.accent_hover,
                       t.warning));

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
    refreshHeaderSubtitle();
    refreshButtonStates();
    armConnectivityProbes();
    // Kick a probe each time the dialog opens. Operators expect a fresh
    // listing every time they hit the dashboard quick-action.
    startProbe();
}

void UploadDialog::closeEvent(QCloseEvent* event) {
    if (runner_ && runner_->isBusy()) {
        // Graceful pause per the design spec — the script will finish
        // its in-flight files, write upload_state.json, and exit.
        runner_->requestPause();
    }
    disarmConnectivityProbes();
    QDialog::closeEvent(event);
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
    probe_in_progress_ = true;
    last_probe_error_.clear();
    if (lbl_probe_status_) {
        lbl_probe_status_->setText(QStringLiteral("Scanning robot…"));
    }
    if (lbl_tree_status_) {
        lbl_tree_status_->setText(QStringLiteral("Scanning robot…"));
        lbl_tree_status_->setVisible(true);
    }
    if (tree_) tree_->clear();
    refreshButtonStates();
    probe_->setDataRoot(data_root_);
    probe_->setRemote(remote_host_, ssh_user_);
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
                QStringLiteral("Could not reach the robot:\n%1").arg(error));
            lbl_tree_status_->setVisible(true);
        }
        if (tree_) tree_->clear();
        all_targets_.clear();
        ordered_dates_.clear();
        rebuildDateCombo();
        refreshSelectionSummary();
        refreshButtonStates();
        return;
    }

    all_targets_.clear();
    QStringList dates_in_order;
    for (const UploadTarget& t : targets) {
        if (!all_targets_.contains(t.run_id)) {
            all_targets_.insert(t.run_id, t);
        }
        if (!dates_in_order.contains(t.date_folder)) {
            dates_in_order.append(t.date_folder);
        }
    }
    // Show newest dates first. `Month_DD_YYYY` strings don't sort lex
    // by date, so we just reverse insertion order — the SSH probe walks
    // in name order, dates are typically a small set, and operators
    // care about the most recent ones at the top of the dropdown.
    std::reverse(dates_in_order.begin(), dates_in_order.end());
    ordered_dates_ = dates_in_order;
    rebuildDateCombo();
    repopulateTree();
    refreshSelectionSummary();

    if (lbl_probe_status_) {
        if (all_targets_.isEmpty()) {
            lbl_probe_status_->setText(
                QStringLiteral("No scans recorded yet."));
        } else {
            lbl_probe_status_->setText(
                QStringLiteral("Found %1 sections across %2 dates.")
                    .arg(all_targets_.size())
                    .arg(ordered_dates_.size()));
        }
    }
    refreshButtonStates();
}

void UploadDialog::onRefreshClicked() { startProbe(); }

void UploadDialog::onDateChanged(int /*index*/) { repopulateTree(); }

void UploadDialog::rebuildDateCombo() {
    if (!combo_date_) return;
    QSignalBlocker blocker(combo_date_);
    combo_date_->clear();
    for (const QString& d : ordered_dates_) {
        combo_date_->addItem(d);
    }
    if (!ordered_dates_.isEmpty()) {
        combo_date_->setCurrentIndex(0);
    }
}

void UploadDialog::repopulateTree() {
    if (!tree_) return;
    QSignalBlocker blocker(tree_);
    tree_->clear();

    const QString date = combo_date_ ? combo_date_->currentText() : QString();
    if (date.isEmpty() || all_targets_.isEmpty()) {
        if (lbl_tree_status_) {
            lbl_tree_status_->setText(
                last_probe_error_.isEmpty()
                    ? QStringLiteral("No sections to upload for this date.")
                    : QStringLiteral("Could not reach the robot:\n%1")
                          .arg(last_probe_error_));
            lbl_tree_status_->setVisible(true);
        }
        return;
    }

    // Group by building.
    QMap<QString, QList<UploadTarget>> by_building;
    for (const UploadTarget& t : all_targets_) {
        if (t.date_folder == date) {
            by_building[t.building_slug].append(t);
        }
    }
    if (by_building.isEmpty()) {
        if (lbl_tree_status_) {
            lbl_tree_status_->setText(
                QStringLiteral("No sections to upload for this date."));
            lbl_tree_status_->setVisible(true);
        }
        return;
    }
    if (lbl_tree_status_) lbl_tree_status_->setVisible(false);

    auto building_keys = by_building.keys();
    std::sort(building_keys.begin(), building_keys.end());
    for (const QString& building : building_keys) {
        auto* parent_item = new QTreeWidgetItem(tree_);
        parent_item->setText(0, building);
        parent_item->setData(0, kIsBuildingRole, true);
        parent_item->setFlags(parent_item->flags() | Qt::ItemIsAutoTristate |
                              Qt::ItemIsUserCheckable);
        parent_item->setCheckState(0, Qt::Unchecked);

        QList<UploadTarget> children = by_building.value(building);
        std::sort(children.begin(), children.end(),
                  [](const UploadTarget& a, const UploadTarget& b) {
                      return a.section_name < b.section_name;
                  });

        qint64 total_size = 0;
        int total_files = 0;
        for (const UploadTarget& t : children) {
            auto* row = new QTreeWidgetItem(parent_item);
            row->setText(0, t.section_name);
            row->setData(0, kRunIdRole, t.run_id);
            row->setData(0, kIsBuildingRole, false);
            row->setText(1, formatBytes(t.total_bytes));
            row->setText(2, QString::number(t.total_files));
            setSectionRowStatus(row, t);
            row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
            if (t.status == UploadStatus::Done) {
                // Re-uploads are a script no-op, so we disable the
                // checkbox to discourage accidental redundant traffic.
                row->setFlags(row->flags() & ~Qt::ItemIsEnabled);
                row->setCheckState(0, Qt::Unchecked);
            } else {
                row->setCheckState(0, Qt::Unchecked);
            }
            total_size += t.total_bytes;
            total_files += t.total_files;
        }
        parent_item->setText(1, formatBytes(total_size));
        parent_item->setText(2, QString::number(total_files));
        parent_item->setExpanded(true);
    }
}

void UploadDialog::setSectionRowStatus(QTreeWidgetItem* row,
                                       const UploadTarget& target) {
    if (!row) return;
    QString status_text = statusBadgeText(target.status);
    if (target.status == UploadStatus::Partial && target.total_files > 0) {
        const int pct = qBound(0, target.completed_files * 100 / qMax(1, target.total_files), 100);
        status_text = QStringLiteral("Partial · %1%").arg(pct);
    }
    row->setText(3, status_text);
    QColor color;
    switch (target.status) {
        case UploadStatus::Done:    color = QColor("#10B981"); break;
        case UploadStatus::Partial: color = QColor("#F59E0B"); break;
        case UploadStatus::None:    color = dark_mode_ ? QColor("#A1A1AA")
                                                       : QColor("#52525B"); break;
    }
    row->setForeground(3, color);
}

QTreeWidgetItem* UploadDialog::findSectionRow(const UploadTarget& target) const {
    if (!tree_) return nullptr;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* parent = tree_->topLevelItem(i);
        for (int j = 0; j < parent->childCount(); ++j) {
            QTreeWidgetItem* row = parent->child(j);
            if (row->data(0, kRunIdRole).toString() == target.run_id) {
                return row;
            }
        }
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
        QTreeWidgetItem* parent = tree_->topLevelItem(i);
        for (int j = 0; j < parent->childCount(); ++j) {
            QTreeWidgetItem* row = parent->child(j);
            if (row->flags() & Qt::ItemIsEnabled) {
                row->setCheckState(0, Qt::Checked);
            }
        }
        parent->setCheckState(0, Qt::Checked);
    }
    refreshSelectionSummary();
    refreshButtonStates();
}

QList<UploadTarget> UploadDialog::selectedTargets() const {
    QList<UploadTarget> out;
    if (!tree_) return out;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* parent = tree_->topLevelItem(i);
        for (int j = 0; j < parent->childCount(); ++j) {
            QTreeWidgetItem* row = parent->child(j);
            if (row->checkState(0) != Qt::Checked) continue;
            const QString run_id = row->data(0, kRunIdRole).toString();
            if (!all_targets_.contains(run_id)) continue;
            const UploadTarget& t = all_targets_.value(run_id);
            // Done items are disabled in the tree, so this branch only
            // hits None/Partial — but keep the guard so a future change
            // to the disable rule doesn't accidentally re-PUT.
            if (t.status == UploadStatus::Done) continue;
            out.append(t);
        }
    }
    return out;
}

void UploadDialog::refreshSelectionSummary() {
    const QList<UploadTarget> sel = selectedTargets();
    qint64 size = 0;
    int files = 0;
    for (const UploadTarget& t : sel) {
        size += t.total_bytes;
        files += t.total_files;
    }
    if (!lbl_selection_summary_) return;
    if (sel.isEmpty()) {
        lbl_selection_summary_->setText(
            QStringLiteral("No sections selected."));
    } else {
        lbl_selection_summary_->setText(
            QStringLiteral("Selected: %1 section%2 · %3 files · %4")
                .arg(sel.size())
                .arg(sel.size() == 1 ? QString() : QStringLiteral("s"))
                .arg(files)
                .arg(formatBytes(size)));
    }
}

void UploadDialog::refreshButtonStates() {
    const bool busy = runner_ && runner_->isBusy();
    const bool any_selected = !selectedTargets().isEmpty();
    const bool any_targets = !all_targets_.isEmpty();
    const bool has_remote = !remote_host_.isEmpty();
    const bool has_creds = !cloud_client_id_.isEmpty() &&
                           !cloud_device_token_.isEmpty();
    const bool online = isFullyOnline();

    if (btn_upload_) {
        btn_upload_->setEnabled(!busy && any_selected && has_remote &&
                                has_creds && online);
        btn_upload_->setVisible(!busy);
        if (!online && any_selected && has_remote && has_creds) {
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
            busy ? QStringLiteral("Close (pauses)") : QStringLiteral("Close"));
    }
    if (btn_select_all_) {
        btn_select_all_->setEnabled(!busy && any_targets);
    }
    if (btn_refresh_) {
        btn_refresh_->setEnabled(!busy && !probe_in_progress_);
    }
    if (combo_date_) {
        combo_date_->setEnabled(!busy && !probe_in_progress_);
    }
    if (tree_) {
        tree_->setEnabled(!busy);
    }

    if (busy) {
        if (lbl_probe_status_) lbl_probe_status_->setText(QString());
    } else {
        // Restore probe summary when idle.
        if (lbl_probe_status_ && !probe_in_progress_ &&
            !all_targets_.isEmpty()) {
            lbl_probe_status_->setText(
                QStringLiteral("Found %1 sections across %2 dates.")
                    .arg(all_targets_.size())
                    .arg(ordered_dates_.size()));
        }
    }
}

void UploadDialog::refreshHeaderSubtitle() {
    if (!lbl_subtitle_) return;
    QString robot = robot_id_.isEmpty() ? QStringLiteral("(no robot)")
                                        : robot_id_;
    QString creds = cloud_client_id_.isEmpty()
                        ? QStringLiteral("creds missing")
                        : QStringLiteral("client %1")
                              .arg(cloud_client_id_);
    lbl_subtitle_->setText(QStringLiteral("Robot: %1 · %2")
                               .arg(robot, creds));
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
    const QList<UploadTarget> sel = selectedTargets();
    if (sel.isEmpty()) return;
    if (cloud_client_id_.isEmpty() || cloud_device_token_.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Upload Data"),
                             QStringLiteral(
                                 "Cloud credentials are missing for this robot. "
                                 "Update robots.json with cloud_client_id and "
                                 "cloud_device_token before uploading."));
        return;
    }
    if (!runner_) return;

    runner_->setQueue(sel);
    active_queue_index_ = -1;
    active_queue_total_ = sel.size();
    if (progress_block_) progress_block_->setVisible(true);
    if (lbl_progress_caption_) {
        lbl_progress_caption_->setText(
            QStringLiteral("Preparing upload (%1 section%2)…")
                .arg(sel.size())
                .arg(sel.size() == 1 ? QString() : QStringLiteral("s")));
    }
    if (lbl_progress_detail_) lbl_progress_detail_->setText(QString());
    if (progress_bar_) {
        progress_bar_->setRange(0, 0);  // indeterminate until first file
        progress_bar_->setValue(0);
    }
    runner_->start();
    refreshButtonStates();
}

void UploadDialog::onPauseClicked() {
    if (runner_) runner_->requestPause();
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
    if (runner_) runner_->requestCancel();
    if (lbl_progress_detail_) {
        lbl_progress_detail_->setText(
            QStringLiteral("Stop requested — aborting current file…"));
    }
}

void UploadDialog::onCloseClicked() {
    close();
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

    if (lbl_progress_caption_) {
        lbl_progress_caption_->setText(
            QStringLiteral("[%1/%2] Uploading %3 / %4")
                .arg(index + 1).arg(total)
                .arg(target.building_slug, target.section_name));
    }
    if (lbl_progress_detail_) {
        lbl_progress_detail_->setText(
            QStringLiteral("Connecting to robot…"));
    }
    if (progress_bar_) {
        progress_bar_->setRange(0, qMax(1, active_files_total_));
        progress_bar_->setValue(active_files_done_);
        progress_bar_->setFormat(QStringLiteral("%v / %m files"));
    }
    if (auto* row = findSectionRow(target)) {
        UploadTarget t = target;
        t.status = UploadStatus::Partial;
        setSectionRowStatus(row, t);
    }
    refreshButtonStates();
}

void UploadDialog::onFileUploaded(const UploadTarget& target,
                                  const QString& relpath) {
    active_files_done_ += 1;
    if (active_files_done_ > active_files_total_) {
        active_files_total_ = active_files_done_;
        if (progress_bar_) {
            progress_bar_->setRange(0, active_files_total_);
        }
    }
    if (progress_bar_) progress_bar_->setValue(active_files_done_);
    if (lbl_progress_detail_) {
        lbl_progress_detail_->setText(
            QStringLiteral("Last: %1").arg(relpath));
    }
    if (auto* row = findSectionRow(target)) {
        UploadTarget& cached = all_targets_[target.run_id];
        cached.completed_files = active_files_done_;
        if (cached.total_files < active_files_total_) {
            cached.total_files = active_files_total_;
        }
        cached.status = UploadStatus::Partial;
        setSectionRowStatus(row, cached);
    }
}

void UploadDialog::onFileSkipped(const UploadTarget& target,
                                 const QString& relpath) {
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
    if (lbl_progress_detail_) {
        lbl_progress_detail_->setText(
            QStringLiteral("Paused: %1").arg(reason));
    }
    if (auto* row = findSectionRow(target)) {
        UploadTarget& cached = all_targets_[target.run_id];
        if (cached.status != UploadStatus::Done) {
            cached.status = UploadStatus::Partial;
        }
        setSectionRowStatus(row, cached);
    }
}

void UploadDialog::onTargetFailed(const UploadTarget& target,
                                  const QString& error) {
    if (lbl_progress_detail_) {
        lbl_progress_detail_->setText(
            QStringLiteral("Failed: %1").arg(error));
    }
    if (auto* row = findSectionRow(target)) {
        // Keep on-disk classification accurate. None stays None; Partial
        // stays Partial. Failure doesn't downgrade past what the script
        // observed in upload_state.json.
        const UploadTarget& cached = all_targets_.value(target.run_id, target);
        setSectionRowStatus(row, cached);
    }
    QMessageBox::warning(this,
                         QStringLiteral("Upload failed"),
                         QStringLiteral(
                             "Section %1 / %2 failed:\n\n%3\n\n"
                             "Other selected sections were not started. "
                             "Press Refresh, then try again.")
                             .arg(target.building_slug,
                                  target.section_name,
                                  error));
}

void UploadDialog::onTargetCompleted(const UploadTarget& target) {
    if (auto* row = findSectionRow(target)) {
        UploadTarget& cached = all_targets_[target.run_id];
        cached.status = UploadStatus::Done;
        cached.completed_files = qMax(cached.total_files, active_files_done_);
        setSectionRowStatus(row, cached);
        // Disable the row now that it's done; this matches the "Done
        // items are unselectable" rule established in repopulateTree().
        row->setFlags(row->flags() & ~Qt::ItemIsEnabled);
        row->setCheckState(0, Qt::Unchecked);
    }
}

void UploadDialog::onQueueFinished(bool cancelled) {
    if (lbl_progress_caption_) {
        lbl_progress_caption_->setText(
            cancelled ? QStringLiteral("Upload cancelled.")
                      : QStringLiteral("Upload finished."));
    }
    if (progress_bar_) {
        progress_bar_->setRange(0, qMax(1, active_files_total_));
        progress_bar_->setValue(progress_bar_->maximum());
    }
    refreshSelectionSummary();
    refreshButtonStates();
    // Re-probe so any newly Done sections refresh from on-disk truth
    // and partial counters get an authoritative value.
    if (!cancelled) {
        startProbe();
    }
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
    // Robot reachability probe — re-arm with the current SSH host.
    if (reachability_probe_ && !remote_host_.isEmpty()) {
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

void UploadDialog::disarmConnectivityProbes() {
    if (reachability_probe_) {
        reachability_probe_->disarm();
    }
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
    cloud_probe_failures_ = 0;
    cloud_probe_seen_response_ = false;
    cloud_reachable_ = false;
    robot_reachable_ = false;
    offline_pause_active_ = false;
    last_offline_reason_.clear();
}

void UploadDialog::onReachabilityChanged(
    RobotReachabilityProbe::State /*old_state*/,
    RobotReachabilityProbe::State new_state) {
    robot_reachable_ = (new_state == RobotReachabilityProbe::State::Reachable);
    handleConnectivityTransition();
}

void UploadDialog::onCloudProbeTick() {
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
    if (!now_online) {
        // Offline transition.  Pause the runner gracefully if it's
        // mid-flight so it doesn't burn through retries against a
        // dead network.  No auto-resume — operator clicks Upload
        // again when the banner clears.
        if (runner_) runner_->setRetryEnabled(false);
        if (runner_ && runner_->isBusy() && !offline_pause_active_) {
            offline_pause_active_ = true;
            runner_->requestPause();
            if (lbl_progress_detail_) {
                lbl_progress_detail_->setText(
                    QStringLiteral("Connectivity lost — pausing safely…"));
            }
        }
    } else {
        // Online again.  Re-enable retries; do NOT auto-resume the
        // queue per the locked design decision.
        if (runner_) runner_->setRetryEnabled(true);
        offline_pause_active_ = false;
    }
    refreshConnectivityBanner();
    refreshButtonStates();
}

void UploadDialog::refreshConnectivityBanner() {
    if (!offline_banner_ || !offline_banner_label_) return;
    if (isFullyOnline()) {
        offline_banner_->setVisible(false);
        last_offline_reason_.clear();
        return;
    }
    QString msg;
    if (!robot_reachable_ && !cloud_reachable_ && cloud_probe_seen_response_) {
        msg = QStringLiteral("Robot and cloud both unreachable — uploads paused.");
    } else if (!robot_reachable_) {
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

}  // namespace f2c_cpp
