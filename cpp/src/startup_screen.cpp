#include "startup_screen.hpp"
#include "settings_constants.hpp"

#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include "components/bdr_message_box.hpp"
#include <QNetworkInterface>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QShowEvent>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace f2c_cpp {

namespace {

// Temporary passthrough: allow launching the dashboard without running diagnostics.
constexpr bool kEnableLaunchDashboardPassthrough = true;

QString trimmed(const QString& s) {
    return s.trimmed();
}

}  // namespace

StartupScreen::StartupScreen(QWidget* parent)
    : QWidget(parent) {
    setObjectName("StartupScreenRoot");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    scroll_area_ = new QScrollArea(this);
    scroll_area_->setObjectName("DiagScrollArea");
    scroll_area_->setWidgetResizable(true);
    scroll_area_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    root->addWidget(scroll_area_);

    auto* scroll_content = new QWidget(scroll_area_);
    scroll_content->setObjectName("DiagScrollContent");
    auto* page = new QVBoxLayout(scroll_content);
    page->setContentsMargins(0, 0, 0, 0);
    page->setSpacing(0);

    // ── Header ────────────────────────────────────────────────────────────
    auto* header = new QWidget(scroll_content);
    header->setObjectName("DiagHeader");
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(32, 18, 32, 18);
    header_layout->setSpacing(12);

    auto* header_text = new QVBoxLayout();
    header_text->setContentsMargins(0, 0, 0, 0);
    header_text->setSpacing(4);

    lbl_title_ = new QLabel("Pre-Operation Diagnostics", header);
    lbl_title_->setObjectName("DiagTitle");
    header_text->addWidget(lbl_title_);

    lbl_subtitle_ = new QLabel("System health verification required before operation", header);
    lbl_subtitle_->setObjectName("DiagSubtitle");
    lbl_subtitle_->setWordWrap(true);
    header_text->addWidget(lbl_subtitle_);

    header_layout->addLayout(header_text, 1);

    page->addWidget(header);

    // ── Pre-diagnostic section ───────────────────────────────────────────
    auto* pre_section = new QWidget(scroll_content);
    pre_section->setObjectName("PreDiagSection");
    auto* pre_layout = new QVBoxLayout(pre_section);
    pre_layout->setContentsMargins(32, 16, 32, 16);
    pre_layout->setSpacing(16);

    auto* pre_cards = new QHBoxLayout();
    pre_cards->setContentsMargins(0, 0, 0, 0);
    pre_cards->setSpacing(16);

    // Checklist card
    auto* checklist = new QWidget(pre_section);
    checklist->setObjectName("PreDiagChecklistCard");
    auto* checklist_layout = new QVBoxLayout(checklist);
    checklist_layout->setContentsMargins(20, 20, 20, 20);
    checklist_layout->setSpacing(12);

    auto* checklist_header = new QHBoxLayout();
    checklist_header->setContentsMargins(0, 0, 0, 0);
    checklist_header->setSpacing(8);

    auto* checklist_icon = new QLabel("!", checklist);
    checklist_icon->setObjectName("PreDiagChecklistIcon");
    checklist_icon->setFixedSize(24, 24);
    checklist_icon->setAlignment(Qt::AlignCenter);
    checklist_header->addWidget(checklist_icon, 0, Qt::AlignTop);

    auto* checklist_title = new QLabel("Pre-Diagnostic Checklist", checklist);
    checklist_title->setObjectName("PreDiagChecklistTitle");
    checklist_header->addWidget(checklist_title, 1);
    checklist_layout->addLayout(checklist_header);

    auto addChecklistItem = [&](int number, const QString& text) {
        auto* row = new QHBoxLayout();
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(12);

        auto* num = new QLabel(QString::number(number), checklist);
        num->setObjectName("ChecklistNumber");
        num->setFixedSize(24, 24);
        num->setAlignment(Qt::AlignCenter);

        auto* label = new QLabel(text, checklist);
        label->setObjectName("ChecklistText");
        label->setWordWrap(true);

        row->addWidget(num, 0, Qt::AlignTop);
        row->addWidget(label, 1);
        checklist_layout->addLayout(row);
    };

    addChecklistItem(1, "Ensure Roofus is powered on and in a stable position");
    addChecklistItem(2, "Verify all camera lenses are clean and unobstructed");
    addChecklistItem(3, "Check that RF antenna is properly connected");
    addChecklistItem(4, "Position robot in an open area for GPS signal acquisition");

    pre_cards->addWidget(checklist, 1);

    // Right column: components + important
    auto* right_col = new QWidget(pre_section);
    auto* right_col_layout = new QVBoxLayout(right_col);
    right_col_layout->setContentsMargins(0, 0, 0, 0);
    right_col_layout->setSpacing(16);

    auto* components = new QWidget(right_col);
    components->setObjectName("PreDiagComponentsCard");
    auto* components_layout = new QVBoxLayout(components);
    components_layout->setContentsMargins(20, 20, 20, 20);
    components_layout->setSpacing(12);

    auto* components_title = new QLabel("Components to be Tested", components);
    components_title->setObjectName("PreDiagComponentsTitle");
    components_layout->addWidget(components_title);

    auto* components_grid = new QGridLayout();
    components_grid->setContentsMargins(0, 0, 0, 0);
    components_grid->setHorizontalSpacing(16);
    components_grid->setVerticalSpacing(12);

    auto addComponent = [&](int row, int col, const QString& labelText, const QString& iconText) {
        auto* row_w = new QWidget(components);
        auto* row_l = new QHBoxLayout(row_w);
        row_l->setContentsMargins(0, 0, 0, 0);
        row_l->setSpacing(8);

        auto* icon = new QLabel(iconText, row_w);
        icon->setObjectName("PreDiagComponentIcon");
        icon->setFixedSize(30, 30);
        icon->setAlignment(Qt::AlignCenter);
        row_l->addWidget(icon, 0);

        auto* text = new QLabel(labelText, row_w);
        text->setObjectName("PreDiagComponentText");
        row_l->addWidget(text, 1);

        components_grid->addWidget(row_w, row, col);
    };

    addComponent(0, 0, "RGB Cameras (Left + Right)", "C");
    addComponent(0, 1, "Thermal Camera", "T");
    addComponent(1, 0, "LiDAR Sensor", "L");
    addComponent(1, 1, "RF Link", "R");
    addComponent(2, 0, "GPS Signal", "G");
    addComponent(2, 1, "Drive Motors", "M");

    components_layout->addLayout(components_grid);
    right_col_layout->addWidget(components);

    auto* important = new QWidget(right_col);
    important->setObjectName("PreDiagImportantCard");
    auto* important_layout = new QHBoxLayout(important);
    important_layout->setContentsMargins(16, 12, 16, 12);
    important_layout->setSpacing(12);

    auto* important_icon = new QLabel("!", important);
    important_icon->setObjectName("PreDiagImportantIcon");
    important_icon->setFixedSize(20, 20);
    important_icon->setAlignment(Qt::AlignCenter);
    important_layout->addWidget(important_icon, 0, Qt::AlignTop);

    auto* important_text_col = new QVBoxLayout();
    important_text_col->setContentsMargins(0, 0, 0, 0);
    important_text_col->setSpacing(4);

    auto* important_title = new QLabel("Important", important);
    important_title->setObjectName("PreDiagImportantTitle");
    important_text_col->addWidget(important_title);

    auto* important_text = new QLabel(
        "Diagnostic process takes approximately 3-4 seconds. Do not interrupt the system during testing.",
        important);
    important_text->setObjectName("PreDiagImportantText");
    important_text->setWordWrap(true);
    important_text_col->addWidget(important_text);

    important_layout->addLayout(important_text_col, 1);
    right_col_layout->addWidget(important);

    pre_cards->addWidget(right_col, 1);
    pre_layout->addLayout(pre_cards);

    auto* start_row = new QHBoxLayout();
    start_row->setContentsMargins(0, 0, 0, 0);
    start_row->setSpacing(16);

    btn_start_ = new QPushButton("▶  Start Diagnostics", pre_section);
    btn_start_->setObjectName("DiagStartButton");
    btn_start_->setFixedHeight(48);
    connect(btn_start_, &QPushButton::clicked, this, &StartupScreen::onStartDiagnosticsClicked);
    start_row->addWidget(btn_start_, 1);

    btn_indoor_ = new QPushButton("⌂  Indoor Mode", pre_section);
    btn_indoor_->setObjectName("DiagIndoorButton");
    btn_indoor_->setCheckable(true);
    btn_indoor_->setFixedHeight(48);
    btn_indoor_->setCursor(Qt::PointingHandCursor);
    start_row->addWidget(btn_indoor_, 0, Qt::AlignRight);

    pre_layout->addLayout(start_row);

    auto* indoor_info = new QWidget(pre_section);
    indoor_info->setObjectName("DiagIndoorInfo");
    auto* indoor_info_layout = new QHBoxLayout(indoor_info);
    indoor_info_layout->setContentsMargins(16, 12, 16, 12);
    indoor_info_layout->setSpacing(12);

    auto* info_icon = new QLabel("ⓘ", indoor_info);
    info_icon->setObjectName("DiagIndoorInfoIcon");
    indoor_info_layout->addWidget(info_icon, 0);

    auto* info_text = new QLabel(indoor_info);
    info_text->setObjectName("DiagIndoorInfoText");
    info_text->setTextFormat(Qt::RichText);
    info_text->setText("<b>GPS check will be skipped.</b> This mode is recommended for indoor operations where satellite signals are unavailable.");
    info_text->setWordWrap(true);
    indoor_info_layout->addWidget(info_text, 1);

    indoor_info->setVisible(btn_indoor_->isChecked());
    connect(btn_indoor_, &QPushButton::toggled, indoor_info, &QWidget::setVisible);

    pre_layout->addWidget(indoor_info);

    page->addWidget(pre_section);

    // ── Live Results Section ─────────────────────────────────────────────
    live_results_section_ = new QWidget(scroll_content);
    live_results_section_->setObjectName("LiveResultsSection");
    live_results_section_->setProperty("active", false);

    auto* live_layout = new QVBoxLayout(live_results_section_);
    live_layout->setContentsMargins(0, 0, 0, 0);
    live_layout->setSpacing(0);

    auto* live_header = new QWidget(live_results_section_);
    live_header->setObjectName("LiveResultsHeader");
    auto* live_header_layout = new QVBoxLayout(live_header);
    live_header_layout->setContentsMargins(32, 16, 32, 8);

    auto* live_title = new QLabel("Live Diagnostic Results", live_header);
    live_title->setObjectName("LiveResultsTitle");
    live_header_layout->addWidget(live_title);
    live_layout->addWidget(live_header);

    auto* live_content = new QWidget(live_results_section_);
    live_content->setObjectName("LiveResultsContent");
    auto* live_content_layout = new QHBoxLayout(live_content);
    live_content_layout->setContentsMargins(32, 16, 32, 24);
    live_content_layout->setSpacing(24);

    auto* live_left = new QWidget(live_content);
    live_left->setObjectName("LiveResultsLeft");
    auto* live_left_layout = new QVBoxLayout(live_left);
    live_left_layout->setContentsMargins(0, 0, 0, 0);
    live_left_layout->setSpacing(16);

    auto* section = new QLabel("System Diagnostic Status", live_left);
    section->setObjectName("LiveResultsSectionTitle");
    live_left_layout->addWidget(section);

    auto makeRow = [&](const QString& title,
                       const QString& iconText,
                       QLabel*& out_badge_icon,
                       QLabel*& out_badge_text,
                       QLabel*& out_subtitle) -> QWidget* {
        auto* card = new QWidget(live_left);
        card->setObjectName("LiveResultsCard");
        auto* h = new QHBoxLayout(card);
        h->setContentsMargins(16, 12, 16, 12);
        h->setSpacing(12);

        auto* icon = new QLabel(iconText, card);
        icon->setObjectName("LiveResultsCardIcon");
        icon->setFixedSize(40, 40);
        icon->setAlignment(Qt::AlignCenter);
        h->addWidget(icon, 0, Qt::AlignVCenter);

        auto* text_col = new QVBoxLayout();
        text_col->setContentsMargins(0, 0, 0, 0);
        text_col->setSpacing(2);

        auto* lbl_title = new QLabel(title, card);
        lbl_title->setObjectName("LiveResultsCardTitle");
        text_col->addWidget(lbl_title);

        out_subtitle = new QLabel(card);
        out_subtitle->setObjectName("LiveResultsCardSubtitle");
        out_subtitle->setWordWrap(true);
        text_col->addWidget(out_subtitle);

        h->addLayout(text_col, 1);

        auto* badge = new QWidget(card);
        badge->setObjectName("LiveResultsBadge");
        auto* badge_layout = new QHBoxLayout(badge);
        badge_layout->setContentsMargins(0, 0, 0, 0);
        badge_layout->setSpacing(6);

        out_badge_icon = new QLabel(" ", badge);
        out_badge_icon->setObjectName("LiveResultsBadgeIcon");
        out_badge_icon->setFixedSize(20, 20);
        out_badge_icon->setAlignment(Qt::AlignCenter);
        badge_layout->addWidget(out_badge_icon);

        out_badge_text = new QLabel("PENDING", badge);
        out_badge_text->setObjectName("LiveResultsStatusText");
        badge_layout->addWidget(out_badge_text);

        h->addWidget(badge, 0, Qt::AlignRight | Qt::AlignVCenter);
        return card;
    };

    live_left_layout->addWidget(makeRow("RGB Cameras (Left + Right)", "C", lbl_rgb_icon_, lbl_rgb_status_, lbl_rgb_subtitle_));
    live_left_layout->addWidget(makeRow("Thermal Camera", "T", lbl_thermal_icon_, lbl_thermal_status_, lbl_thermal_subtitle_));
    live_left_layout->addWidget(makeRow("LiDAR Sensor", "L", lbl_lidar_icon_, lbl_lidar_status_, lbl_lidar_subtitle_));
    live_left_layout->addWidget(makeRow("RF Link", "R", lbl_rf_icon_, lbl_rf_status_, lbl_rf_subtitle_));
    gps_card_ = makeRow("GPS Signal", "G", lbl_gps_icon_, lbl_gps_status_, lbl_gps_subtitle_);
    live_left_layout->addWidget(gps_card_);
    live_left_layout->addWidget(makeRow("Drive Motors", "M", lbl_motors_icon_, lbl_motors_status_, lbl_motors_subtitle_));

    auto* overall = new QWidget(live_left);
    overall->setObjectName("LiveResultsOverallCard");
    auto* overall_layout = new QHBoxLayout(overall);
    overall_layout->setContentsMargins(16, 12, 16, 12);
    overall_layout->setSpacing(12);

    auto* overall_title = new QLabel("Overall Status", overall);
    overall_title->setObjectName("LiveResultsOverallTitle");
    overall_layout->addWidget(overall_title, 1);

    auto* overall_badge = new QWidget(overall);
    overall_badge->setObjectName("LiveResultsBadge");
    auto* overall_badge_layout = new QHBoxLayout(overall_badge);
    overall_badge_layout->setContentsMargins(0, 0, 0, 0);
    overall_badge_layout->setSpacing(6);

    lbl_overall_icon_ = new QLabel(" ", overall_badge);
    lbl_overall_icon_->setObjectName("LiveResultsBadgeIcon");
    lbl_overall_icon_->setFixedSize(20, 20);
    lbl_overall_icon_->setAlignment(Qt::AlignCenter);
    overall_badge_layout->addWidget(lbl_overall_icon_);

    lbl_overall_status_ = new QLabel("INITIALIZING", overall_badge);
    lbl_overall_status_->setObjectName("LiveResultsStatusText");
    overall_badge_layout->addWidget(lbl_overall_status_);

    overall_layout->addWidget(overall_badge, 0, Qt::AlignRight | Qt::AlignVCenter);
    live_left_layout->addWidget(overall);

    btn_retry_report_ = new QPushButton("Retry report fetch", live_left);
    btn_retry_report_->setObjectName("DiagRetryReport");
    btn_retry_report_->setFixedHeight(34);
    btn_retry_report_->setVisible(false);
    connect(btn_retry_report_, &QPushButton::clicked, this, &StartupScreen::onRetryFetchReportClicked);
    live_left_layout->addWidget(btn_retry_report_, 0, Qt::AlignLeft);

    live_left_layout->addStretch(1);
    live_content_layout->addWidget(live_left, 3);

    live_results_log_pane_ = new QWidget(live_content);
    live_results_log_pane_->setObjectName("LiveResultsLogPane");
    auto* log_layout = new QVBoxLayout(live_results_log_pane_);
    log_layout->setContentsMargins(16, 16, 16, 16);
    log_layout->setSpacing(12);

    auto* log_title = new QLabel("DIAGNOSTIC LOG", live_results_log_pane_);
    log_title->setObjectName("LiveResultsLogTitle");
    log_layout->addWidget(log_title, 0, Qt::AlignLeft);

    txt_log_ = new QPlainTextEdit(live_results_log_pane_);
    txt_log_->setObjectName("LiveResultsLog");
    txt_log_->setReadOnly(true);
    txt_log_->setMaximumBlockCount(2500);
    log_layout->addWidget(txt_log_, 1);

    live_content_layout->addWidget(live_results_log_pane_, 2);
    live_layout->addWidget(live_content);

    page->addWidget(live_results_section_);

    // ── Footer actions ────────────────────────────────────────────────────
    auto* footer = new QWidget(scroll_content);
    footer->setObjectName("DiagFooter");
    auto* footer_layout = new QHBoxLayout(footer);
    footer_layout->setContentsMargins(32, 16, 32, 16);
    footer_layout->setSpacing(12);

    btn_rerun_ = new QPushButton("Rerun Diagnostics", footer);
    btn_rerun_->setObjectName("DiagRerunButton");
    btn_rerun_->setFixedHeight(44);
    connect(btn_rerun_, &QPushButton::clicked, this, &StartupScreen::onRerunDiagnosticsClicked);
    footer_layout->addWidget(btn_rerun_, 0, Qt::AlignLeft);

    footer_layout->addStretch(1);

    btn_launch_ = new QPushButton("Launch Dashboard", footer);
    btn_launch_->setObjectName("DiagLaunchButton");
    btn_launch_->setFixedHeight(48);
    btn_launch_->setEnabled(false);
    connect(btn_launch_, &QPushButton::clicked, this, &StartupScreen::onLaunchDashboardClicked);
    footer_layout->addWidget(btn_launch_, 0, Qt::AlignRight);

    page->addWidget(footer);
    scroll_area_->setWidget(scroll_content);

    applyLocalStyle();
    resetState();

    // Processes
    preflight_proc_ = new QProcess(this);
    connect(preflight_proc_, &QProcess::readyReadStandardOutput, this, &StartupScreen::onPreflightStdoutReady);
    connect(preflight_proc_, &QProcess::readyReadStandardError, this, &StartupScreen::onPreflightStderrReady);
    connect(preflight_proc_,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            &StartupScreen::onPreflightFinished);
    connect(preflight_proc_, &QProcess::errorOccurred, this, &StartupScreen::onPreflightError);

    report_proc_ = new QProcess(this);
    connect(report_proc_,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            &StartupScreen::onReportFetchFinished);
    connect(report_proc_, &QProcess::errorOccurred, this, &StartupScreen::onReportError);
}

