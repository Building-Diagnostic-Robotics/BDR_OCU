#include "satellite_screen.hpp"

#include "satellite_download_dialog.hpp"
#include "satellite_map_widget.hpp"
#include "satellite_mission_controller.hpp"
#include "satellite_palette.hpp"
#include "satellite_ros_link.hpp"
#include "satellite_tile_service.hpp"
#include "settings_constants.hpp"
#include "ui_theme_constants.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

#include <cmath>

namespace f2c_cpp {

namespace {

constexpr const char* kSatViewLatKey = "satellite/center_lat";
constexpr const char* kSatViewLonKey = "satellite/center_lon";
constexpr const char* kSatViewZoomKey = "satellite/zoom";
constexpr const char* kSatLastJobKey = "satellite/last_job";

constexpr double kDefaultLat = 39.5;
constexpr double kDefaultLon = -98.35;
constexpr int kDefaultZoom = 5;
constexpr double kTeleopAngularSpeed = 1.0;  // rad/s

QDoubleSpinBox* makeSpin(double min, double max, double step, int decimals,
                         const QString& suffix) {
    auto* spin = new QDoubleSpinBox;
    spin->setRange(min, max);
    spin->setSingleStep(step);
    spin->setDecimals(decimals);
    spin->setSuffix(suffix);
    spin->setKeyboardTracking(false);
    return spin;
}

void stylePillPair(QLabel* dot, QLabel* text, const QString& label,
                   const QColor& color) {
    dot->setStyleSheet(QStringLiteral(
        "background: %1; border-radius: 4px; min-width: 8px; max-width: 8px; "
        "min-height: 8px; max-height: 8px;").arg(color.name()));
    text->setText(label);
    text->setStyleSheet(QStringLiteral(
        "font-family: 'Arimo'; font-size: 14px; font-weight: 600; color: %1;")
        .arg(color.name()));
}

}  // namespace

SatelliteScreen::SatelliteScreen(QWidget* parent) : QWidget(parent) {
    setObjectName("SatelliteScreen");
    setAttribute(Qt::WA_StyledBackground, true);

    tiles_ = new TileService(this);
    map_ = new SatelliteMapWidget(tiles_, this);
    ros_ = new RosLink(this);
    mission_ = new MissionController(this);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(buildTopBar());
    root->addWidget(buildToolbar());

    auto* content = new QWidget(this);
    auto* content_layout = new QHBoxLayout(content);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(0);
    content_layout->addWidget(map_, 1);
    content_layout->addWidget(buildSidePanel());
    root->addWidget(content, 1);

    // ---- Map <-> panel sync ----
    connect(map_, &SatelliteMapWidget::roiChanged, this, [this] {
        const RoiRect roi = map_->roi();
        for (QDoubleSpinBox* spin : {roi_length_, roi_width_, roi_heading_}) {
            spin->blockSignals(true);
            spin->setEnabled(roi.valid && !mission_->missionActive());
        }
        roi_length_->setValue(roi.length_m);
        roi_width_->setValue(roi.width_m);
        roi_heading_->setValue(roi.heading_deg);
        for (QDoubleSpinBox* spin : {roi_length_, roi_width_, roi_heading_}) {
            spin->blockSignals(false);
        }
    });
    connect(map_, &SatelliteMapWidget::markerChanged, this, [this] {
        const geo::GeoPose marker = map_->marker();
        robot_heading_->blockSignals(true);
        robot_heading_->setEnabled(marker.valid && !mission_->missionActive());
        robot_heading_->setValue(marker.heading_deg);
        robot_heading_->blockSignals(false);
        robot_pos_label_->setText(
            marker.valid ? QStringLiteral("Robot: %1, %2")
                               .arg(marker.lat, 0, 'f', 6)
                               .arg(marker.lon, 0, 'f', 6)
                         : QStringLiteral("Robot: not placed"));
    });

    // ---- ROS telemetry -> map + pills ----
    connect(ros_, &RosLink::gridUpdated, this,
            [this] { map_->setGrid(ros_->gridSnapshot()); });
    connect(ros_, &RosLink::pathUpdated, this,
            [this] { map_->setPath(ros_->pathSnapshot()); });
    connect(ros_, &RosLink::swathsUpdated, this,
            [this] { map_->setSwaths(ros_->swathsSnapshot()); });
    connect(ros_, &RosLink::odomUpdated, this,
            [this] { map_->setOdom(ros_->odomSnapshot()); });
    connect(ros_, &RosLink::statusUpdated, this,
            [this] { updateStatePill(); });
    connect(ros_, &RosLink::segmentStatusUpdated, this, [this] {
        segment_label_->setText(
            QStringLiteral("Segment: %1").arg(ros_->lastSegmentStatus()));
    });
    connect(ros_, &RosLink::axisResult, this,
            [this](bool ok, const QString& detail) {
                appendLog(QStringLiteral("[axis] %1%2")
                              .arg(ok ? QString() : QStringLiteral("FAILED: "))
                              .arg(detail));
            });

    // ---- Mission lifecycle ----
    connect(mission_, &MissionController::logLine, this,
            &SatelliteScreen::appendLog);
    connect(mission_, &MissionController::missionStateChanged, this,
            [this](bool active) {
                map_->setEditLocked(active);
                send_button_->setEnabled(!active);
                end_button_->setEnabled(active);
                autonomy_button_->setEnabled(active);
                arm_button_->setEnabled(active);
                disarm_button_->setEnabled(active);
                estop_button_->setEnabled(active);
                add_roi_button_->setEnabled(!active);
                place_robot_button_->setEnabled(!active);
                if (!active) {
                    autonomy_on_ = false;
                    autonomy_button_->setText(QStringLiteral("Start Autonomy"));
                    map_->clearMissionAnchor();
                    map_->clearTelemetry();
                    setStatePill(QStringLiteral("NO MISSION"), satpal::border());
                    reason_label_->clear();
                    coverage_bar_->setVisible(false);
                }
            });

    teleop_timer_ = new QTimer(this);
    teleop_timer_->setInterval(100);
    connect(teleop_timer_, &QTimer::timeout, this,
            &SatelliteScreen::publishTeleopTick);
    qApp->installEventFilter(this);

    slow_timer_ = new QTimer(this);
    slow_timer_->setInterval(1000);
    connect(slow_timer_, &QTimer::timeout, this,
            &SatelliteScreen::updateLinkPill);
    slow_timer_->start();

    setStatePill(QStringLiteral("NO MISSION"), satpal::border());
    setLinkPill(QStringLiteral("ROS OFFLINE"), satpal::border());
    send_button_->setEnabled(true);
    end_button_->setEnabled(false);
    autonomy_button_->setEnabled(false);
    arm_button_->setEnabled(false);
    disarm_button_->setEnabled(false);
    estop_button_->setEnabled(false);

    applyStyles();

    QString ros_error;
    if (ros_->start(&ros_error)) {
        appendLog(QStringLiteral("[ros] node started"));
    } else {
        appendLog(QStringLiteral("[ros] failed to start: %1").arg(ros_error));
    }
}

SatelliteScreen::~SatelliteScreen() {
    QSettings settings(kSettingsOrgName, kSettingsAppName);
    settings.setValue(kSatViewLatKey, map_->centerLat());
    settings.setValue(kSatViewLonKey, map_->centerLon());
    settings.setValue(kSatViewZoomKey, map_->zoom());
    settings.setValue(kSatLastJobKey, current_job_id_);
}

bool SatelliteScreen::missionActive() const {
    return mission_->missionActive();
}

void SatelliteScreen::shutdownMission() {
    if (!mission_->missionActive()) {
        return;
    }
    ros_->publishAutonomyEnable(false);
    ros_->requestAxisState(RosLink::kAxisIdle);
    mission_->teardownMission();
}

void SatelliteScreen::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!view_initialized_) {
        view_initialized_ = true;
        QSettings settings(kSettingsOrgName, kSettingsAppName);
        map_->setView(settings.value(kSatViewLatKey, kDefaultLat).toDouble(),
                      settings.value(kSatViewLonKey, kDefaultLon).toDouble(),
                      settings.value(kSatViewZoomKey, kDefaultZoom).toInt());
        refreshJobsCombo(settings.value(kSatLastJobKey).toString());
    }
}

// ---- UI construction --------------------------------------------------------

QWidget* SatelliteScreen::buildTopBar() {
    auto* top_bar = new QWidget(this);
    top_bar->setObjectName("SatTopBar");
    top_bar->setFixedHeight(49);
    auto* layout = new QHBoxLayout(top_bar);
    layout->setContentsMargins(24, 0, 24, 0);
    layout->setSpacing(8);

    auto* back = new QPushButton(QStringLiteral("‹  Dashboard"), top_bar);
    back->setObjectName("SatBackButton");
    back->setCursor(Qt::PointingHandCursor);
    back->setFixedHeight(28);
    connect(back, &QPushButton::clicked, this, [this] {
        if (mission_->missionActive()) {
            appendLog(QStringLiteral(
                "[nav] mission active — End Mission before leaving"));
            return;
        }
        emit backRequested();
    });
    layout->addWidget(back, 0, Qt::AlignVCenter);

    auto* title = new QLabel(QStringLiteral("Satellite Coverage"), top_bar);
    title->setObjectName("SatTitle");
    layout->addWidget(title, 0, Qt::AlignVCenter);
    layout->addStretch(1);

    auto makePill = [top_bar](QLabel** dot_out, QLabel** text_out) {
        auto* host = new QWidget(top_bar);
        auto* pill_layout = new QHBoxLayout(host);
        pill_layout->setContentsMargins(0, 0, 0, 0);
        pill_layout->setSpacing(8);
        auto* dot = new QLabel(host);
        dot->setFixedSize(8, 8);
        auto* text = new QLabel(host);
        pill_layout->addWidget(dot, 0, Qt::AlignVCenter);
        pill_layout->addWidget(text, 0, Qt::AlignVCenter);
        *dot_out = dot;
        *text_out = text;
        return host;
    };
    layout->addWidget(makePill(&link_pill_dot_, &link_pill_text_));
    layout->addSpacing(16);
    layout->addWidget(makePill(&state_pill_dot_, &state_pill_text_));
    return top_bar;
}