void StartupScreen::setRobotId(const QString& robotId) {
    robot_id_ = robotId.trimmed();
    updateUiState();
}

void StartupScreen::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // Re-apply theme when screen becomes visible so property-based styles
    // resolve correctly (they may not when the widget was hidden in the stack).
    applyLocalStyle();
}

void StartupScreen::setDarkMode(bool dark_mode) {
    if (dark_mode_ == dark_mode) {
        return;
    }
    dark_mode_ = dark_mode;
    applyLocalStyle();
}

void StartupScreen::resetState() {
    // Stop any running processes
    if (preflight_proc_ && preflight_proc_->state() != QProcess::NotRunning) {
        preflight_proc_->kill();
        preflight_proc_->waitForFinished(1000);
    }
    if (report_proc_ && report_proc_->state() != QProcess::NotRunning) {
        report_proc_->kill();
        report_proc_->waitForFinished(1000);
    }

    preflight_running_ = false;
    preflight_completed_ = false;
    overall_status_.clear();
    resetResultsUi();
    setLiveResultsActive(false);
    if (txt_log_) {
        txt_log_->clear();
    }
    if (btn_retry_report_) {
        btn_retry_report_->setVisible(false);
    }
    updateUiState();
}

void StartupScreen::onStartDiagnosticsClicked() {
    startDiagnostics(true);
}