QWidget* SatelliteScreen::buildToolbar() {
    auto* toolbar = new QWidget(this);
    toolbar->setObjectName("SatToolbar");
    toolbar->setFixedHeight(52);
    auto* layout = new QHBoxLayout(toolbar);
    layout->setContentsMargins(24, 8, 24, 8);
    layout->setSpacing(8);

    auto* jobs_label = new QLabel(QStringLiteral("Job"), toolbar);
    jobs_label->setObjectName("SatMuted");
    layout->addWidget(jobs_label);

    jobs_combo_ = new QComboBox(toolbar);
    jobs_combo_->setMinimumWidth(220);
    connect(jobs_combo_, QOverload<int>::of(&QComboBox::activated), this,
            [this](int index) {
                const QString id = jobs_combo_->itemData(index).toString();
                for (const Job& job : jobs_) {
                    if (job.id == id) {
                        loadJob(job);
                        return;
                    }
                }
                newJob();
            });
    layout->addWidget(jobs_combo_);

    auto* new_button = new QPushButton(QStringLiteral("New"), toolbar);
    connect(new_button, &QPushButton::clicked, this, &SatelliteScreen::newJob);
    layout->addWidget(new_button);

    auto* save_button = new QPushButton(QStringLiteral("Save Job"), toolbar);
    connect(save_button, &QPushButton::clicked, this,
            &SatelliteScreen::saveJob);
    layout->addWidget(save_button);
    layout->addSpacing(16);

    address_edit_ = new QLineEdit(toolbar);
    address_edit_->setPlaceholderText(
        QStringLiteral("Address or \"lat, lon\""));
    address_edit_->setMinimumWidth(240);
    connect(address_edit_, &QLineEdit::returnPressed, this,
            &SatelliteScreen::onGoToAddress);
    layout->addWidget(address_edit_, 1);

    auto* go = new QPushButton(QStringLiteral("Go"), toolbar);
    connect(go, &QPushButton::clicked, this, &SatelliteScreen::onGoToAddress);
    layout->addWidget(go);

    auto* download = new QPushButton(QStringLiteral("Download Area…"), toolbar);
    connect(download, &QPushButton::clicked, this,
            &SatelliteScreen::onDownloadArea);
    layout->addWidget(download);
    return toolbar;
}

QWidget* SatelliteScreen::buildSidePanel() {
    auto* content = new QWidget;
    content->setObjectName("SatPanel");
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(12);
    layout->addWidget(buildPlanCard());
    layout->addWidget(buildMissionCard());
    layout->addWidget(buildTeleopCard());

    log_view_ = new QPlainTextEdit(content);
    log_view_->setObjectName("SatLog");
    log_view_->setReadOnly(true);
    log_view_->setMaximumBlockCount(600);
    log_view_->setFixedHeight(110);
    layout->addWidget(log_view_);
    layout->addStretch(1);

    auto* scroll = new QScrollArea(this);
    scroll->setObjectName("SatPanelScroll");
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFixedWidth(356);
    return scroll;
}

QWidget* SatelliteScreen::buildPlanCard() {
    auto* card = new QWidget;
    card->setObjectName("SatCard");
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(8);

    auto* title = new QLabel(QStringLiteral("PLAN"), card);
    title->setObjectName("SatCardTitle");
    layout->addWidget(title);

    job_name_ = new QLineEdit(card);
    job_name_->setPlaceholderText(QStringLiteral("Building / job name"));
    job_address_ = new QLineEdit(card);
    job_address_->setPlaceholderText(QStringLiteral("Address (reference)"));
    layout->addWidget(job_name_);
    layout->addWidget(job_address_);

    auto* buttons = new QHBoxLayout;
    add_roi_button_ = new QPushButton(QStringLiteral("Add ROI"), card);
    place_robot_button_ = new QPushButton(QStringLiteral("Place Robot"), card);
    connect(add_roi_button_, &QPushButton::clicked, this,
            [this] { map_->addRoiAtViewCenter(); });
    connect(place_robot_button_, &QPushButton::clicked, this, [this] {
        map_->armMarkerPlacement();
        appendLog(QStringLiteral(
            "[plan] click the map where the robot physically sits"));
    });
    buttons->addWidget(add_roi_button_);
    buttons->addWidget(place_robot_button_);
    layout->addLayout(buttons);

    auto addRow = [&](const QString& label, QWidget* field) {
        auto* row = new QHBoxLayout;
        auto* text = new QLabel(label, card);
        text->setObjectName("SatMuted");
        text->setMinimumWidth(110);
        row->addWidget(text);
        row->addWidget(field, 1);
        layout->addLayout(row);
    };

    roi_length_ = makeSpin(2.0, 500.0, 0.5, 1, QStringLiteral(" m"));
    roi_width_ = makeSpin(2.0, 500.0, 0.5, 1, QStringLiteral(" m"));
    roi_heading_ = makeSpin(0.0, 359.9, 1.0, 1, QStringLiteral(" °"));
    roi_heading_->setWrapping(true);
    robot_heading_ = makeSpin(0.0, 359.9, 1.0, 1, QStringLiteral(" °"));
    robot_heading_->setWrapping(true);
    addRow(QStringLiteral("ROI along"), roi_length_);
    addRow(QStringLiteral("ROI across"), roi_width_);
    addRow(QStringLiteral("ROI heading"), roi_heading_);
    addRow(QStringLiteral("Robot heading"), robot_heading_);

    const auto pushRoi = [this] {
        RoiRect roi = map_->roi();
        if (!roi.valid) {
            return;
        }
        roi.length_m = roi_length_->value();
        roi.width_m = roi_width_->value();
        roi.heading_deg = roi_heading_->value();
        map_->setRoi(roi);
    };
    connect(roi_length_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, pushRoi);
    connect(roi_width_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, pushRoi);
    connect(roi_heading_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, pushRoi);
    connect(robot_heading_,
            QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double value) {
                geo::GeoPose marker = map_->marker();
                if (marker.valid) {
                    marker.heading_deg = value;
                    map_->setMarker(marker);
                }
            });

    robot_pos_label_ = new QLabel(QStringLiteral("Robot: not placed"), card);
    robot_pos_label_->setObjectName("SatMuted");
    robot_pos_label_->setWordWrap(true);
    layout->addWidget(robot_pos_label_);
    return card;
}

QWidget* SatelliteScreen::buildMissionCard() {
    auto* card = new QWidget;
    card->setObjectName("SatCard");
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(8);

    auto* title = new QLabel(QStringLiteral("MISSION"), card);
    title->setObjectName("SatCardTitle");
    layout->addWidget(title);

    reason_label_ = new QLabel(card);
    reason_label_->setObjectName("SatMuted");
    reason_label_->setWordWrap(true);
    layout->addWidget(reason_label_);

    coverage_bar_ = new QProgressBar(card);
    coverage_bar_->setRange(0, 1000);
    coverage_bar_->setFormat(QStringLiteral("coverage %p%"));
    coverage_bar_->setVisible(false);
    layout->addWidget(coverage_bar_);

    segment_label_ = new QLabel(card);
    segment_label_->setObjectName("SatMuted");
    layout->addWidget(segment_label_);

    send_button_ = new QPushButton(QStringLiteral("Send to Robot"), card);
    send_button_->setObjectName("SatPrimaryButton");
    send_button_->setMinimumHeight(38);
    connect(send_button_, &QPushButton::clicked, this,
            &SatelliteScreen::onSendMission);
    layout->addWidget(send_button_);

    auto* row1 = new QHBoxLayout;
    autonomy_button_ = new QPushButton(QStringLiteral("Start Autonomy"), card);
    connect(autonomy_button_, &QPushButton::clicked, this, [this] {
        autonomy_on_ = !autonomy_on_;
        autonomy_button_->setText(autonomy_on_
                                      ? QStringLiteral("Pause Autonomy")
                                      : QStringLiteral("Start Autonomy"));
        ros_->publishAutonomyEnable(autonomy_on_);
        appendLog(QStringLiteral("[cmd] autonomy_enable=%1")
                      .arg(autonomy_on_ ? QStringLiteral("true")
                                        : QStringLiteral("false")));
        if (autonomy_on_) {
            teleop_check_->setChecked(false);
        }
    });
    end_button_ = new QPushButton(QStringLiteral("End Mission"), card);
    connect(end_button_, &QPushButton::clicked, this,
            &SatelliteScreen::onEndMission);
    row1->addWidget(autonomy_button_);
    row1->addWidget(end_button_);
    layout->addLayout(row1);

    auto* row2 = new QHBoxLayout;
    arm_button_ = new QPushButton(QStringLiteral("Arm Motors"), card);
    disarm_button_ = new QPushButton(QStringLiteral("Disarm"), card);
    connect(arm_button_, &QPushButton::clicked, this, [this] {
        ros_->requestAxisState(RosLink::kAxisClosedLoop);
        appendLog(QStringLiteral("[cmd] arm (CLOSED_LOOP_CONTROL)"));
    });
    connect(disarm_button_, &QPushButton::clicked, this, [this] {
        ros_->requestAxisState(RosLink::kAxisIdle);
        appendLog(QStringLiteral("[cmd] disarm (IDLE)"));
    });
    row2->addWidget(arm_button_);
    row2->addWidget(disarm_button_);
    layout->addLayout(row2);

    estop_button_ = new QPushButton(QStringLiteral("EMERGENCY STOP"), card);
    estop_button_->setObjectName("SatEstopButton");
    estop_button_->setMinimumHeight(46);
    connect(estop_button_, &QPushButton::clicked, this,
            &SatelliteScreen::onEstop);
    layout->addWidget(estop_button_);
    return card;
}