void StartupScreen::onRerunDiagnosticsClicked() {
    startDiagnostics(true);
}

void StartupScreen::startDiagnostics(bool scroll_to_results) {
    if (preflight_running_) {
        return;
    }

    const int reply = BdrMessageBox::question(
        this,
        "Run Diagnostics",
        "This will run the pre-operation diagnostics on the robot.\n\n"
        "It includes a motor motion test and may move the robot.\n\n"
        "Proceed?",
        BdrMessageBox::No);

    if (reply != BdrMessageBox::Yes) {
        return;
    }

    if (scroll_to_results) {
        setLiveResultsActive(true);
        scrollToLiveResults();
    }

    // Reset UI for a new run
    preflight_running_ = true;
    preflight_completed_ = false;
    overall_status_.clear();
    resetResultsUi();
    if (btn_retry_report_) {
        btn_retry_report_->setVisible(false);
    }
    if (txt_log_) {
        txt_log_->clear();
    }

    const QString robot_host = robotHostFromSettings();
    QString local_ip = detectLocalIP();
    if (local_ip.isEmpty()) {
        // Fallback to script default target (still passed explicitly for visibility).
        local_ip = "192.168.168.100";
        appendLog("[warn] Could not detect local IP; using rf_target_ip=192.168.168.100");
    }

    const bool indoor = btn_indoor_ && btn_indoor_->isChecked();

    updateUiState();
    appendLog(QString("[info] Robot: roofus@%1").arg(robot_host));
    appendLog(QString("[info] Starting system diagnostics (rf_target_ip=%1%2)…")
        .arg(local_ip, indoor ? ", indoor mode (skip GPS)" : ""));
    appendLog("[info] Command: ros2 run pilot_control startup_preflight");

    // Build remote command (best-effort env setup on robot)
    QString script = QString(
        "set -e; "
        "if [ -f /opt/ros/humble/setup.bash ]; then source /opt/ros/humble/setup.bash; "
        "elif [ -f /opt/ros/foxy/setup.bash ]; then source /opt/ros/foxy/setup.bash; "
        "elif [ -f /opt/ros/galactic/setup.bash ]; then source /opt/ros/galactic/setup.bash; "
        "fi; "
        "if [ -f \"$HOME/pilot_ws/install/setup.bash\" ]; then source \"$HOME/pilot_ws/install/setup.bash\"; fi; "
        "export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp; export ROS_DOMAIN_ID=0; "
        "ros2 run pilot_control startup_preflight --ros-args "
        "-p rf_target_ip:=%1 "
        "-p skip_motion_test:=false"
        "%2"
    ).arg(local_ip, indoor ? " -p skip_gps_check:=true" : "");

    QString remote_cmd = QString("bash -lc \"%1\"").arg(script.replace("\"", "\\\""));

    if (preflight_proc_) {
        preflight_proc_->setProgram("ssh");
        QStringList args = sshBaseArgs(robot_host);
        args << remote_cmd;
        preflight_proc_->setArguments(args);
        preflight_proc_->start();
    }
}