QWidget* SatelliteScreen::buildTeleopCard() {
    auto* card = new QWidget;
    card->setObjectName("SatCard");
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(8);

    auto* title = new QLabel(QStringLiteral("TELEOP"), card);
    title->setObjectName("SatCardTitle");
    layout->addWidget(title);

    teleop_check_ = new QCheckBox(QStringLiteral("Enable keyboard teleop"), card);
    connect(teleop_check_, &QCheckBox::toggled, this, [this](bool enabled) {
        pressed_keys_.clear();
        if (enabled) {
            teleop_timer_->start();
            appendLog(QStringLiteral(
                "[teleop] active — W/S drive, A/D turn; ownership switch "
                "disarms motors, re-arm after"));
        } else {
            teleop_timer_->stop();
            ros_->publishTwist(0.0, 0.0);
        }
    });
    layout->addWidget(teleop_check_);

    auto* row = new QHBoxLayout;
    auto* label = new QLabel(QStringLiteral("Speed"), card);
    label->setObjectName("SatMuted");
    teleop_speed_ = new QSlider(Qt::Horizontal, card);
    teleop_speed_->setRange(10, 80);
    teleop_speed_->setValue(35);
    teleop_speed_label_ = new QLabel(QStringLiteral("0.35 m/s"), card);
    teleop_speed_label_->setObjectName("SatMuted");
    connect(teleop_speed_, &QSlider::valueChanged, this, [this](int value) {
        teleop_speed_label_->setText(
            QStringLiteral("%1 m/s").arg(value / 100.0, 0, 'f', 2));
    });
    row->addWidget(label);
    row->addWidget(teleop_speed_, 1);
    row->addWidget(teleop_speed_label_);
    layout->addLayout(row);
    return card;
}

void SatelliteScreen::applyStyles() {
    const UiThemeTokens t = uiThemeTokens(dark_mode_);
    setStyleSheet(QStringLiteral(R"QSS(
QWidget#SatelliteScreen { background: %1; }
QWidget#SatTopBar { background: %2; border-bottom: 1px solid %3; }
QWidget#SatToolbar { background: %2; border-bottom: 1px solid %3; }
QWidget#SatPanel { background: %1; }
QScrollArea#SatPanelScroll { background: %1; border: none; border-left: 1px solid %3; }
QWidget#SatCard { background: %2; border: 1px solid %3; border-radius: 10px; }
QLabel { color: %4; font-family: 'Arimo'; font-size: 13px; }
QLabel#SatTitle { font-family: 'Arimo'; font-weight: 700; font-size: 16px; color: %4; }
QLabel#SatCardTitle {
    font-family: 'Arimo'; font-weight: 700; font-size: 11px;
    letter-spacing: 1px; color: %5;
}
QLabel#SatMuted { color: %5; font-family: 'Arimo'; font-size: 12px; }
QPushButton {
    background: transparent; border: 1px solid %3; border-radius: 6px;
    padding: 6px 12px; color: %4; font-family: 'Arimo'; font-size: 13px;
}
QPushButton:hover { border-color: %6; color: %6; }
QPushButton:disabled { color: %5; border-color: %3; }
QPushButton#SatBackButton { border: none; color: %5; font-size: 14px; }
QPushButton#SatBackButton:hover { color: %4; }
QPushButton#SatPrimaryButton {
    background: %6; border-color: %6; color: #FFFFFF; font-weight: 700;
}
QPushButton#SatPrimaryButton:hover { background: %7; }
QPushButton#SatPrimaryButton:disabled { background: %3; border-color: %3; color: %5; }
QPushButton#SatEstopButton {
    background: %8; border-color: %8; color: #FFFFFF;
    font-weight: 800; font-size: 14px; letter-spacing: 1px;
}
QPushButton#SatEstopButton:disabled { background: %3; border-color: %3; color: %5; }
QLineEdit, QDoubleSpinBox, QComboBox {
    background: %9; border: 1px solid %3; border-radius: 6px;
    padding: 5px 8px; color: %4; font-family: 'Arimo'; font-size: 13px;
}
QLineEdit:focus, QDoubleSpinBox:focus, QComboBox:focus { border-color: %6; }
QComboBox::drop-down { border: none; width: 22px; }
QComboBox QAbstractItemView {
    background: %2; border: 1px solid %3; color: %4;
    selection-background-color: %3;
}
QCheckBox { color: %4; font-family: 'Arimo'; font-size: 13px; }
QSlider::groove:horizontal { height: 4px; background: %3; border-radius: 2px; }
QSlider::handle:horizontal {
    width: 14px; height: 14px; margin: -5px 0; border-radius: 7px; background: %6;
}
QProgressBar {
    background: %9; border: 1px solid %3; border-radius: 6px;
    text-align: center; color: %4; font-family: 'Arimo'; font-size: 11px;
}
QProgressBar::chunk { background: %6; border-radius: 5px; }
QPlainTextEdit#SatLog {
    background: %9; border: 1px solid %3; border-radius: 8px;
    color: %5; font-family: monospace; font-size: 11px;
}
)QSS")
            .arg(t.bg, t.card_bg, t.border, t.text, t.muted, t.accent,
                 t.accent_hover, t.danger, t.log_bg));
}

void SatelliteScreen::setDarkMode(bool dark_mode) {
    dark_mode_ = dark_mode;
    applyStyles();
}

// ---- Jobs -------------------------------------------------------------------

void SatelliteScreen::refreshJobsCombo(const QString& select_id) {
    jobs_ = job_store_.loadAll();
    jobs_combo_->clear();
    jobs_combo_->addItem(QStringLiteral("— unsaved plan —"), QString());
    int select_index = 0;
    for (const Job& job : jobs_) {
        jobs_combo_->addItem(job.name.isEmpty() ? job.id : job.name, job.id);
        if (!select_id.isEmpty() && job.id == select_id) {
            select_index = jobs_combo_->count() - 1;
        }
    }
    jobs_combo_->setCurrentIndex(select_index);
    if (select_index > 0) {
        loadJob(jobs_[select_index - 1]);
    }
}

void SatelliteScreen::loadJob(const Job& job) {
    current_job_id_ = job.id;
    job_name_->setText(job.name);
    job_address_->setText(job.address);
    map_->setRoi(job.roi);
    map_->setMarker(job.robot);
    emit map_->roiChanged();
    emit map_->markerChanged();
    if (job.roi.valid) {
        map_->setView(job.roi.center.lat, job.roi.center.lon, 19);
    }
    appendLog(QStringLiteral("[job] loaded '%1'").arg(job.name));
}

void SatelliteScreen::newJob() {
    current_job_id_.clear();
    jobs_combo_->setCurrentIndex(0);
    job_name_->clear();
    job_address_->clear();
    map_->setRoi(RoiRect{});
    map_->setMarker(geo::GeoPose{});
    emit map_->roiChanged();
    emit map_->markerChanged();
}

void SatelliteScreen::saveJob() {
    Job job;
    job.name = job_name_->text().trimmed();
    if (job.name.isEmpty()) {
        appendLog(QStringLiteral("[job] give the job a name before saving"));
        return;
    }
    job.address = job_address_->text().trimmed();
    job.roi = map_->roi();
    job.robot = map_->marker();
    job.updated = QDateTime::currentDateTime();
    if (current_job_id_.isEmpty()) {
        job.id = JobStore::slugify(job.name) + QStringLiteral("_") +
                 QUuid::createUuid().toString(QUuid::Id128).left(6);
        job.created = job.updated;
    } else {
        job.id = current_job_id_;
        for (const Job& existing : jobs_) {
            if (existing.id == job.id) {
                job.created = existing.created;
                break;
            }
        }
    }
    QString error;
    if (!job_store_.save(job, &error)) {
        appendLog(QStringLiteral("[job] save failed: %1").arg(error));
        return;
    }
    current_job_id_ = job.id;
    refreshJobsCombo(job.id);
    appendLog(QStringLiteral("[job] saved '%1'").arg(job.name));
}

// ---- Navigation -------------------------------------------------------------