void StartupScreen::onLaunchDashboardClicked() {
    emit continueRequested();
}

void StartupScreen::applyLocalStyle() {
    setProperty("theme", dark_mode_ ? "dark" : "light");
    setStyleSheet(R"(
        #StartupScreenRoot {
            background-color: #FAFAFA;
            color: #18181B;
            font-family: "Arimo";
        }

        #DiagScrollArea,
        #DiagScrollContent {
            background-color: #FAFAFA;
            border: none;
        }
        #DiagScrollArea QScrollBar:vertical {
            width: 10px;
            border: none;
            border-radius: 5px;
            background: #E4E4E7;
            margin: 0;
        }
        #DiagScrollArea QScrollBar::handle:vertical {
            background: #00BC7D;
            border-radius: 5px;
            min-height: 30px;
        }
        #DiagScrollArea QScrollBar::handle:vertical:hover {
            background: #00D492;
        }
        #DiagScrollArea QScrollBar::add-line:vertical,
        #DiagScrollArea QScrollBar::sub-line:vertical {
            height: 0;
        }
        #DiagScrollArea QScrollBar::add-page:vertical,
        #DiagScrollArea QScrollBar::sub-page:vertical {
            background: none;
        }

        /* Header */
        #DiagHeader {
            background-color: #FFFFFF;
            border-bottom: 1px solid #E4E4E7;
        }
        #DiagTitle {
            font-size: 24px;
            font-weight: 700;
            color: #18181B;
        }
        #DiagSubtitle {
            font-size: 16px;
            color: #71717B;
        }
        #DiagHeaderIcon {
            border: 2px solid #E4E4E7;
            border-radius: 4px;
            color: #9F9FA9;
            font-size: 12px;
        }

        /* Pre-diagnostic section */
        #PreDiagChecklistCard {
            background: #FFFFFF;
            border: 1px solid #E4E4E7;
            border-radius: 10px;
        }
        #PreDiagChecklistIcon {
            background: #00BC7D;
            color: #FFFFFF;
            border-radius: 12px;
            font-weight: 700;
        }
        #PreDiagChecklistTitle {
            font-size: 20px;
            font-weight: 700;
            color: #18181B;
        }
        #ChecklistNumber {
            background: #00BC7D;
            color: #FFFFFF;
            border-radius: 12px;
            font-size: 14px;
            font-weight: 700;
        }
        #ChecklistText {
            font-size: 16px;
            color: #71717B;
        }

        #PreDiagComponentsCard {
            background: #FFFFFF;
            border: 1px solid #E4E4E7;
            border-radius: 10px;
        }
        #PreDiagComponentsTitle {
            font-size: 18px;
            font-weight: 700;
            color: #18181B;
        }
        #PreDiagComponentIcon {
            background: rgba(0, 188, 125, 0.1);
            border: 1px solid #E4E4E7;
            border-radius: 4px;
            color: #00BC7D;
            font-size: 12px;
            font-weight: 700;
        }
        #PreDiagComponentText {
            font-size: 14px;
            color: #71717B;
        }

        #PreDiagImportantCard {
            background: #FEF3C7;
            border: 1px solid #FCD34D;
            border-radius: 10px;
        }
        #PreDiagImportantIcon {
            border: 1px solid #B45309;
            border-radius: 10px;
            color: #B45309;
            font-weight: 700;
        }
        #PreDiagImportantTitle {
            font-size: 14px;
            font-weight: 700;
            color: #92400E;
        }
        #PreDiagImportantText {
            font-size: 14px;
            color: #B45309;
        }

        #DiagIndoorButton {
            font-size: 16px;
            font-weight: 500;
            color: #18181B;
            padding: 12px 20px;
            background: #FFFFFF;
            border: 1px solid #E4E4E7;
            border-radius: 10px;
        }
        #DiagIndoorButton:hover {
            border-color: #00BC7D;
        }
        #DiagIndoorButton:checked {
            background: rgba(0, 188, 125, 0.1);
            border-color: #00BC7D;
        }
        #DiagIndoorInfo {
            background: rgba(0, 188, 125, 0.1);
            border: 1px solid #A7F3D0;
            border-radius: 10px;
        }
        #DiagIndoorInfoIcon {
            font-size: 18px;
            color: #00BC7D;
        }
        #DiagIndoorInfoText {
            font-size: 14px;
            color: #065F46;
            line-height: 20px;
        }
        #DiagStartButton {
            background: #00BC7D;
            border: none;
            border-radius: 10px;
            padding: 12px 16px;
            color: #FFFFFF;
            font-weight: 700;
            font-size: 16px;
        }
        #DiagStartButton:disabled {
            background: #E4E4E7;
            color: #9F9FA9;
        }

        /* Live results */
        #LiveResultsHeader {
            background: #FFFFFF;
            border-top: 1px solid #E4E4E7;
            border-bottom: 1px solid #E4E4E7;
        }
        #LiveResultsTitle {
            font-size: 20px;
            font-weight: 700;
            color: #18181B;
        }
        #LiveResultsSectionTitle {
            font-size: 20px;
            font-weight: 700;
            color: #18181B;
        }
        #LiveResultsCard {
            background: #FFFFFF;
            border: 1px solid #E4E4E7;
            border-radius: 10px;
        }
        #LiveResultsCardIcon {
            background: #F4F4F5;
            border-radius: 10px;
            color: #9F9FA9;
            font-weight: 700;
        }
        #LiveResultsCardTitle {
            font-size: 18px;
            font-weight: 700;
            color: #18181B;
        }
        #LiveResultsCardSubtitle {
            font-size: 13px;
            color: #71717B;
            font-weight: 400;
        }
        #LiveResultsBadgeIcon {
            border-radius: 10px;
            border: 1px solid #9F9FA9;
            color: #9F9FA9;
            font-size: 12px;
            font-weight: 700;
        }
        #LiveResultsStatusText {
            font-size: 16px;
            font-weight: 700;
            color: #9F9FA9;
        }
        #LiveResultsOverallCard {
            background: #FAFAFA;
            border: 2px solid #E4E4E7;
            border-radius: 10px;
        }
        #LiveResultsOverallTitle {
            font-size: 18px;
            font-weight: 700;
            color: #18181B;
        }
        #DiagRetryReport {
            background: transparent;
            border: 1px solid #E4E4E7;
            border-radius: 4px;
            padding: 6px 10px;
            color: #18181B;
            font-weight: 700;
        }
        #DiagRetryReport:hover {
            background: #F4F4F5;
        }

        /* Diagnostic log – light theme (default) */
        #LiveResultsLogPane {
            background: #F4F4F5;
            border: 1px solid #E4E4E7;
        }
        #LiveResultsLogTitle {
            font-family: "Liberation Mono";
            font-size: 14px;
            font-weight: 700;
            color: #18181B;
        }
        #LiveResultsLog {
            background: #FFFFFF;
            border: none;
            color: #71717B;
            font-family: "Liberation Mono";
            font-size: 12px;
        }

        /* Inactive live results – light theme */
        #LiveResultsSection[active="false"] #LiveResultsLogPane {
            background: #F4F4F5;
            border-color: #E4E4E7;
        }
        #LiveResultsSection[active="false"] #LiveResultsLogTitle,
        #LiveResultsSection[active="false"] #LiveResultsLog,
        #LiveResultsSection[active="false"] #LiveResultsCardTitle,
        #LiveResultsSection[active="false"] #LiveResultsSectionTitle,
        #LiveResultsSection[active="false"] #LiveResultsOverallTitle {
            color: #9F9FA9;
        }
        #LiveResultsSection[active="false"] #LiveResultsCardIcon {
            color: #9F9FA9;
        }

        /* Footer */
        #DiagFooter {
            background: #FFFFFF;
            border-top: 1px solid #E4E4E7;
        }
        #DiagRerunButton {
            background: transparent;
            border: 2px solid #E4E4E7;
            border-radius: 4px;
            padding: 8px 14px;
            color: #9F9FA9;
            font-weight: 700;
            font-size: 16px;
        }
        #DiagRerunButton:enabled {
            color: #18181B;
        }
        #DiagRerunButton:hover:enabled {
            background: #F4F4F5;
        }
        #DiagLaunchButton {
            background: #E4E4E7;
            border-radius: 4px;
            padding: 8px 18px;
            color: #9F9FA9;
            font-weight: 700;
            font-size: 16px;
        }
        #DiagLaunchButton:enabled {
            background: #00BC7D;
            color: #FFFFFF;
        }

        /* Dark mode overrides */
        #StartupScreenRoot[theme="dark"] {
            background-color: #09090B;
            color: #FFFFFF;
        }
        #StartupScreenRoot[theme="dark"] #DiagScrollArea,
        #StartupScreenRoot[theme="dark"] #DiagScrollContent {
            background-color: #09090B;
        }
        #StartupScreenRoot[theme="dark"] #DiagScrollArea QScrollBar:vertical {
            background: #27272A;
        }
        #StartupScreenRoot[theme="dark"] #DiagScrollArea QScrollBar::handle:vertical {
            background: #00BC7D;
        }
        #StartupScreenRoot[theme="dark"] #DiagScrollArea QScrollBar::handle:vertical:hover {
            background: #00D492;
        }
        #StartupScreenRoot[theme="dark"] #DiagHeader {
            background-color: #18181B;
            border-bottom: 1px solid #27272A;
        }
        #StartupScreenRoot[theme="dark"] #DiagTitle {
            color: #FFFFFF;
        }
        #StartupScreenRoot[theme="dark"] #DiagSubtitle {
            color: #9F9FA9;
        }
        #StartupScreenRoot[theme="dark"] #DiagHeaderIcon {
            border-color: #27272A;
            color: #9F9FA9;
        }
        #StartupScreenRoot[theme="dark"] #PreDiagChecklistCard,
        #StartupScreenRoot[theme="dark"] #PreDiagComponentsCard,
        #StartupScreenRoot[theme="dark"] #LiveResultsCard {
            background: #18181B;
            border: 1px solid #27272A;
        }
        #StartupScreenRoot[theme="dark"] #PreDiagChecklistTitle,
        #StartupScreenRoot[theme="dark"] #PreDiagComponentsTitle,
        #StartupScreenRoot[theme="dark"] #LiveResultsTitle,
        #StartupScreenRoot[theme="dark"] #LiveResultsSectionTitle,
        #StartupScreenRoot[theme="dark"] #LiveResultsCardTitle,
        #StartupScreenRoot[theme="dark"] #LiveResultsOverallTitle {
            color: #FFFFFF;
        }
        #StartupScreenRoot[theme="dark"] #LiveResultsCardSubtitle {
            color: #9F9FA9;
        }
        #StartupScreenRoot[theme="dark"] #ChecklistText,
        #StartupScreenRoot[theme="dark"] #PreDiagComponentText {
            color: #9F9FA9;
        }
        #StartupScreenRoot[theme="dark"] #DiagIndoorButton {
            background: #18181B;
            border-color: #27272A;
            color: #9F9FA9;
        }
        #StartupScreenRoot[theme="dark"] #DiagIndoorButton:hover {
            border-color: #00BC7D;
        }
        #StartupScreenRoot[theme="dark"] #DiagIndoorButton:checked {
            background: rgba(0, 188, 125, 0.1);
            border-color: #00BC7D;
        }
        #StartupScreenRoot[theme="dark"] #DiagIndoorInfo {
            background: #18181B;
            border-color: #27272A;
        }
        #StartupScreenRoot[theme="dark"] #DiagIndoorInfoIcon {
            color: #00BC7D;
        }
        #StartupScreenRoot[theme="dark"] #DiagIndoorInfoText {
            color: #9F9FA9;
        }
        #StartupScreenRoot[theme="dark"] #PreDiagComponentIcon {
            background: rgba(0, 188, 125, 0.1);
            border-color: #27272A;
            color: #00BC7D;
        }
        #StartupScreenRoot[theme="dark"] #PreDiagImportantCard {
            background: #2b1f0f;
            border: 1px solid #8a5a10;
        }
        #StartupScreenRoot[theme="dark"] #PreDiagImportantTitle {
            color: #fbbf24;
        }
        #StartupScreenRoot[theme="dark"] #PreDiagImportantText {
            color: #f59e0b;
        }
        #StartupScreenRoot[theme="dark"] #LiveResultsHeader {
            background: #18181B;
            border-top: 1px solid #27272A;
            border-bottom: 1px solid #27272A;
        }
        #StartupScreenRoot[theme="dark"] #LiveResultsCardIcon {
            background: #27272A;
        }
        #StartupScreenRoot[theme="dark"] #LiveResultsOverallCard {
            background: #18181B;
            border: 2px solid #27272A;
        }
        #StartupScreenRoot[theme="dark"] #LiveResultsLogPane {
            background: #18181B;
            border-color: #27272A;
        }
        #StartupScreenRoot[theme="dark"] #LiveResultsLogTitle {
            color: #00BC7D;
        }
        #StartupScreenRoot[theme="dark"] #LiveResultsLog {
            background: transparent;
            color: #9F9FA9;
        }
        #StartupScreenRoot[theme="dark"] #LiveResultsSection[active="false"] #LiveResultsLogPane {
            background: #27272A;
        }
        #StartupScreenRoot[theme="dark"] #LiveResultsSection[active="false"] #LiveResultsLogTitle,
        #StartupScreenRoot[theme="dark"] #LiveResultsSection[active="false"] #LiveResultsLog,
        #StartupScreenRoot[theme="dark"] #LiveResultsSection[active="false"] #LiveResultsCardTitle,
        #StartupScreenRoot[theme="dark"] #LiveResultsSection[active="false"] #LiveResultsSectionTitle,
        #StartupScreenRoot[theme="dark"] #LiveResultsSection[active="false"] #LiveResultsOverallTitle {
            color: #71717B;
        }
        #StartupScreenRoot[theme="dark"] #DiagFooter {
            background: #18181B;
            border-top: 1px solid #27272A;
        }
        #StartupScreenRoot[theme="dark"] #DiagRerunButton {
            border-color: #27272A;
            color: #9F9FA9;
        }
        #StartupScreenRoot[theme="dark"] #DiagLaunchButton {
            background: #27272A;
            color: #9F9FA9;
        }
        #StartupScreenRoot[theme="dark"] #DiagLaunchButton:enabled {
            background: #00BC7D;
            color: #FFFFFF;
        }
    )");
}