void SatelliteScreen::onGoToAddress() {
    const QString query = address_edit_->text().trimmed();
    if (query.isEmpty()) {
        return;
    }
    const QStringList parts = query.split(QLatin1Char(','));
    if (parts.size() == 2) {
        bool lat_ok = false;
        bool lon_ok = false;
        const double lat = parts[0].trimmed().toDouble(&lat_ok);
        const double lon = parts[1].trimmed().toDouble(&lon_ok);
        if (lat_ok && lon_ok && std::abs(lat) <= 85.0 &&
            std::abs(lon) <= 180.0) {
            map_->setView(lat, lon, 18);
            return;
        }
    }
    appendLog(QStringLiteral("[geo] searching '%1'…").arg(query));
    tiles_->geocode(query,
                    [this](bool ok, double lat, double lon, QString label) {
                        appendLog(QStringLiteral("[geo] %1").arg(label));
                        if (ok) {
                            map_->setView(lat, lon, 18);
                        }
                    });
}

void SatelliteScreen::onDownloadArea() {
    DownloadAreaDialog dialog(tiles_, map_->centerLat(), map_->centerLon(),
                              this);
    connect(&dialog, &DownloadAreaDialog::areaReady, this,
            [this](double lat, double lon) { map_->setView(lat, lon, 18); });
    dialog.exec();
}

// ---- Mission ----------------------------------------------------------------

bool SatelliteScreen::confirmDialog(const QString& title, const QString& body,
                                    const QString& accept_label) {
    const UiThemeTokens t = uiThemeTokens(dark_mode_);
    QDialog dialog(this);
    dialog.setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    dialog.setModal(true);
    dialog.setMinimumWidth(430);
    dialog.setStyleSheet(QStringLiteral(
        "QDialog { background: %1; border: 1px solid %2; border-radius: 10px; }"
        "QLabel { color: %3; font-family: 'Arimo'; }"
        "QPushButton { background: transparent; border: 1px solid %2; "
        "border-radius: 6px; padding: 7px 16px; color: %3; font-family: 'Arimo'; }"
        "QPushButton#Accept { background: %4; border-color: %4; color: white; "
        "font-weight: 700; }")
            .arg(t.card_bg, t.border, t.text, t.accent));
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(12);

    auto* title_label = new QLabel(title, &dialog);
    QFont title_font = title_label->font();
    title_font.setPointSizeF(title_font.pointSizeF() + 3);
    title_font.setBold(true);
    title_label->setFont(title_font);
    layout->addWidget(title_label);

    auto* body_label = new QLabel(body, &dialog);
    body_label->setWordWrap(true);
    layout->addWidget(body_label);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto* cancel = new QPushButton(QStringLiteral("Cancel"), &dialog);
    auto* accept = new QPushButton(accept_label, &dialog);
    accept->setObjectName("Accept");
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(accept, &QPushButton::clicked, &dialog, &QDialog::accept);
    buttons->addWidget(cancel);
    buttons->addWidget(accept);
    layout->addLayout(buttons);
    return dialog.exec() == QDialog::Accepted;
}

void SatelliteScreen::onSendMission() {
    const RoiRect roi = map_->roi();
    const geo::GeoPose marker = map_->marker();
    if (!roi.valid || !marker.valid) {
        appendLog(QStringLiteral(
            "[send] draw the ROI and place the robot marker first"));
        return;
    }
    const QString roi_arg =
        MissionController::roiVerticesArgument(roi, marker);
    if (!confirmDialog(
            QStringLiteral("Send Mission to Robot"),
            QStringLiteral(
                "Confirm before launch:\n\n"
                "• The robot is physically at the marker position.\n"
                "• The robot is facing the marker's arrow direction (%1°).\n"
                "• The ROI will be anchored to the robot exactly as drawn.\n\n"
                "roi_vertices (robot frame): %2")
                .arg(marker.heading_deg, 0, 'f', 1)
                .arg(roi_arg),
            QStringLiteral("Launch Mission"))) {
        return;
    }
    QString error;
    if (!mission_->startMission(roi, marker, &error)) {
        appendLog(QStringLiteral("[send] FAILED: %1").arg(error));
        return;
    }
    map_->setMissionAnchor(marker);
    setStatePill(QStringLiteral("LAUNCHING"), satpal::warning());
    reason_label_->setText(
        QStringLiteral("Waiting for autonomy stack… Arm motors, then Start "
                       "Autonomy when the plan appears."));
}

void SatelliteScreen::onEndMission() {
    if (!confirmDialog(
            QStringLiteral("End Mission"),
            QStringLiteral("Autonomy will be disabled, motors disarmed, and "
                           "both launch trees stopped."),
            QStringLiteral("End Mission"))) {
        return;
    }
    shutdownMission();
}

void SatelliteScreen::onEstop() {
    ros_->publishAutonomyEnable(false);
    ros_->requestAxisState(RosLink::kAxisIdle);
    appendLog(
        QStringLiteral("[E-STOP] autonomy disabled + axis IDLE requested"));
    setStatePill(QStringLiteral("E-STOP"), satpal::danger());
}

// ---- Teleop -----------------------------------------------------------------