void StartupScreen::updateUiState() {
    if (btn_start_) {
        btn_start_->setEnabled(!preflight_running_);
    }
    if (btn_rerun_) {
        btn_rerun_->setEnabled(preflight_completed_ && !preflight_running_);
    }

    const QString overall = trimmed(overall_status_).toUpper();
    const bool have_report = preflight_completed_;
    const bool has_fail = (overall == "FAIL");
    // Continue/Launch enabled only when report exists and no FAIL state (PASS + WARN allowed),
    // unless temporary passthrough is enabled.
    const bool ready_to_launch =
        (!preflight_running_) &&
        (kEnableLaunchDashboardPassthrough || (have_report && !has_fail));

    if (btn_launch_) {
        btn_launch_->setEnabled(ready_to_launch);
    }

    if (lbl_overall_icon_ && lbl_overall_status_) {
        if (preflight_running_) {
            applyStatusBadge(lbl_overall_icon_, lbl_overall_status_, "RUNNING");
        } else if (!have_report) {
            applyStatusBadge(lbl_overall_icon_, lbl_overall_status_, "INITIALIZING");
        } else {
            applyStatusBadge(lbl_overall_icon_, lbl_overall_status_, has_fail ? "NOT READY" : "READY");
        }
    }
}

void StartupScreen::setLiveResultsActive(bool active) {
    live_results_active_ = active;

    if (live_results_section_) {
        live_results_section_->setProperty("active", active);
        live_results_section_->style()->unpolish(live_results_section_);
        live_results_section_->style()->polish(live_results_section_);
        live_results_section_->update();
    }
    if (live_results_log_pane_) {
        live_results_log_pane_->setProperty("active", active);
        live_results_log_pane_->style()->unpolish(live_results_log_pane_);
        live_results_log_pane_->style()->polish(live_results_log_pane_);
        live_results_log_pane_->update();
    }
}

void StartupScreen::scrollToLiveResults() {
    if (!scroll_area_ || !live_results_section_) {
        return;
    }
    QTimer::singleShot(0, this, [this]() {
        if (scroll_area_ && live_results_section_) {
            if (auto* bar = scroll_area_->verticalScrollBar()) {
                const int target = live_results_section_->y();
                bar->setValue(target);
            } else {
                scroll_area_->ensureWidgetVisible(live_results_section_, 0, 12);
            }
        }
    });
}

void StartupScreen::onRetryFetchReportClicked() {
    if (preflight_running_) {
        return;
    }
    fetchLatestReport();
}

void StartupScreen::onPreflightStdoutReady() {
    if (!preflight_proc_) return;
    const QByteArray data = preflight_proc_->readAllStandardOutput();
    if (!data.isEmpty()) {
        appendLog(QString::fromUtf8(data));
    }
}

void StartupScreen::onPreflightStderrReady() {
    if (!preflight_proc_) return;
    const QByteArray data = preflight_proc_->readAllStandardError();
    if (!data.isEmpty()) {
        appendLog(QString::fromUtf8(data));
    }
}

void StartupScreen::onPreflightFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    preflight_running_ = false;
    updateUiState();

    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        appendLog(QString("[error] Preflight SSH command failed (exit=%1).").arg(exitCode));
        if (btn_retry_report_) {
            btn_retry_report_->setVisible(false);
        }
        return;
    }

    appendLog("[info] Diagnostics complete. Fetching latest report…");
    fetchLatestReport();
}

void StartupScreen::onPreflightError(QProcess::ProcessError error) {
    Q_UNUSED(error);
    preflight_running_ = false;
    updateUiState();
    const QString msg = preflight_proc_ ? preflight_proc_->errorString() : "unknown error";
    appendLog(QString("[error] Preflight process error: %1").arg(msg));
}

void StartupScreen::fetchLatestReport() {
    const QString robot_host = robotHostFromSettings();
    // Avoid login shells here so stdout is clean JSON.
    const QString remote_cmd = "cat /R_DATA/startup_check/latest/preflight_report.json";

    if (!report_proc_) {
        return;
    }

    if (report_proc_->state() != QProcess::NotRunning) {
        report_proc_->kill();
        report_proc_->waitForFinished(500);
    }

    appendLog("[info] Fetching latest diagnostic report JSON…");
    report_proc_->setProgram("ssh");
    QStringList args = sshBaseArgs(robot_host);
    args << remote_cmd;
    report_proc_->setArguments(args);
    report_proc_->start();
}