bool SatelliteScreen::eventFilter(QObject* watched, QEvent* event) {
    if (!isVisible() || !teleop_check_ || !teleop_check_->isChecked()) {
        return QWidget::eventFilter(watched, event);
    }
    if (event->type() != QEvent::KeyPress &&
        event->type() != QEvent::KeyRelease) {
        return QWidget::eventFilter(watched, event);
    }
    auto* key_event = static_cast<QKeyEvent*>(event);
    if (key_event->isAutoRepeat()) {
        return QWidget::eventFilter(watched, event);
    }
    if (qobject_cast<QLineEdit*>(QApplication::focusWidget())) {
        return QWidget::eventFilter(watched, event);
    }
    const int key = key_event->key();
    const bool teleop_key = key == Qt::Key_W || key == Qt::Key_S ||
                            key == Qt::Key_A || key == Qt::Key_D ||
                            key == Qt::Key_Up || key == Qt::Key_Down ||
                            key == Qt::Key_Left || key == Qt::Key_Right;
    if (!teleop_key) {
        return QWidget::eventFilter(watched, event);
    }
    if (event->type() == QEvent::KeyPress) {
        pressed_keys_.insert(key);
    } else {
        pressed_keys_.remove(key);
    }
    return true;
}

void SatelliteScreen::publishTeleopTick() {
    if (!teleop_check_->isChecked() || !ros_->isRunning()) {
        return;
    }
    double linear = 0.0;
    double angular = 0.0;
    const double speed = teleop_speed_->value() / 100.0;
    if (pressed_keys_.contains(Qt::Key_W) ||
        pressed_keys_.contains(Qt::Key_Up)) {
        linear += speed;
    }
    if (pressed_keys_.contains(Qt::Key_S) ||
        pressed_keys_.contains(Qt::Key_Down)) {
        linear -= speed;
    }
    if (pressed_keys_.contains(Qt::Key_A) ||
        pressed_keys_.contains(Qt::Key_Left)) {
        angular += kTeleopAngularSpeed;
    }
    if (pressed_keys_.contains(Qt::Key_D) ||
        pressed_keys_.contains(Qt::Key_Right)) {
        angular -= kTeleopAngularSpeed;
    }
    ros_->publishTwist(linear, angular);
}

// ---- Status surfaces ----------------------------------------------------------

void SatelliteScreen::setLinkPill(const QString& text, const QColor& color) {
    stylePillPair(link_pill_dot_, link_pill_text_, text, color);
}

void SatelliteScreen::setStatePill(const QString& text, const QColor& color) {
    stylePillPair(state_pill_dot_, state_pill_text_, text, color);
}

void SatelliteScreen::updateLinkPill() {
    if (!ros_->isRunning()) {
        setLinkPill(QStringLiteral("ROS OFFLINE"), satpal::border());
        return;
    }
    const OdomSnapshot odom = ros_->odomSnapshot();
    if (!odom.valid) {
        setLinkPill(QStringLiteral("BOT —"), satpal::border());
        return;
    }
    const qint64 age_ms = QDateTime::currentMSecsSinceEpoch() - odom.wall_ms;
    if (age_ms < 2500) {
        setLinkPill(QStringLiteral("BOT LIVE"), satpal::accent());
    } else if (age_ms < 10000) {
        setLinkPill(QStringLiteral("BOT SYNCING %1s").arg(age_ms / 1000),
                    satpal::warning());
    } else {
        setLinkPill(QStringLiteral("BOT OFFLINE %1s").arg(age_ms / 1000),
                    satpal::danger());
    }
}

void SatelliteScreen::updateStatePill() {
    const CoverageStatus status = ros_->coverageStatus();
    if (!status.valid) {
        return;
    }
    const QString state = status.state.toUpper();
    QColor color = satpal::border();
    if (state == QLatin1String("RUNNING")) {
        color = satpal::accent();
    } else if (state == QLatin1String("BLOCKED")) {
        color = satpal::danger();
    } else if (state == QLatin1String("PAUSED") ||
               state == QLatin1String("REPLANNING") ||
               state.contains(QLatin1String("COLLECTION"))) {
        color = satpal::warning();
    } else if (state == QLatin1String("COMPLETE")) {
        color = satpal::info();
    }
    setStatePill(status.mode.isEmpty()
                     ? state
                     : QStringLiteral("%1 · %2").arg(state, status.mode),
                 color);
    reason_label_->setText(status.reason);
    if (status.coverage >= 0.0) {
        coverage_bar_->setVisible(true);
        coverage_bar_->setValue(
            int(qBound(0.0, status.coverage, 1.0) * 1000));
    }
}

void SatelliteScreen::appendLog(const QString& line) {
    log_view_->appendPlainText(line);
}

}  // namespace f2c_cpp