void StartupScreen::onReportFetchFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        appendLog(QString("[error] Failed to fetch report JSON over SSH (exit=%1).").arg(exitCode));
        if (btn_retry_report_) {
            btn_retry_report_->setVisible(true);
        }
        preflight_completed_ = false;
        updateUiState();
        return;
    }

    const QByteArray out = report_proc_ ? report_proc_->readAllStandardOutput() : QByteArray();
    if (out.isEmpty()) {
        appendLog("[error] Report fetch returned empty output.");
        if (btn_retry_report_) {
            btn_retry_report_->setVisible(true);
        }
        preflight_completed_ = false;
        updateUiState();
        return;
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(out, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        appendLog(QString("[error] Could not parse report JSON: %1").arg(err.errorString()));
        if (btn_retry_report_) {
            btn_retry_report_->setVisible(true);
        }
        preflight_completed_ = false;
        updateUiState();
        return;
    }

    const QJsonObject obj = doc.object();
    const QJsonObject checks = obj.value("checks").toObject();

    auto statusOf = [&](const QString& key) -> QString {
        const QJsonValue v = checks.value(key);
        if (!v.isObject()) return QString();
        return v.toObject().value("status").toString().trimmed();
    };

    auto strFrom = [&](const QString& key, const QString& field) -> QString {
        const QJsonValue v = checks.value(key);
        if (!v.isObject()) return QString();
        return v.toObject().value(field).toString().trimmed();
    };

    bool has_any_fail = false;

    // RF Link: rf_dbm (negative dBm; higher = stronger). Fallback to status if no metric.
    const QJsonObject rf_check = checks.value("rf").toObject();
    const bool has_rf_dbm = rf_check.contains("rf_dbm");
    QString rf_status, rf_subtitle;
    if (has_rf_dbm) {
        const int rf_dbm = rf_check.value("rf_dbm").toInt(0);
        if (rf_dbm >= -65) {
            rf_status = "PASS";
        } else if (rf_dbm >= -85) {
            rf_status = "WARN";
            rf_subtitle = QString("Signal: %1 dBm").arg(rf_dbm);
        } else {
            rf_status = "FAIL";
            rf_subtitle = QString("Signal: %1 dBm").arg(rf_dbm);
        }
    } else {
        rf_status = statusOf("rf");
        if (rf_status.isEmpty()) rf_status = "PENDING";
    }
    if (rf_status == "FAIL") has_any_fail = true;
    applyStatusBadge(lbl_rf_icon_, lbl_rf_status_, rf_status, lbl_rf_subtitle_, rf_subtitle);

    // GPS Signal: SKIP (indoor mode), or gps_satellites thresholds. When skipped, hide the row.
    const QString gps_json_status = statusOf("gps");
    const QJsonObject gps_check = checks.value("gps").toObject();
    const bool has_gps_satellites = gps_check.contains("gps_satellites");
    QString gps_status, gps_subtitle;

    if (gps_json_status.toUpper() == "SKIP") {
        gps_status = "SKIP";
        if (gps_card_) gps_card_->setVisible(false);
    } else {
        if (gps_card_) gps_card_->setVisible(true);
        if (has_gps_satellites) {
            const int gps_satellites = gps_check.value("gps_satellites").toInt(0);
            if (gps_satellites >= 4) {
                gps_status = "PASS";
            } else if (gps_satellites > 0) {
                gps_status = "WARN";
                gps_subtitle = QString("Minimum 4 required. Current: %1").arg(gps_satellites);
            } else {
                gps_status = "FAIL";
                gps_subtitle = "No signal";
            }
        } else {
            gps_status = gps_json_status.isEmpty() ? "PENDING" : gps_json_status;
        }
    }
    if (gps_status == "FAIL") has_any_fail = true;
    applyStatusBadge(lbl_gps_icon_, lbl_gps_status_, gps_status, lbl_gps_subtitle_, gps_subtitle);

    // Cameras / Thermal: camera_occlusion (from checks.thermal, checks.cameras, or top-level)
    QString thermal_occlusion = strFrom("thermal", "camera_occlusion");
    if (thermal_occlusion.isEmpty()) {
        thermal_occlusion = strFrom("cameras", "camera_occlusion");
    }
    if (thermal_occlusion.isEmpty()) {
        thermal_occlusion = obj.value("camera_occlusion").toString().trimmed();
    }
    QString thermal_status, thermal_subtitle;
    if (!thermal_occlusion.isEmpty()) {
        const QString occ = thermal_occlusion.toLower();
        if (occ == "none") {
            thermal_status = "PASS";
        } else if (occ == "minor") {
            thermal_status = "WARN";
            thermal_subtitle = "Minor occlusion detected";
        } else {
            thermal_status = "FAIL";
            thermal_subtitle = "Major occlusion detected";
        }
        if (thermal_status == "FAIL") has_any_fail = true;
        applyStatusBadge(lbl_thermal_icon_, lbl_thermal_status_, thermal_status, lbl_thermal_subtitle_, thermal_subtitle);
    } else {
        applyStatusBadge(lbl_thermal_icon_, lbl_thermal_status_, statusOf("thermal"));
    }

    // RGB, LiDAR, Motors: use status from JSON (no threshold logic)
    left_rgb_status_ = statusOf("left_rgb");
    right_rgb_status_ = statusOf("right_rgb");
    updateCombinedRgbStatus();
    if (left_rgb_status_ == "FAIL" || right_rgb_status_ == "FAIL") has_any_fail = true;
    applyStatusBadge(lbl_lidar_icon_, lbl_lidar_status_, statusOf("lidar"));
    if (statusOf("lidar") == "FAIL") has_any_fail = true;
    applyStatusBadge(lbl_motors_icon_, lbl_motors_status_, statusOf("motors"));
    if (statusOf("motors") == "FAIL") has_any_fail = true;

    overall_status_ = has_any_fail ? "FAIL" : "READY";
    applyStatusBadge(lbl_overall_icon_, lbl_overall_status_, has_any_fail ? "NOT READY" : "READY");

    preflight_completed_ = true;
    if (btn_retry_report_) {
        btn_retry_report_->setVisible(false);
    }
    appendLog(QString("[info] Report parsed. Overall=%1").arg(overall_status_.isEmpty() ? "UNKNOWN" : overall_status_.toUpper()));
    updateUiState();
}

void StartupScreen::onReportError(QProcess::ProcessError error) {
    Q_UNUSED(error);
    const QString msg = report_proc_ ? report_proc_->errorString() : "unknown error";
    appendLog(QString("[error] Report fetch process error: %1").arg(msg));
    if (btn_retry_report_) {
        btn_retry_report_->setVisible(true);
    }
    preflight_completed_ = false;
    updateUiState();
}

void StartupScreen::appendLog(const QString& text) {
    if (!txt_log_) return;
    QString t = text;
    // Normalize newlines to avoid excessive blank lines when appending chunks.
    t.replace("\r\n", "\n");
    t.replace("\r", "\n");
    const QStringList lines = t.split('\n');
    for (const QString& line : lines) {
        if (line.isEmpty()) continue;
        updateStatusFromLogLine(line);
        txt_log_->appendPlainText(line);
    }
}

void StartupScreen::updateCombinedRgbStatus() {
    QString combined;
    if (left_rgb_status_.isEmpty() || right_rgb_status_.isEmpty()) {
        combined = "PENDING";
    } else {
        combined = combineStatus(left_rgb_status_, right_rgb_status_);
    }
    applyStatusBadge(lbl_rgb_icon_, lbl_rgb_status_, combined);
}

void StartupScreen::updateStatusFromLogLine(const QString& line) {
    // Script logs "[gps] GPS check skipped by parameter" for indoor mode; regex expects "[gps] SKIP"
    if (line.contains("[gps]") && line.contains("skipped", Qt::CaseInsensitive)) {
        applyStatusBadge(lbl_gps_icon_, lbl_gps_status_, "SKIP", lbl_gps_subtitle_, QString());
        if (gps_card_) gps_card_->setVisible(false);
        return;
    }

    static const QRegularExpression status_re(
        R"(\[(left_rgb|right_rgb|thermal|lidar|rf|gps|motors)\]\s+(PASS|WARN|FAIL|SKIP))");

    const QRegularExpressionMatch match = status_re.match(line);
    if (!match.hasMatch()) {
        return;
    }

    const QString key = match.captured(1);
    const QString status = match.captured(2);

    if (key == "left_rgb") {
        left_rgb_status_ = status;
        updateCombinedRgbStatus();
        return;
    }
    if (key == "right_rgb") {
        right_rgb_status_ = status;
        updateCombinedRgbStatus();
        return;
    }
    if (key == "thermal") {
        applyStatusBadge(lbl_thermal_icon_, lbl_thermal_status_, status);
        return;
    }
    if (key == "lidar") {
        applyStatusBadge(lbl_lidar_icon_, lbl_lidar_status_, status);
        return;
    }
    if (key == "rf") {
        applyStatusBadge(lbl_rf_icon_, lbl_rf_status_, status);
        return;
    }
    if (key == "gps") {
        applyStatusBadge(lbl_gps_icon_, lbl_gps_status_, status, lbl_gps_subtitle_, QString());
        if (status.toUpper() == "SKIP" && gps_card_) gps_card_->setVisible(false);
        else if (gps_card_) gps_card_->setVisible(true);
        return;
    }
    if (key == "motors") {
        applyStatusBadge(lbl_motors_icon_, lbl_motors_status_, status);
        return;
    }
}

void StartupScreen::resetResultsUi() {
    overall_status_.clear();
    left_rgb_status_.clear();
    right_rgb_status_.clear();
    applyStatusBadge(lbl_rgb_icon_, lbl_rgb_status_, "PENDING", lbl_rgb_subtitle_, QString());
    applyStatusBadge(lbl_thermal_icon_, lbl_thermal_status_, "PENDING", lbl_thermal_subtitle_, QString());
    applyStatusBadge(lbl_lidar_icon_, lbl_lidar_status_, "PENDING", lbl_lidar_subtitle_, QString());
    applyStatusBadge(lbl_rf_icon_, lbl_rf_status_, "PENDING", lbl_rf_subtitle_, QString());
    applyStatusBadge(lbl_gps_icon_, lbl_gps_status_, "PENDING", lbl_gps_subtitle_, QString());
    applyStatusBadge(lbl_motors_icon_, lbl_motors_status_, "PENDING", lbl_motors_subtitle_, QString());
    applyStatusBadge(lbl_overall_icon_, lbl_overall_status_, "INITIALIZING");
    if (gps_card_) gps_card_->setVisible(true);
}

QString StartupScreen::combineStatus(const QString& a, const QString& b) const {
    const QString sa = trimmed(a).toUpper();
    const QString sb = trimmed(b).toUpper();

    if (sa.isEmpty()) return sb;
    if (sb.isEmpty()) return sa;

    auto has = [&](const QString& v) -> bool { return sa == v || sb == v; };
    if (has("FAIL")) return "FAIL";
    if (has("WARN")) return "WARN";
    if (has("PASS")) return "PASS";
    if (has("SKIP")) return "SKIP";
    if (has("NOT_RUN")) return "NOT_RUN";
    return sa;
}

void StartupScreen::applyStatusBadge(QLabel* icon, QLabel* text, const QString& status,
                                    QLabel* subtitle, const QString& subtitleText) const {
    if (!icon || !text) return;

    QString s = trimmed(status).toUpper();
    if (s == "NOT_READY") {
        s = "NOT READY";
    }

    QString display = s;
    QString color = "#9F9FA9";
    QString glyph = " ";

    if (s.isEmpty() || s == "-" || s == "NOT_RUN") {
        display = "PENDING";
        glyph = " ";
        color = "#9F9FA9";
    } else if (s == "PASS") {
        glyph = "✓";
        color = "#2ECC71";
    } else if (s == "WARN") {
        glyph = "!";
        color = "#FFB020";
    } else if (s == "FAIL") {
        glyph = "×";
        color = "#E74C3C";
    } else if (s == "READY") {
        glyph = "✓";
        color = "#2ECC71";
        display = "READY";
    } else if (s == "NOT READY") {
        glyph = "×";
        color = "#E74C3C";
        display = "NOT READY";
    } else if (s == "RUNNING" || s == "INITIALIZING" || s == "PENDING") {
        glyph = " ";
        color = "#9F9FA9";
        display = (s == "RUNNING") ? "RUNNING..." : s;
    } else if (s == "SKIP") {
        glyph = " ";
        color = "#9F9FA9";
    } else {
        glyph = " ";
        color = "#9F9FA9";
    }

    text->setText(display.isEmpty() ? "-" : display);
    icon->setText(glyph);

    icon->setStyleSheet(QString(
                             "border: 1px solid %1; color: %1; border-radius: 10px;")
                             .arg(color));
    text->setStyleSheet(QString("color: %1; font-weight: 800;").arg(color));

    if (subtitle) {
        subtitle->setText(subtitleText);
        subtitle->setVisible(!subtitleText.isEmpty());
    }
}

QString StartupScreen::detectLocalIP() const {
    // Prefer: 192.168.168.x (RF) > 10.x.x.x (WiFi) > others
    QString rf_ip, wifi_ip, other_ip;

    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        if (iface.flags() & QNetworkInterface::IsLoopBack) continue;
        if (!(iface.flags() & QNetworkInterface::IsUp)) continue;
        if (!(iface.flags() & QNetworkInterface::IsRunning)) continue;

        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol) continue;
            const QString ip = entry.ip().toString();

            if (ip.startsWith("192.168.168.")) {
                rf_ip = ip;
            } else if (ip.startsWith("10.")) {
                wifi_ip = ip;
            } else if (!ip.startsWith("127.")) {
                other_ip = ip;
            }
        }
    }

    if (!rf_ip.isEmpty()) return rf_ip;
    if (!wifi_ip.isEmpty()) return wifi_ip;
    if (!other_ip.isEmpty()) return other_ip;
    return QString();
}

QString StartupScreen::robotHostFromSettings() const {
    QSettings settings(kSettingsOrgName, kSettingsAppName);
    const QString from_settings = settings.value("robot_ip", "").toString().trimmed();
    return from_settings.isEmpty() ? "192.168.168.101" : from_settings;
}

QStringList StartupScreen::sshBaseArgs(const QString& robotHost) const {
    // Force pseudo-tty so sudo (CAN up/down) works if configured NOPASSWD.
    return QStringList()
        << "-tt"
        << "-o" << "ConnectTimeout=10"
        << "-o" << "StrictHostKeyChecking=no"
        << "-o" << "UserKnownHostsFile=/dev/null"
        << "-o" << "BatchMode=yes"
        << QString("roofus@%1").arg(robotHost);
}

}  // namespace f2c_cpp

