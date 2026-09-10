/**
 * @file satellite_screen.cpp
 * @brief Implementation of Stage 6 — ROI Coverage planning + mission.
 *
 * Construction mirrors the staged screens: file-local SVG/pill factories
 * (same shapes as exploration_screen.cpp / planner_screen.cpp), named
 * pixel constants, per-element Arimo styling, object-name-scoped QSS only
 * (no bare-selector cascading rules). Dark palette is the zinc family the
 * modals and planner use (#18181b / #27272a / #3f3f47, #00BC7D accent);
 * light mode follows the dashboard's white-card language.
 */

#include "satellite_screen.hpp"

#include "link_health_monitor.hpp"
#include "satellite_download_dialog.hpp"
#include "satellite_map_widget.hpp"
#include "satellite_mission_controller.hpp"
#include "satellite_palette.hpp"
#include "satellite_ros_link.hpp"
#include "satellite_tile_service.hpp"
#include "settings_constants.hpp"
#include "units_system.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFile>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QStyle>
#include <QSvgRenderer>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

#include <cmath>

namespace f2c_cpp {

namespace {

// ---- Layout constants (Stage 4/5 conventions) -------------------------------
constexpr int kTopBarHeight = 49;
constexpr int kTopStatusItemHeight = 20;
constexpr int kTopStatusBatteryMinWidth = 52;
constexpr int kTopStatusPillMinWidth = 69;
constexpr int kTopStatusMotorsChipMinWidth = 96;
constexpr int kTopStatusMotorsChipHeight = 20;
// Floating window controls (theme toggle + min/max/close) overlay the
// top-right corner — same reservation Stage 4/5 make.
constexpr int kTopStatusWindowControlsReservedWidth = 184;
constexpr int kLeftRailWidth = 320;
constexpr int kSendButtonHeight = 44;
constexpr int kEstopButtonHeight = 44;

constexpr const char* kSatViewLatKey = "satellite/center_lat";
constexpr const char* kSatViewLonKey = "satellite/center_lon";
constexpr const char* kSatViewZoomKey = "satellite/zoom";

constexpr double kDefaultLat = 39.5;
constexpr double kDefaultLon = -98.35;
constexpr int kDefaultZoom = 5;
constexpr double kTeleopAngularSpeed = 1.0;  // rad/s

// Measured plans live in a fictional geo frame anchored at the reference
// origin — the operator only ever sees meters.
constexpr int kMeasuredDefaultZoom = 21;

/** Spin display value -> meters, honoring the operator's unit system. */
double spinToMeters(double display_value) {
    return UnitsProvider::instance()->isMetric()
               ? display_value
               : units::feetToMeters(display_value);
}

/** Meters -> spin display value. */
double metersToSpin(double meters) {
    return UnitsProvider::instance()->isMetric()
               ? meters
               : units::metersToFeet(meters);
}

// ---- Palette (dark = zinc family per MissionMetadataDialog / planner) -------
constexpr const char* kAccent = "#00BC7D";
constexpr const char* kAccentHover = "#00A86D";
constexpr const char* kEstopRed = "#E7000B";
constexpr const char* kEstopRedHover = "#C10007";
constexpr const char* kAmber = "#F0B100";

QString mutedColor(bool dark) {
    return dark ? QStringLiteral("#9F9FA9") : QStringLiteral("#6B7280");
}

QString textColor(bool dark) {
    return dark ? QStringLiteral("#FAFAFA") : QStringLiteral("#1E2939");
}

/** Pill text style — the Stage 4/5 kInitialStatus14 shape. */
QString statusTextStyle(const QString& color) {
    return QStringLiteral(
               "font-family: 'Arimo'; font-size: 14px; font-weight: 400; "
               "color: %1;")
        .arg(color);
}

/** SVG loader with stroke AND fill retint (superset of the screens'
    loadSvgPixmap — the status dot is fill-based, the icons stroke-based). */
QPixmap loadTintedSvg(const QString& resource_path, int w, int h,
                      const QString& color = QString()) {
    QFile file(resource_path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QPixmap();
    }
    QByteArray data = file.readAll();
    file.close();
    if (!color.isEmpty()) {
        for (const QByteArray needle : {QByteArrayLiteral("stroke=\""),
                                        QByteArrayLiteral("fill=\"")}) {
            int index = data.indexOf(needle);
            while (index >= 0) {
                const int value_start = index + needle.size();
                const int value_end = data.indexOf('"', value_start);
                if (value_end <= value_start) {
                    break;
                }
                const QByteArray value =
                    data.mid(value_start, value_end - value_start);
                if (value != "none") {
                    data = data.left(value_start) + color.toUtf8() +
                           data.mid(value_end);
                }
                index = data.indexOf(needle, value_start);
            }
        }
    }
    QSvgRenderer renderer(data);
    if (!renderer.isValid()) {
        return QPixmap();
    }
    QPixmap pixmap(w, h);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    renderer.render(&painter);
    return pixmap;
}

QLabel* makeStatusIconLabel(QWidget* parent, const QString& resource_path,
                            int size, const QString& color = QString()) {
    auto* label = new QLabel(parent);
    label->setFixedSize(size, size);
    label->setAlignment(Qt::AlignCenter);
    label->setAttribute(Qt::WA_TranslucentBackground, true);
    label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    label->setStyleSheet(QStringLiteral("background: transparent;"));
    label->setPixmap(loadTintedSvg(resource_path, size, size, color));
    return label;
}

QLabel* makeStatusTextLabel(QWidget* parent, const QString& text,
                            const QString& style) {
    auto* label = new QLabel(text, parent);
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    label->setAttribute(Qt::WA_TranslucentBackground, true);
    label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    label->setStyleSheet(style + QStringLiteral(" background: transparent;"));
    return label;
}

/** Stage 4/5 status pill: icon + text with a fixed minimum width. */
QWidget* makeStatusItem(QWidget* parent, const QString& resource_path,
                        int icon_size, const QString& text, int minimum_width,
                        const QString& text_style, QLabel** out_icon,
                        QLabel** out_label) {
    auto* item = new QWidget(parent);
    item->setFixedHeight(kTopStatusItemHeight);
    item->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    if (minimum_width > 0) {
        item->setMinimumWidth(minimum_width);
    }
    auto* layout = new QHBoxLayout(item);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    auto* icon = makeStatusIconLabel(item, resource_path, icon_size);
    layout->addWidget(icon, 0, Qt::AlignVCenter);
    auto* label = makeStatusTextLabel(item, text, text_style);
    layout->addWidget(label, 0, Qt::AlignVCenter);
    layout->addStretch(1);
    if (out_icon) {
        *out_icon = icon;
    }
    if (out_label) {
        *out_label = label;
    }
    return item;
}

/** Rail-card section header — the Mission Planner pattern: 16px accent
    icon + Title Case Arimo 700 14. */
QWidget* makeCardHeader(const QString& icon_alias, const QString& text,
                        QWidget* parent) {
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 4);
    layout->setSpacing(8);
    auto* icon = new QLabel(row);
    icon->setFixedSize(16, 16);
    icon->setAlignment(Qt::AlignCenter);
    icon->setAttribute(Qt::WA_TranslucentBackground, true);
    icon->setStyleSheet(QStringLiteral("background: transparent;"));
    icon->setPixmap(loadTintedSvg(icon_alias, 16, 16, QLatin1String(kAccent)));
    layout->addWidget(icon, 0, Qt::AlignVCenter);
    auto* label = new QLabel(text, row);
    label->setObjectName("SatCardHeader");
    layout->addWidget(label, 1, Qt::AlignVCenter);
    return row;
}

/** Field row: muted 12px label left, field right — the rail's form shape. */
QWidget* makeFieldRow(const QString& label_text, QWidget* field,
                      QWidget* parent) {
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    auto* label = new QLabel(label_text, row);
    label->setObjectName("SatFieldLabel");
    label->setMinimumWidth(104);
    layout->addWidget(label, 0, Qt::AlignVCenter);
    layout->addWidget(field, 1);
    return row;
}

QDoubleSpinBox* makeSpin(double min, double max, double step, int decimals,
                         const QString& suffix) {
    auto* spin = new QDoubleSpinBox;
    spin->setObjectName("SatInput");
    spin->setRange(min, max);
    spin->setSingleStep(step);
    spin->setDecimals(decimals);
    spin->setSuffix(suffix);
    spin->setKeyboardTracking(false);
    spin->setFixedHeight(36);
    return spin;
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

    auto* content = new QWidget(this);
    auto* content_layout = new QHBoxLayout(content);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(0);
    content_layout->addWidget(buildLeftRail());
    content_layout->addWidget(map_, 1);
    root->addWidget(content, 1);

    // ---- Map <-> rail sync ----
    connect(map_, &SatelliteMapWidget::roiChanged, this, [this] {
        const RoiRect roi = map_->roi();
        // Along/across/heading only describe a rectangle. Once the operator
        // draws a free polygon the rect mirror is stale, so the fields go
        // read-only rather than reporting a shape that is no longer on the
        // canvas — the per-edge chips are the live dimensions there.
        const bool rectangular = map_->polygon().vertices.size() <= 4;
        for (QDoubleSpinBox* spin : {roi_length_, roi_width_, roi_heading_}) {
            spin->blockSignals(true);
            spin->setEnabled(roi.valid && rectangular &&
                             !mission_->missionActive());
        }
        roi_length_->setValue(metersToSpin(roi.length_m));
        roi_width_->setValue(metersToSpin(roi.width_m));
        roi_heading_->setValue(roi.heading_deg);
        for (QDoubleSpinBox* spin : {roi_length_, roi_width_, roi_heading_}) {
            spin->blockSignals(false);
        }
    });
    // Units toggle: re-suffix + re-display the length fields (values stay
    // SI in the model; only the presentation flips — house rule).
    connect(UnitsProvider::instance(), &UnitsProvider::unitsChanged, this,
            [this](Units) {
                const RoiRect roi = map_->roi();
                for (QDoubleSpinBox* spin : {roi_length_, roi_width_}) {
                    spin->blockSignals(true);
                    spin->setSuffix(QStringLiteral(" ") +
                                    units::lengthUnitSuffix());
                }
                roi_length_->setValue(metersToSpin(roi.length_m));
                roi_width_->setValue(metersToSpin(roi.width_m));
                for (QDoubleSpinBox* spin : {roi_length_, roi_width_}) {
                    spin->blockSignals(false);
                }
            });
    // World Imagery is a mosaic: the same roof can be served from different
    // flights at different zooms. Re-check provenance whenever the view moves,
    // but only mark it dirty here — viewChanged fires on every wheel notch and
    // drag step. refreshImageryInfo() runs off slow_timer_ at 1 Hz.
    connect(map_, &SatelliteMapWidget::viewChanged, this,
            [this](double, double, int) { imagery_query_pending_ = true; });
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
    // Every handler stamps the app's LinkHealthMonitor (house rule: a new
    // subscriber that doesn't stamp makes the monitor go OFFLINE during a
    // healthy session that only uses that topic).
    connect(ros_, &RosLink::gridUpdated, this, [this] {
        if (link_monitor_) link_monitor_->stamp(LinkHealthMonitor::Source::ScanStatus);
        map_->setGrid(ros_->gridSnapshot());
        if (!manager_occupancy_seen_ && ros_->gridSnapshot().revision > 0) {
            manager_occupancy_seen_ = true;
            appendLog(QStringLiteral(
                "[ros] coverage occupancy received — manager is live"));
        }
    });
    connect(ros_, &RosLink::pathUpdated, this, [this] {
        if (link_monitor_) link_monitor_->stamp(LinkHealthMonitor::Source::ScanStatus);
        map_->setPath(ros_->pathSnapshot());
    });
    connect(ros_, &RosLink::swathsUpdated, this, [this] {
        if (link_monitor_) link_monitor_->stamp(LinkHealthMonitor::Source::ScanStatus);
        map_->setSwaths(ros_->swathsSnapshot());
    });
    connect(ros_, &RosLink::odomUpdated, this, [this] {
        if (link_monitor_) link_monitor_->stamp(LinkHealthMonitor::Source::Odom);
        map_->setOdom(ros_->odomSnapshot());
    });
    connect(ros_, &RosLink::statusUpdated, this, [this] {
        if (link_monitor_) link_monitor_->stamp(LinkHealthMonitor::Source::ScanStatus);
        updateStatePill();
    });
    connect(ros_, &RosLink::segmentStatusUpdated, this, [this] {
        if (link_monitor_) link_monitor_->stamp(LinkHealthMonitor::Source::ScanStatus);
        segment_label_->setText(
            QStringLiteral("Segment: %1").arg(ros_->lastSegmentStatus()));
    });
    connect(ros_, &RosLink::axisResult, this,
            [this](bool ok, const QString& detail) {
                appendLog(QStringLiteral("[axis] %1%2")
                              .arg(ok ? QString() : QStringLiteral("FAILED: "))
                              .arg(detail));
                if (ok && detail.contains(QStringLiteral("state now"))) {
                    if (detail.endsWith(QStringLiteral("8"))) {
                        setMotorsChip(QStringLiteral("MOTORS ARMED"),
                                      QColor(kAccent));
                    } else if (detail.endsWith(QStringLiteral("1"))) {
                        setMotorsChip(QStringLiteral("MOTORS DISARMED"),
                                      QColor(mutedColor(dark_mode_)));
                    }
                }
            });

    // ---- Mission lifecycle ----
    connect(mission_, &MissionController::logLine, this,
            &SatelliteScreen::appendLog);
    connect(mission_, &MissionController::missionStateChanged, this,
            [this](bool active) {
                map_->setEditLocked(active);
                send_button_->setEnabled(!active);
                end_button_->setEnabled(active);
                // Autonomy stays hard-blocked until the metadata push
                // lands (same arming-gate rule as the classic flow).
                autonomy_button_->setEnabled(active && metadata_pushed_);
                arm_button_->setEnabled(active);
                disarm_button_->setEnabled(active);
                estop_button_->setEnabled(active);
                add_roi_button_->setEnabled(!active);
                draw_polygon_button_->setEnabled(!active);
                place_robot_button_->setEnabled(!active);
                clear_roi_button_->setEnabled(!active);
                save_button_->setEnabled(!active);
                if (!active) {
                    stopMetadataPushLoop();
                    stopAutonomyLatch();
                    if (manager_watch_timer_) {
                        manager_watch_timer_->stop();
                    }
                    autonomy_on_ = false;
                    manager_occupancy_seen_ = false;
                    autonomy_button_->setText(QStringLiteral("Start Autonomy"));
                    map_->clearMissionAnchor();
                    map_->clearTelemetry();
                    setStatePill(QStringLiteral("NO MISSION"),
                                 QColor(mutedColor(dark_mode_)));
                    reason_label_->clear();
                    coverage_bar_->setVisible(false);
                }
                emit missionActiveChanged(active);
            });

    teleop_timer_ = new QTimer(this);
    teleop_timer_->setInterval(100);
    connect(teleop_timer_, &QTimer::timeout, this,
            &SatelliteScreen::publishTeleopTick);
    qApp->installEventFilter(this);

    metadata_timer_ = new QTimer(this);
    metadata_timer_->setInterval(3000);
    connect(metadata_timer_, &QTimer::timeout, this,
            &SatelliteScreen::attemptMetadataPush);

    // autonomy's /mpc_autonomy_enable is VOLATILE keep_last(1). A single
    // publish is lost if the coverage manager is still in its ctor when
    // the operator clicks Start (coordinator comes up first and unlocks
    // the button). Latch at 2 Hz so the false→true edge cannot be missed.
    autonomy_latch_timer_ = new QTimer(this);
    autonomy_latch_timer_->setInterval(500);
    connect(autonomy_latch_timer_, &QTimer::timeout, this, [this] {
        if (autonomy_on_ && ros_->isRunning()) {
            ros_->publishAutonomyEnable(true);
        }
    });

    manager_watch_timer_ = new QTimer(this);
    manager_watch_timer_->setSingleShot(true);
    manager_watch_timer_->setInterval(20000);
    connect(manager_watch_timer_, &QTimer::timeout, this, [this] {
        if (mission_->missionActive() && !manager_occupancy_seen_) {
            appendLog(QStringLiteral(
                "[send] no /coverage/global_occupancy after 20 s — the "
                "coverage manager may have died on roi_vertices parse. "
                "On the robot look for "
                "'Coverage horizon manager started'"));
        }
    });

    slow_timer_ = new QTimer(this);
    slow_timer_->setInterval(1000);
    connect(slow_timer_, &QTimer::timeout, this,
            &SatelliteScreen::updateBotPill);
    connect(slow_timer_, &QTimer::timeout, this,
            &SatelliteScreen::refreshImageryInfo);
    slow_timer_->start();

    setStatePill(QStringLiteral("NO MISSION"), QColor(mutedColor(true)));
    setBotPill(QStringLiteral("BOT —"), QColor(mutedColor(true)));
    setMotorsChip(QStringLiteral("MOTORS —"), QColor(mutedColor(true)));
    send_button_->setEnabled(true);
    end_button_->setEnabled(false);
    autonomy_button_->setEnabled(false);
    arm_button_->setEnabled(false);
    disarm_button_->setEnabled(false);
    estop_button_->setEnabled(false);

    // NOTE: no applyTheme() here. AppShellWindow::ensureStage6() always
    // calls setDarkMode() immediately after construction; applying a
    // default-theme stylesheet first left the scroll-area subtree polished
    // with the wrong palette (observed: light rail under a dark top bar).

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
}

// ---- Public API ---------------------------------------------------------------

bool SatelliteScreen::missionActive() const {
    return mission_->missionActive();
}

void SatelliteScreen::shutdownMission() {
    if (!mission_->missionActive()) {
        return;
    }
    setAutonomyEnabled(false);
    ros_->requestAxisState(RosLink::kAxisIdle);
    mission_->teardownMission();
}

void SatelliteScreen::attachLinkHealthMonitor(LinkHealthMonitor* monitor) {
    link_monitor_ = monitor;
}

void SatelliteScreen::startMetadataPushLoop() {
    metadata_pushed_ = false;
    metadata_attempts_ = 0;
    autonomy_button_->setEnabled(false);
    attemptMetadataPush();
    metadata_timer_->start();
}

void SatelliteScreen::stopMetadataPushLoop() {
    metadata_timer_->stop();
}

void SatelliteScreen::attemptMetadataPush() {
    if (metadata_pushed_ || !mission_->missionActive()) {
        stopMetadataPushLoop();
        return;
    }
    ++metadata_attempts_;
    QSettings settings(kSettingsOrgName, kSettingsAppName);
    const QString building =
        settings.value(kSettingsBuildingNameKey).toString();
    const QString operator_name =
        settings.value(kSettingsOperatorNameKey).toString();
    const QString units_pref =
        units::toString(UnitsProvider::instance()->units());
    ros_->pushSessionMetadata(
        building, operator_name, units_pref, [this](bool ok) {
            if (!mission_->missionActive() || metadata_pushed_) {
                return;
            }
            if (ok) {
                metadata_pushed_ = true;
                stopMetadataPushLoop();
                autonomy_button_->setEnabled(true);
                appendLog(QStringLiteral(
                    "[send] session metadata accepted by coordinator"));
                reason_label_->setText(QStringLiteral(
                    "Robot stack ready. Arm motors, then Start Autonomy."));
            } else if (metadata_attempts_ % 5 == 1) {
                appendLog(QStringLiteral(
                              "[send] waiting for coordinator (metadata "
                              "push, attempt %1)…")
                              .arg(metadata_attempts_));
            }
        });
}

void SatelliteScreen::configureForScan(const Job& job) {
    planning_only_ = false;
    plan_mode_ = job.isMeasured() ? PlanMode::Measured : PlanMode::Satellite;
    refreshJobsCombo(job.id);  // selects + loads the plan
    applyModeVisibility();
}

void SatelliteScreen::configureForScan(PlanMode mode) {
    planning_only_ = false;
    plan_mode_ = mode;
    refreshJobsCombo(QString());
    newJob();
    applyModeVisibility();
    if (mode == PlanMode::Measured) {
        // The measured canvas opens centered on its reference origin at a
        // scale where a typical roof fills the view (~60 m across).
        map_->setView(0.0, 0.0, kMeasuredDefaultZoom);
    }
}

void SatelliteScreen::devSeedDemoPlan() {
    if (plan_mode_ == PlanMode::Satellite) {
        // Somewhere real with buildings so the ROI reads against imagery.
        map_->setView(43.6007, -116.2497, 19);
    }
    map_->addRoiAtViewCenter();
    RoiRect roi = map_->roi();
    roi.length_m = 24.0;
    roi.width_m = 16.0;
    roi.heading_deg = 20.0;
    map_->setRoi(roi);

    // Seed a non-rectangular footprint: the rect mirror alone never exercises
    // the vertex handles, the per-edge dimension chips, or the roof-edge
    // colouring on an edge count other than four.
    RoiPolygon poly;
    const QVector<QPointF> corners_enu{{-12.0, -8.0}, {12.0, -8.0},
                                       {12.0, 4.0},   {2.0, 4.0},
                                       {2.0, 8.0},    {-12.0, 8.0}};
    for (const QPointF& enu : corners_enu) {
        poly.vertices.append(geo::geoFromEnu(roi.center, enu.x(), enu.y()));
    }
    poly.ensureEdgeFlags();
    poly.roof_edges[0] = true;
    poly.roof_edges[2] = true;
    map_->setPolygon(poly);

    geo::GeoPose marker;
    const geo::GeoPoint at = geo::geoFromEnu(roi.center, -6.0, -4.0);
    marker.lat = at.lat;
    marker.lon = at.lon;
    marker.heading_deg = 20.0;
    marker.valid = true;
    map_->setMarker(marker);
    emit map_->markerChanged();
}

void SatelliteScreen::configureForPlanning() {
    planning_only_ = true;
    plan_mode_ = PlanMode::Satellite;
    refreshJobsCombo(current_job_id_);
    applyModeVisibility();
}

void SatelliteScreen::applyModeVisibility() {
    const bool measured = plan_mode_ == PlanMode::Measured;
    map_->setImageryEnabled(!measured);
    if (lbl_title_) {
        lbl_title_->setText(planning_only_
                                ? QStringLiteral("Plan Job")
                                : (measured
                                       ? QStringLiteral("Measured ROI Scan")
                                       : QStringLiteral("Satellite ROI Scan")));
    }
    if (geo_tools_host_) {
        // Address search / tile download / imagery provenance are geographic
        // tools — meaningless on the measured (grid) canvas.
        geo_tools_host_->setVisible(!measured);
    }
    if (!measured) {
        // Entering (or re-entering) the satellite canvas: re-resolve
        // provenance for whatever view we land on.
        imagery_query_pending_ = true;
    }
    if (jobs_combo_row_) {
        // In the scan flow the plan was already chosen in ScanSetupDialog;
        // the in-screen selector is an office (planning-only) affordance.
        jobs_combo_row_->setVisible(planning_only_);
    }
    if (mission_card_) {
        mission_card_->setVisible(!planning_only_);
    }
    if (teleop_card_) {
        teleop_card_->setVisible(!planning_only_);
    }
}

void SatelliteScreen::setTopBatteryState(double pct, bool stale) {
    if (!lbl_top_battery_) {
        return;
    }
    last_batt_pct_ = pct;
    last_batt_stale_ = stale;
    if (stale || std::isnan(pct)) {
        lbl_top_battery_->setText(QStringLiteral("—%"));
        lbl_top_battery_->setStyleSheet(
            statusTextStyle(mutedColor(dark_mode_)) +
            QStringLiteral(" background: transparent;"));
        return;
    }
    QString color = textColor(dark_mode_);
    if (pct < 10.0) {
        color = QLatin1String(kEstopRed);
    } else if (pct < 20.0) {
        color = QLatin1String(kAmber);
    }
    lbl_top_battery_->setText(QStringLiteral("%1%").arg(qRound(pct)));
    lbl_top_battery_->setStyleSheet(
        statusTextStyle(color) + QStringLiteral(" background: transparent;"));
}

void SatelliteScreen::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!view_initialized_) {
        view_initialized_ = true;
        // Restore the last map view only when nothing was configured —
        // configureForScan(job) already centered on the plan's ROI.
        if (current_job_id_.isEmpty() && !map_->roi().valid) {
            QSettings settings(kSettingsOrgName, kSettingsAppName);
            map_->setView(
                settings.value(kSatViewLatKey, kDefaultLat).toDouble(),
                settings.value(kSatViewLonKey, kDefaultLon).toDouble(),
                settings.value(kSatViewZoomKey, kDefaultZoom).toInt());
        }
    }
}

// ---- UI construction ------------------------------------------------------------

QWidget* SatelliteScreen::buildTopBar() {
    auto* top_bar = new QWidget(this);
    top_bar->setObjectName("SatTopBar");
    top_bar->setAttribute(Qt::WA_StyledBackground, true);
    top_bar->setFixedHeight(kTopBarHeight);
    auto* layout = new QHBoxLayout(top_bar);
    layout->setContentsMargins(24, 0, 24, 0);
    layout->setSpacing(8);

    // Back button — the Stage 4/5 SVG construction (back_vector_a/b).
    auto* back = new QPushButton(top_bar);
    back->setObjectName("SatBackButton");
    back->setCursor(Qt::PointingHandCursor);
    back->setFixedSize(40, 28);
    auto* back_layout = new QHBoxLayout(back);
    back_layout->setContentsMargins(12, 6, 12, 6);
    back_layout->setSpacing(0);
    auto* back_icon = new QWidget(back);
    back_icon->setFixedSize(16, 16);
    auto* back_head = makeStatusIconLabel(
        back_icon, QStringLiteral(":/assets/exploration/back_vector_a.svg"), 11);
    back_head->setFixedSize(6, 11);
    back_head->move(3, 2);
    auto* back_line = makeStatusIconLabel(
        back_icon, QStringLiteral(":/assets/exploration/back_vector_b.svg"), 11);
    back_line->setFixedSize(11, 2);
    back_line->move(3, 7);
    back_layout->addWidget(back_icon, 0, Qt::AlignCenter);
    connect(back, &QPushButton::clicked, this, [this] {
        if (mission_->missionActive()) {
            appendLog(QStringLiteral(
                "[nav] mission active — End Mission before leaving"));
            return;
        }
        emit backRequested();
    });
    layout->addWidget(back, 0, Qt::AlignVCenter);

    lbl_title_ = new QLabel(QStringLiteral("Satellite ROI Scan"), top_bar);
    lbl_title_->setObjectName("SatTitle");
    layout->addWidget(lbl_title_, 0, Qt::AlignVCenter);
    layout->addStretch(1);

    // Status pills (right-aligned, 24px spacing like Stage 4/5).
    auto* status_host = new QWidget(top_bar);
    status_host->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    auto* status_layout = new QHBoxLayout(status_host);
    status_layout->setContentsMargins(0, 0, 0, 0);
    status_layout->setSpacing(24);

    const QString muted_style = statusTextStyle(mutedColor(true));
    status_layout->addWidget(makeStatusItem(
        status_host, QStringLiteral(":/assets/missionplanner/battery.svg"), 16,
        QStringLiteral("—%"), kTopStatusBatteryMinWidth, muted_style, nullptr,
        &lbl_top_battery_));
    status_layout->addWidget(makeStatusItem(
        status_host, QStringLiteral(":/assets/missionplanner/status_dot.svg"),
        8, QStringLiteral("BOT —"), kTopStatusPillMinWidth, muted_style,
        &lbl_bot_dot_, &lbl_bot_text_));
    status_layout->addWidget(makeStatusItem(
        status_host, QStringLiteral(":/assets/missionplanner/status_dot.svg"),
        8, QStringLiteral("NO MISSION"), kTopStatusPillMinWidth, muted_style,
        &lbl_state_dot_, &lbl_state_text_));

    // Motors chip — the Stage 4/5 96x20 bordered chip.
    motors_chip_ = new QWidget(status_host);
    motors_chip_->setObjectName("SatMotorsChip");
    motors_chip_->setFixedSize(kTopStatusMotorsChipMinWidth,
                               kTopStatusMotorsChipHeight);
    motors_chip_->setAttribute(Qt::WA_StyledBackground, true);
    auto* motors_layout = new QHBoxLayout(motors_chip_);
    motors_layout->setContentsMargins(9, 1, 9, 1);
    motors_layout->setSpacing(6);
    lbl_motors_dot_ = makeStatusIconLabel(
        motors_chip_, QStringLiteral(":/assets/missionplanner/status_dot.svg"),
        6, mutedColor(true));
    motors_layout->addWidget(lbl_motors_dot_, 0, Qt::AlignVCenter);
    lbl_motors_text_ = makeStatusTextLabel(
        motors_chip_, QStringLiteral("MOTORS —"),
        QStringLiteral("font-family: 'Arimo'; font-size: 10px; "
                       "font-weight: 700; letter-spacing: 0.5px; color: %1;")
            .arg(mutedColor(true)));
    motors_layout->addWidget(lbl_motors_text_, 0, Qt::AlignVCenter);
    status_layout->addWidget(motors_chip_);

    layout->addWidget(status_host, 0, Qt::AlignVCenter);
    layout->addSpacing(kTopStatusWindowControlsReservedWidth);
    return top_bar;
}

QWidget* SatelliteScreen::buildLeftRail() {
    auto* rail_content = new QWidget;
    rail_content->setObjectName("SatRail");
    rail_content->setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QVBoxLayout(rail_content);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);
    layout->addWidget(buildPlanCard(rail_content));
    mission_card_ = buildMissionCard(rail_content);
    layout->addWidget(mission_card_);
    teleop_card_ = buildTeleopCard(rail_content);
    layout->addWidget(teleop_card_);
    layout->addWidget(buildLogCard(rail_content));
    layout->addStretch(1);

    auto* scroll = new QScrollArea(this);
    scroll->setObjectName("SatRailScroll");
    scroll->setWidget(rail_content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFixedWidth(kLeftRailWidth);
    // The viewport must not paint its default Base brush over the rail's
    // themed background.
    scroll->viewport()->setAutoFillBackground(false);
    return scroll;
}

QWidget* SatelliteScreen::buildPlanCard(QWidget* parent) {
    auto* card = new QWidget(parent);
    card->setObjectName("SatCard");
    card->setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 16);
    layout->setSpacing(8);
    layout->addWidget(makeCardHeader(QStringLiteral(":/assets/exploration/map.svg"), QStringLiteral("Plan"), card));

    // Plan selector — office (planning-only) affordance.
    jobs_combo_ = new QComboBox(card);
    jobs_combo_->setObjectName("SatInput");
    jobs_combo_->setFixedHeight(36);
    connect(jobs_combo_, QOverload<int>::of(&QComboBox::activated), this,
            [this](int index) {
                const QString id = jobs_combo_->itemData(index).toString();
                for (const Job& job : jobs_) {
                    if (job.id == id) {
                        plan_mode_ = job.isMeasured() ? PlanMode::Measured
                                                      : PlanMode::Satellite;
                        loadJob(job);
                        applyModeVisibility();
                        return;
                    }
                }
                newJob();
            });
    jobs_combo_row_ = makeFieldRow(QStringLiteral("Saved plan"), jobs_combo_, card);
    layout->addWidget(jobs_combo_row_);

    job_name_ = new QLineEdit(card);
    job_name_->setObjectName("SatInput");
    job_name_->setPlaceholderText(QStringLiteral("Building / job name"));
    job_name_->setFixedHeight(36);
    layout->addWidget(job_name_);

    job_address_ = new QLineEdit(card);
    job_address_->setObjectName("SatInput");
    job_address_->setPlaceholderText(QStringLiteral("Address (reference)"));
    job_address_->setFixedHeight(36);
    layout->addWidget(job_address_);

    // Geographic tools (satellite canvas only).
    geo_tools_host_ = new QWidget(card);
    auto* geo_layout = new QVBoxLayout(geo_tools_host_);
    geo_layout->setContentsMargins(0, 0, 0, 0);
    geo_layout->setSpacing(8);
    auto* search_row = new QWidget(geo_tools_host_);
    auto* search_layout = new QHBoxLayout(search_row);
    search_layout->setContentsMargins(0, 0, 0, 0);
    search_layout->setSpacing(8);
    address_edit_ = new QLineEdit(search_row);
    address_edit_->setObjectName("SatInput");
    address_edit_->setPlaceholderText(QStringLiteral("Find address or \"lat, lon\""));
    address_edit_->setFixedHeight(36);
    connect(address_edit_, &QLineEdit::returnPressed, this,
            &SatelliteScreen::onGoToAddress);
    search_layout->addWidget(address_edit_, 1);
    auto* go = new QPushButton(QStringLiteral("Go"), search_row);
    go->setObjectName("SatButton");
    go->setFixedHeight(36);
    go->setCursor(Qt::PointingHandCursor);
    connect(go, &QPushButton::clicked, this, &SatelliteScreen::onGoToAddress);
    search_layout->addWidget(go);
    geo_layout->addWidget(search_row);
    auto* download =
        new QPushButton(QStringLiteral("Download Area…"), geo_tools_host_);
    download->setObjectName("SatButton");
    download->setFixedHeight(36);
    download->setCursor(Qt::PointingHandCursor);
    connect(download, &QPushButton::clicked, this,
            &SatelliteScreen::onDownloadArea);
    geo_layout->addWidget(download);

    // Source-imagery provenance. Lives inside geo_tools_host_ so
    // applyModeVisibility() hides it on the measured canvas for free.
    imagery_label_ = new QLabel(QStringLiteral("Imagery: —"), geo_tools_host_);
    imagery_label_->setObjectName("SatFieldLabel");
    imagery_label_->setWordWrap(true);
    geo_layout->addWidget(imagery_label_);

    layout->addWidget(geo_tools_host_);

    auto* draw_row = new QWidget(card);
    auto* draw_layout = new QHBoxLayout(draw_row);
    draw_layout->setContentsMargins(0, 0, 0, 0);
    draw_layout->setSpacing(8);
    add_roi_button_ = new QPushButton(QStringLiteral("Add ROI"), draw_row);
    draw_polygon_button_ =
        new QPushButton(QStringLiteral("Draw Shape"), draw_row);
    for (QPushButton* button : {add_roi_button_, draw_polygon_button_}) {
        button->setObjectName("SatButton");
        button->setFixedHeight(36);
        button->setCursor(Qt::PointingHandCursor);
        draw_layout->addWidget(button, 1);
    }
    connect(add_roi_button_, &QPushButton::clicked, this,
            [this] { map_->addRoiAtViewCenter(); });
    connect(draw_polygon_button_, &QPushButton::clicked, this, [this] {
        map_->armPolygonDraw();
        appendLog(QStringLiteral(
            "[plan] click each roof corner in order; right-click to close"));
    });
    layout->addWidget(draw_row);

    auto* draw_row2 = new QWidget(card);
    auto* draw_layout2 = new QHBoxLayout(draw_row2);
    draw_layout2->setContentsMargins(0, 0, 0, 0);
    draw_layout2->setSpacing(8);
    place_robot_button_ = new QPushButton(QStringLiteral("Place Robot"), draw_row2);
    clear_roi_button_ = new QPushButton(QStringLiteral("Clear ROI"), draw_row2);
    for (QPushButton* button : {place_robot_button_, clear_roi_button_}) {
        button->setObjectName("SatButton");
        button->setFixedHeight(36);
        button->setCursor(Qt::PointingHandCursor);
        draw_layout2->addWidget(button, 1);
    }
    connect(place_robot_button_, &QPushButton::clicked, this, [this] {
        map_->armMarkerPlacement();
        appendLog(QStringLiteral(
            "[plan] click the canvas where the robot physically sits"));
    });
    connect(clear_roi_button_, &QPushButton::clicked, this, [this] {
        map_->setRoi(RoiRect{});
        map_->clearPolygon();
        emit map_->roiChanged();
    });
    layout->addWidget(draw_row2);

    const QString length_suffix =
        QStringLiteral(" ") + units::lengthUnitSuffix();
    roi_length_ = makeSpin(2.0, 2000.0, 0.5, 1, length_suffix);
    roi_width_ = makeSpin(2.0, 2000.0, 0.5, 1, length_suffix);
    roi_heading_ = makeSpin(0.0, 359.9, 1.0, 1, QStringLiteral(" °"));
    roi_heading_->setWrapping(true);
    robot_heading_ = makeSpin(0.0, 359.9, 1.0, 1, QStringLiteral(" °"));
    robot_heading_->setWrapping(true);
    layout->addWidget(makeFieldRow(QStringLiteral("ROI along"), roi_length_, card));
    layout->addWidget(makeFieldRow(QStringLiteral("ROI across"), roi_width_, card));
    layout->addWidget(makeFieldRow(QStringLiteral("ROI heading"), roi_heading_, card));
    layout->addWidget(
        makeFieldRow(QStringLiteral("Robot heading"), robot_heading_, card));

    const auto pushRoi = [this] {
        RoiRect roi = map_->roi();
        if (!roi.valid) {
            return;
        }
        // Spins display the operator's units; the model stays SI.
        roi.length_m = spinToMeters(roi_length_->value());
        roi.width_m = spinToMeters(roi_width_->value());
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
    robot_pos_label_->setObjectName("SatFieldLabel");
    robot_pos_label_->setWordWrap(true);
    layout->addWidget(robot_pos_label_);

    auto* edge_hint = new QLabel(
        QStringLiteral("Tap an ROI edge to mark it as a roof edge — red "
                       "edges are physical fall hazards and get a larger "
                       "setback."),
        card);
    edge_hint->setObjectName("SatFieldLabel");
    edge_hint->setWordWrap(true);
    layout->addWidget(edge_hint);

    save_button_ = new QPushButton(QStringLiteral("Save Plan"), card);
    save_button_->setObjectName("SatButton");
    save_button_->setFixedHeight(36);
    save_button_->setCursor(Qt::PointingHandCursor);
    connect(save_button_, &QPushButton::clicked, this,
            &SatelliteScreen::saveJob);
    layout->addWidget(save_button_);
    return card;
}

QWidget* SatelliteScreen::buildMissionCard(QWidget* parent) {
    auto* card = new QWidget(parent);
    card->setObjectName("SatCard");
    card->setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 16);
    layout->setSpacing(8);
    layout->addWidget(makeCardHeader(QStringLiteral(":/assets/exploration/start_scan.svg"), QStringLiteral("Mission"), card));

    reason_label_ = new QLabel(card);
    reason_label_->setObjectName("SatFieldLabel");
    reason_label_->setWordWrap(true);
    layout->addWidget(reason_label_);

    coverage_bar_ = new QProgressBar(card);
    coverage_bar_->setObjectName("SatCoverage");
    coverage_bar_->setRange(0, 1000);
    coverage_bar_->setFormat(QStringLiteral("coverage %p%"));
    coverage_bar_->setFixedHeight(18);
    coverage_bar_->setVisible(false);
    layout->addWidget(coverage_bar_);

    segment_label_ = new QLabel(card);
    segment_label_->setObjectName("SatFieldLabel");
    layout->addWidget(segment_label_);

    send_button_ = new QPushButton(QStringLiteral("Send to Robot"), card);
    send_button_->setObjectName("SatSendButton");
    send_button_->setFixedHeight(kSendButtonHeight);
    send_button_->setCursor(Qt::PointingHandCursor);
    connect(send_button_, &QPushButton::clicked, this,
            &SatelliteScreen::onSendMission);
    layout->addWidget(send_button_);

    auto* row1 = new QWidget(card);
    auto* row1_layout = new QHBoxLayout(row1);
    row1_layout->setContentsMargins(0, 0, 0, 0);
    row1_layout->setSpacing(8);
    autonomy_button_ = new QPushButton(QStringLiteral("Start Autonomy"), row1);
    end_button_ = new QPushButton(QStringLiteral("End Mission"), row1);
    for (QPushButton* button : {autonomy_button_, end_button_}) {
        button->setObjectName("SatButton");
        button->setFixedHeight(36);
        button->setCursor(Qt::PointingHandCursor);
        row1_layout->addWidget(button, 1);
    }
    connect(autonomy_button_, &QPushButton::clicked, this, [this] {
        setAutonomyEnabled(!autonomy_on_);
    });
    connect(end_button_, &QPushButton::clicked, this,
            &SatelliteScreen::onEndMission);
    layout->addWidget(row1);

    auto* row2 = new QWidget(card);
    auto* row2_layout = new QHBoxLayout(row2);
    row2_layout->setContentsMargins(0, 0, 0, 0);
    row2_layout->setSpacing(8);
    arm_button_ = new QPushButton(QStringLiteral("Arm Motors"), row2);
    disarm_button_ = new QPushButton(QStringLiteral("Disarm"), row2);
    for (QPushButton* button : {arm_button_, disarm_button_}) {
        button->setObjectName("SatButton");
        button->setFixedHeight(36);
        button->setCursor(Qt::PointingHandCursor);
        row2_layout->addWidget(button, 1);
    }
    connect(arm_button_, &QPushButton::clicked, this, [this] {
        ros_->requestAxisState(RosLink::kAxisClosedLoop);
        appendLog(QStringLiteral("[cmd] arm (CLOSED_LOOP_CONTROL)"));
    });
    connect(disarm_button_, &QPushButton::clicked, this, [this] {
        ros_->requestAxisState(RosLink::kAxisIdle);
        appendLog(QStringLiteral("[cmd] disarm (IDLE)"));
    });
    layout->addWidget(row2);

    estop_button_ = new QPushButton(QStringLiteral("Emergency Stop"), card);
    estop_button_->setObjectName("SatEstopButton");
    estop_button_->setFixedHeight(kEstopButtonHeight);
    estop_button_->setCursor(Qt::PointingHandCursor);
    connect(estop_button_, &QPushButton::clicked, this,
            &SatelliteScreen::onEstop);
    layout->addWidget(estop_button_);
    return card;
}

QWidget* SatelliteScreen::buildTeleopCard(QWidget* parent) {
    auto* card = new QWidget(parent);
    card->setObjectName("SatCard");
    card->setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 16);
    layout->setSpacing(8);
    layout->addWidget(makeCardHeader(QStringLiteral(":/assets/exploration/telemetry.svg"), QStringLiteral("Teleop"), card));

    teleop_check_ = new QCheckBox(QStringLiteral("Enable keyboard teleop"), card);
    teleop_check_->setObjectName("SatCheck");
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

    auto* row = new QWidget(card);
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(0, 0, 0, 0);
    row_layout->setSpacing(8);
    auto* label = new QLabel(QStringLiteral("Speed"), row);
    label->setObjectName("SatFieldLabel");
    teleop_speed_ = new QSlider(Qt::Horizontal, row);
    teleop_speed_->setObjectName("SatSlider");
    teleop_speed_->setRange(10, 80);
    teleop_speed_->setValue(35);
    teleop_speed_label_ = new QLabel(QStringLiteral("0.35 m/s"), row);
    teleop_speed_label_->setObjectName("SatFieldLabel");
    connect(teleop_speed_, &QSlider::valueChanged, this, [this](int value) {
        teleop_speed_label_->setText(
            QStringLiteral("%1 m/s").arg(value / 100.0, 0, 'f', 2));
    });
    row_layout->addWidget(label);
    row_layout->addWidget(teleop_speed_, 1);
    row_layout->addWidget(teleop_speed_label_);
    layout->addWidget(row);
    return card;
}

QWidget* SatelliteScreen::buildLogCard(QWidget* parent) {
    auto* card = new QWidget(parent);
    card->setObjectName("SatCard");
    card->setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 16);
    layout->setSpacing(8);
    layout->addWidget(makeCardHeader(QStringLiteral(":/assets/exploration/standby.svg"), QStringLiteral("Log"), card));

    log_view_ = new QPlainTextEdit(card);
    log_view_->setObjectName("SatLog");
    log_view_->setReadOnly(true);
    log_view_->setMaximumBlockCount(600);
    log_view_->setFixedHeight(104);
    log_view_->setFrameShape(QFrame::NoFrame);
    layout->addWidget(log_view_);
    return card;
}

// ---- Theming --------------------------------------------------------------------

void SatelliteScreen::applyTheme() {
    const bool dark = dark_mode_;
    const QString page_bg = dark ? QStringLiteral("#0b0b0b")
                                 : QStringLiteral("#F9FAFB");
    const QString surface = dark ? QStringLiteral("#18181b")
                                 : QStringLiteral("#FFFFFF");
    const QString surface_border = dark ? QStringLiteral("#27272a")
                                        : QStringLiteral("#D1D5DC");
    const QString card_border = dark ? QStringLiteral("#3f3f47")
                                     : QStringLiteral("#D1D5DC");
    const QString input_bg = dark ? QStringLiteral("#27272a")
                                  : QStringLiteral("#FFFFFF");
    const QString input_border = dark ? QStringLiteral("#3f3f47")
                                      : QStringLiteral("#D1D5DC");
    const QString button_bg = dark ? QStringLiteral("#3f3f47")
                                   : QStringLiteral("#FFFFFF");
    const QString button_hover = dark ? QStringLiteral("#4a4a52")
                                      : QStringLiteral("#F3F4F6");
    const QString log_bg = dark ? QStringLiteral("#101014")
                                : QStringLiteral("#F1F5F9");
    const QString text = textColor(dark);
    const QString muted = mutedColor(dark);

    // Every rule is scoped to an object name — no cascading bare selectors.
    // Named-token substitution (not QString::arg chains) so a missed
    // placeholder can never silently truncate the theme.
    QString qss = QStringLiteral(R"QSS(
#SatelliteScreen { background-color: @PAGE@; }
#SatTopBar { background-color: @SURFACE@; border-bottom: 1px solid @SURFACE_BORDER@; }
#SatTitle {
    font-family: 'Arimo'; font-weight: 700; font-size: 18px; color: @TEXT@;
    background: transparent;
}
#SatBackButton {
    background-color: transparent; border: 1px solid @CARD_BORDER@; border-radius: 8px;
}
#SatBackButton:hover { background-color: @BUTTON_HOVER@; }
#SatMotorsChip {
    background-color: transparent; border: 1px solid @CARD_BORDER@; border-radius: 10px;
}
#SatRailScroll { background-color: @PAGE@; border: none; border-right: 1px solid @SURFACE_BORDER@; }
#SatRail { background-color: @PAGE@; }
#SatCard {
    background-color: @SURFACE@; border: 1px solid @CARD_BORDER@; border-radius: 10px;
}
#SatCardHeader {
    font-family: 'Arimo'; font-weight: 700; font-size: 14px;
    color: @TEXT@; background: transparent;
}
#SatFieldLabel {
    font-family: 'Arimo'; font-size: 12px; color: @MUTED@; background: transparent;
}
QLineEdit#SatInput, QDoubleSpinBox#SatInput, QComboBox#SatInput {
    background-color: @INPUT_BG@; border: 1px solid @INPUT_BORDER@; border-radius: 10px;
    padding: 0 12px; font-family: 'Arimo'; font-size: 14px; color: @TEXT@;
    selection-background-color: rgba(0, 188, 125, 0.30);
}
QLineEdit#SatInput:focus, QDoubleSpinBox#SatInput:focus,
QComboBox#SatInput:focus { border-color: #00BC7D; }
QComboBox#SatInput::drop-down { border: none; width: 24px; }
QComboBox#SatInput QAbstractItemView {
    background-color: @INPUT_BG@; border: 1px solid @INPUT_BORDER@; color: @TEXT@;
    selection-background-color: rgba(0, 188, 125, 0.30);
}
QPushButton#SatButton {
    background-color: @BUTTON_BG@; border: 1px solid @INPUT_BORDER@; border-radius: 10px;
    font-family: 'Arimo'; font-weight: 600; font-size: 13px; color: @TEXT@;
    padding: 0 12px;
}
QPushButton#SatButton:hover { background-color: @BUTTON_HOVER@; }
QPushButton#SatButton:disabled { color: @MUTED@; background-color: transparent; }
QPushButton#SatSendButton {
    background-color: #00BC7D; border: none; border-radius: 10px;
    font-family: 'Arimo'; font-weight: 700; font-size: 14px; color: #FFFFFF;
}
QPushButton#SatSendButton:hover { background-color: #00A86D; }
QPushButton#SatSendButton:disabled { background-color: @BUTTON_BG@; color: @MUTED@; }
QPushButton#SatEstopButton {
    background-color: rgba(231, 0, 11, 0.12); border: 1px solid #E7000B;
    border-radius: 10px; font-family: 'Arimo'; font-weight: 700;
    font-size: 14px; color: @ESTOP_TEXT@;
}
QPushButton#SatEstopButton:hover { background-color: rgba(231, 0, 11, 0.24); }
QPushButton#SatEstopButton:disabled {
    background-color: transparent; border-color: @INPUT_BORDER@; color: @MUTED@;
}
QCheckBox#SatCheck {
    font-family: 'Arimo'; font-size: 13px; color: @TEXT@; background: transparent;
}
QCheckBox#SatCheck::indicator {
    width: 16px; height: 16px; border: 1px solid @INPUT_BORDER@; border-radius: 4px;
    background-color: @INPUT_BG@;
}
QCheckBox#SatCheck::indicator:checked {
    background-color: #00BC7D; border-color: #00BC7D;
}
QSlider#SatSlider::groove:horizontal {
    height: 4px; background: @INPUT_BORDER@; border-radius: 2px;
}
QSlider#SatSlider::handle:horizontal {
    width: 14px; height: 14px; margin: -5px 0; border-radius: 7px;
    background: #00BC7D;
}
QProgressBar#SatCoverage {
    background-color: @INPUT_BG@; border: 1px solid @INPUT_BORDER@; border-radius: 6px;
    text-align: center; font-family: 'Arimo'; font-size: 11px; color: @TEXT@;
}
QProgressBar#SatCoverage::chunk { background-color: #00BC7D; border-radius: 5px; }
QPlainTextEdit#SatLog {
    background-color: @LOG_BG@; border: 1px solid @INPUT_BORDER@; border-radius: 8px;
    color: @MUTED@; font-family: monospace; font-size: 11px;
}
)QSS");
    qss.replace(QStringLiteral("@PAGE@"), page_bg);
    qss.replace(QStringLiteral("@SURFACE_BORDER@"), surface_border);
    qss.replace(QStringLiteral("@SURFACE@"), surface);
    qss.replace(QStringLiteral("@TEXT@"), text);
    qss.replace(QStringLiteral("@CARD_BORDER@"), card_border);
    qss.replace(QStringLiteral("@BUTTON_HOVER@"), button_hover);
    qss.replace(QStringLiteral("@BUTTON_BG@"), button_bg);
    qss.replace(QStringLiteral("@MUTED@"), muted);
    qss.replace(QStringLiteral("@INPUT_BORDER@"), input_border);
    qss.replace(QStringLiteral("@INPUT_BG@"), input_bg);
    qss.replace(QStringLiteral("@LOG_BG@"), log_bg);
    qss.replace(QStringLiteral("@ESTOP_TEXT@"),
                dark ? QStringLiteral("#FF6467") : QStringLiteral("#E7000B"));
    setStyleSheet(qss);

    // Replacing an ancestor stylesheet does not reliably repolish widgets
    // inside the QScrollArea subtree (observed: rail keeping the previous
    // palette). Force a deterministic repolish of every descendant.
    const QList<QWidget*> descendants = findChildren<QWidget*>();
    for (QWidget* child : descendants) {
        child->style()->unpolish(child);
        child->style()->polish(child);
    }
    update();
}

void SatelliteScreen::setDarkMode(bool dark_mode) {
    const QString old_muted = mutedColor(dark_mode_);
    dark_mode_ = dark_mode;
    applyTheme();
    // Re-render every dynamic surface against the new palette. Pills whose
    // color was the old palette's muted tone follow to the new muted tone;
    // semantic colors (accent/amber/red) are theme-independent.
    const auto remap = [&](const QColor& color) {
        return color.name().compare(old_muted, Qt::CaseInsensitive) == 0
                   ? QColor(mutedColor(dark_mode_))
                   : color;
    };
    setBotPill(bot_text_, remap(bot_color_));
    setStatePill(state_text_, remap(state_color_));
    setMotorsChip(motors_text_, remap(motors_color_));
    setTopBatteryState(last_batt_pct_, last_batt_stale_);
    // The imagery label carries an inline colour too. Re-resolving is free
    // (the provenance cache answers without a request) and repaints it
    // against the new palette on the next slow_timer_ tick.
    imagery_query_pending_ = true;
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
    if (job.polygon.valid()) {
        map_->setPolygon(job.polygon);
    }
    map_->setMarker(job.robot);
    emit map_->roiChanged();
    emit map_->markerChanged();

    // Offline first: point the canvas at this job's prefetched pyramid before
    // choosing a view, so the very first paint comes off disk.
    if (job.imagery_cache.cached && !job.isMeasured()) {
        applyImageryManifest(
            TileService::readSiteManifest(job_store_.assetsDir(job.id) +
                                          QStringLiteral("/imagery.json")),
            job_store_.assetsDir(job.id));
    }

    const int geo_zoom = job.imagery_cache.cached && job.imagery_cache.max_zoom > 0
                             ? job.imagery_cache.max_zoom
                             : 19;
    if (job.polygon.valid()) {
        map_->setView(job.polygon.vertices.first().lat,
                      job.polygon.vertices.first().lon,
                      job.isMeasured() ? kMeasuredDefaultZoom : geo_zoom);
    } else if (job.roi.valid) {
        map_->setView(job.roi.center.lat, job.roi.center.lon,
                      job.isMeasured() ? kMeasuredDefaultZoom : geo_zoom);
    } else if (job.gps.valid && !job.isMeasured()) {
        // No geometry yet, but the robot reported a fix during map collection
        // — that is the best seed we have for where the site actually is.
        map_->setView(job.gps.lat, job.gps.lon, geo_zoom);
    } else if (job.isMeasured()) {
        map_->setView(0.0, 0.0, kMeasuredDefaultZoom);
    }
    appendLog(QStringLiteral("[plan] loaded '%1'").arg(job.name));
}

void SatelliteScreen::newJob() {
    current_job_id_.clear();
    jobs_combo_->setCurrentIndex(0);
    job_name_->clear();
    job_address_->clear();
    map_->setRoi(RoiRect{});
    map_->clearPolygon();
    map_->setMarker(geo::GeoPose{});
    emit map_->roiChanged();
    emit map_->markerChanged();
}

void SatelliteScreen::saveJob() {
    Job job;
    job.name = job_name_->text().trimmed();
    if (job.name.isEmpty()) {
        appendLog(QStringLiteral("[plan] give the plan a name before saving"));
        return;
    }
    job.address = job_address_->text().trimmed();
    job.mode = QString::fromLatin1(plan_mode_ == PlanMode::Measured
                                       ? Job::kModeMeasured
                                       : Job::kModeSatellite);
    job.roi = map_->roi();
    job.polygon = map_->polygon();
    job.robot = map_->marker();
    job.updated = QDateTime::currentDateTime();
    // Stamp what this plan was actually drawn against. Only meaningful on the
    // satellite canvas — a measured plan came off a tape, not off imagery.
    if (plan_mode_ == PlanMode::Satellite) {
        job.imagery_captured = last_imagery_captured_;
        job.imagery_res_m = last_imagery_res_m_;
        job.imagery_zoom = last_imagery_zoom_;
    }
    if (current_job_id_.isEmpty()) {
        job.id = JobStore::slugify(job.name) + QStringLiteral("_") +
                 QUuid::createUuid().toString(QUuid::Id128).left(6);
        job.created = job.updated;
    } else {
        job.id = current_job_id_;
        for (const Job& existing : jobs_) {
            if (existing.id == job.id) {
                job.created = existing.created;
                job.last_executed_at = existing.last_executed_at;
                // Fields the plan card does not own. They are produced by the
                // prefetch / map-collection / alignment steps, so re-saving
                // from the rail must not wipe them.
                job.gps = existing.gps;
                job.imagery_cache = existing.imagery_cache;
                job.alignment = existing.alignment;
                job.align_rmse_m = existing.align_rmse_m;
                break;
            }
        }
    }
    QString error;
    if (!job_store_.save(job, &error)) {
        appendLog(QStringLiteral("[plan] save failed: %1").arg(error));
        return;
    }
    current_job_id_ = job.id;
    refreshJobsCombo(job.id);
    appendLog(QStringLiteral("[plan] saved '%1'").arg(job.name));
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

void SatelliteScreen::refreshImageryInfo() {
    if (!imagery_label_ || !imagery_query_pending_) {
        return;
    }
    if (plan_mode_ == PlanMode::Measured) {
        // Measured canvas has no imagery under it at all.
        imagery_query_pending_ = false;
        return;
    }
    imagery_query_pending_ = false;

    const int zoom = map_->zoom();
    tiles_->imageryInfoAt(
        map_->centerLat(), map_->centerLon(), zoom,
        [this, zoom](ImageryInfo info) {
            if (!imagery_label_) {
                return;
            }
            // Always set an explicit colour rather than clearing the
            // stylesheet: an emptied inline sheet does not reliably repolish
            // back to the card's #SatFieldLabel rule (same trap applyTheme's
            // repolish sweep exists for).
            const QString normal_color =
                QStringLiteral("color: %1;").arg(mutedColor(dark_mode_));
            if (!info.valid) {
                imagery_label_->setText(
                    QStringLiteral("Imagery: capture date unavailable"));
                imagery_label_->setStyleSheet(normal_color);
                return;
            }

            const bool changed = info.captured != last_imagery_captured_;
            last_imagery_captured_ = info.captured;
            last_imagery_res_m_ = info.src_res_m;
            last_imagery_zoom_ = zoom;

            const int age = info.ageYears();
            // Three years is roughly the window in which a commercial roof
            // can be re-covered, re-penetrated, or have its HVAC swapped —
            // past that the operator should not trust what they are drawing
            // around. Advisory only: imagery is a drawing surface, not an
            // accuracy input (the ROI exports in the robot's body frame).
            const bool stale = age >= 3;

            QString detail = info.captured.toString(Qt::ISODate);
            if (info.src_res_m > 0.0) {
                detail += QStringLiteral(" · %1 cm")
                              .arg(info.src_res_m * 100.0, 0, 'f', 0);
            }
            if (age >= 0) {
                detail += QStringLiteral(" · %1 yr old").arg(age);
            }
            imagery_label_->setText(QStringLiteral("Imagery: %1").arg(detail));
            imagery_label_->setStyleSheet(
                stale ? QStringLiteral("color: %1;").arg(QLatin1String(kAmber))
                      : normal_color);

            if (changed) {
                // Log it: the whole point is that this changes silently as
                // the operator zooms, because different LODs are served from
                // different flights.
                appendLog(QStringLiteral("[imagery] z%1 · %2%3%4")
                              .arg(zoom)
                              .arg(detail,
                                   info.source.isEmpty()
                                       ? QString()
                                       : QStringLiteral(" · %1").arg(info.source),
                                   stale ? QStringLiteral("  ** STALE **")
                                         : QString()));
            }
        });
}

void SatelliteScreen::onDownloadArea() {
    // The prefetch is per-job: tiles, the stitched site image, and the
    // imagery manifest all live under the job's assets folder so the whole
    // site travels with the plan and uploads with the mission. That needs an
    // id, so an unsaved plan is saved first.
    if (current_job_id_.isEmpty()) {
        saveJob();
    }
    if (current_job_id_.isEmpty()) {
        appendLog(QStringLiteral(
            "[geo] name and save the plan before downloading — cached "
            "imagery is stored per job"));
        return;
    }
    const QString assets = job_store_.assetsDir(current_job_id_);
    const QString shared_cache = tiles_->cacheRoot();

    DownloadAreaDialog dialog(tiles_, map_->centerLat(), map_->centerLon(),
                              this);
    dialog.setAssetsDir(assets);
    connect(&dialog, &DownloadAreaDialog::areaReady, this,
            [this](double lat, double lon) { map_->setView(lat, lon, 18); });
    dialog.exec();

    // Adopt whatever the dialog actually cached, then hand the canvas back to
    // the job's tile tree so the operator sees the offline pyramid, not the
    // shared scratch cache.
    const TileService::SiteManifest manifest = TileService::readSiteManifest(
        assets + QStringLiteral("/imagery.json"));
    if (manifest.cached) {
        applyImageryManifest(manifest, assets);
        for (Job& job : jobs_) {
            if (job.id != current_job_id_) {
                continue;
            }
            job.imagery_cache.cached = true;
            job.imagery_cache.max_zoom = manifest.max_zoom;
            job.imagery_cache.captured = manifest.captured;
            job.imagery_cache.res_m = manifest.res_m;
            job.imagery_cache.layer = manifest.layer;
            job.imagery_cache.wayback_release = manifest.wayback_release;
            job.imagery_cache.min_date = manifest.min_date;
            job.imagery_cache.stitch_relpath = manifest.stitch_relpath;
            job_store_.save(job);
            break;
        }
        appendLog(QStringLiteral("[geo] offline imagery ready (z%1, %2)")
                      .arg(manifest.max_zoom)
                      .arg(manifest.captured.isValid()
                               ? manifest.captured.toString(Qt::ISODate)
                               : QStringLiteral("date unknown")));
    } else {
        tiles_->setCacheRoot(shared_cache);
    }
}

void SatelliteScreen::applyImageryManifest(
    const TileService::SiteManifest& manifest, const QString& assets_dir) {
    tiles_->setCacheRoot(assets_dir + QStringLiteral("/tiles"));
    tiles_->setLayer(manifest.layer == QLatin1String("clarity")
                         ? TileService::ImageryLayer::Clarity
                         : TileService::ImageryLayer::World);
    tiles_->setWaybackRelease(manifest.wayback_release);
    // Past the prefetched ceiling there is nothing on disk and no internet on
    // the roof, so the canvas must not let the operator zoom into blanks.
    tiles_->setMaxZoomCap(manifest.max_zoom);
}

// ---- Mission ----------------------------------------------------------------

bool SatelliteScreen::confirmDialog(const QString& title, const QString& body,
                                    const QString& accept_label) {
    QDialog dialog(this);
    dialog.setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    dialog.setModal(true);
    dialog.setMinimumWidth(430);
    dialog.setObjectName("SatConfirmDialog");
    // Modals are dark-only across the app (see MissionMetadataDialog).
    dialog.setStyleSheet(QStringLiteral(
        "#SatConfirmDialog { background-color: #18181b; "
        "border: 1px solid #27272a; border-radius: 10px; }"
        "QLabel { color: #FAFAFA; font-family: 'Arimo'; "
        "background: transparent; }"
        "QPushButton { background-color: #3f3f47; border: none; "
        "border-radius: 10px; padding: 10px 20px; color: #FAFAFA; "
        "font-family: 'Arimo'; font-weight: 600; font-size: 14px; }"
        "QPushButton:hover { background-color: #4a4a52; }"
        "QPushButton#Accept { background-color: #00BC7D; color: #FFFFFF; "
        "font-weight: 700; }"
        "QPushButton#Accept:hover { background-color: #00A86D; }"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(12);

    auto* title_label = new QLabel(title, &dialog);
    title_label->setStyleSheet(QStringLiteral(
        "font-family: 'Arimo'; font-weight: 700; font-size: 20px; "
        "color: #FAFAFA; background: transparent;"));
    layout->addWidget(title_label);

    auto* body_label = new QLabel(body, &dialog);
    body_label->setWordWrap(true);
    body_label->setStyleSheet(QStringLiteral(
        "font-family: 'Arimo'; font-size: 14px; color: #D4D4D8; "
        "background: transparent;"));
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
    // The polygon is the authored geometry; the rectangle is only its
    // four-corner special case kept for the rail's length/width spinboxes.
    const RoiPolygon poly = map_->polygon().valid()
                                ? map_->polygon()
                                : RoiPolygon::fromRect(map_->roi());
    const geo::GeoPose marker = map_->marker();
    if (!poly.valid() || !marker.valid) {
        appendLog(QStringLiteral(
            "[send] draw the ROI and place the robot marker first"));
        return;
    }
    const QString roi_arg =
        MissionController::roiVerticesArgument(poly, marker);
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
    if (!mission_->startMission(poly, marker, &error)) {
        appendLog(QStringLiteral("[send] FAILED: %1").arg(error));
        return;
    }
    map_->setMissionAnchor(marker);
    setStatePill(QStringLiteral("LAUNCHING"), QColor(kAmber));
    reason_label_->setText(
        QStringLiteral("Launching robot stack… Start Autonomy unlocks once "
                       "the session metadata push is accepted."));
    // The arming gate: autonomy stays locked until the coordinator accepts
    // building/operator/units (retried while the stack boots).
    startMetadataPushLoop();
    manager_occupancy_seen_ = false;
    manager_watch_timer_->start();

    // Stamp the plan as executed — drives the "LAST RUN" chip in the
    // Scan Setup list.
    if (!current_job_id_.isEmpty()) {
        for (Job& job : jobs_) {
            if (job.id == current_job_id_) {
                job.last_executed_at = QDateTime::currentDateTime();
                job_store_.save(job);
                break;
            }
        }
    }
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
    setAutonomyEnabled(false);
    ros_->requestAxisState(RosLink::kAxisIdle);
    appendLog(
        QStringLiteral("[E-STOP] autonomy disabled + axis IDLE requested"));
    setStatePill(QStringLiteral("E-STOP"), QColor(kEstopRed));
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

void SatelliteScreen::setAutonomyEnabled(bool enabled) {
    const bool rising = enabled && !autonomy_on_;
    autonomy_on_ = enabled;
    autonomy_button_->setText(autonomy_on_
                                  ? QStringLiteral("Pause Autonomy")
                                  : QStringLiteral("Start Autonomy"));
    if (autonomy_on_) {
        teleop_check_->setChecked(false);
        // Force a false→true edge. The manager only starts planning on
        // that edge; a lone true can be dropped while it is still
        // constructing, and later trues are ignored.
        if (rising) {
            ros_->publishAutonomyEnable(false);
        }
        ros_->publishAutonomyEnable(true);
        startAutonomyLatch();
    } else {
        stopAutonomyLatch();
        ros_->publishAutonomyEnable(false);
    }
    appendLog(QStringLiteral("[cmd] autonomy_enable=%1")
                  .arg(autonomy_on_ ? QStringLiteral("true")
                                    : QStringLiteral("false")));
}

void SatelliteScreen::startAutonomyLatch() {
    if (autonomy_latch_timer_ && !autonomy_latch_timer_->isActive()) {
        autonomy_latch_timer_->start();
    }
}

void SatelliteScreen::stopAutonomyLatch() {
    if (autonomy_latch_timer_) {
        autonomy_latch_timer_->stop();
    }
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

void SatelliteScreen::setBotPill(const QString& text, const QColor& color) {
    if (!lbl_bot_dot_ || !lbl_bot_text_) {
        return;
    }
    bot_text_ = text;
    bot_color_ = color;
    lbl_bot_dot_->setPixmap(loadTintedSvg(
        QStringLiteral(":/assets/missionplanner/status_dot.svg"), 8, 8,
        color.name()));
    lbl_bot_text_->setText(text);
    lbl_bot_text_->setStyleSheet(statusTextStyle(color.name()) +
                                 QStringLiteral(" background: transparent;"));
}

void SatelliteScreen::setStatePill(const QString& text, const QColor& color) {
    if (!lbl_state_dot_ || !lbl_state_text_) {
        return;
    }
    state_text_ = text;
    state_color_ = color;
    lbl_state_dot_->setPixmap(loadTintedSvg(
        QStringLiteral(":/assets/missionplanner/status_dot.svg"), 8, 8,
        color.name()));
    lbl_state_text_->setText(text);
    lbl_state_text_->setStyleSheet(statusTextStyle(color.name()) +
                                   QStringLiteral(" background: transparent;"));
}

void SatelliteScreen::setMotorsChip(const QString& text, const QColor& color) {
    if (!lbl_motors_dot_ || !lbl_motors_text_) {
        return;
    }
    motors_text_ = text;
    motors_color_ = color;
    lbl_motors_dot_->setPixmap(loadTintedSvg(
        QStringLiteral(":/assets/missionplanner/status_dot.svg"), 6, 6,
        color.name()));
    lbl_motors_text_->setText(text);
    lbl_motors_text_->setStyleSheet(
        QStringLiteral("font-family: 'Arimo'; font-size: 10px; "
                       "font-weight: 700; letter-spacing: 0.5px; color: %1; "
                       "background: transparent;")
            .arg(color.name()));
}

void SatelliteScreen::updateBotPill() {
    // Preferred: the app's layered link model (topic freshness + network
    // reachability) while a mission has the monitor armed. Wording matches
    // the Stage 4/5 BOT pill states.
    if (link_monitor_ && link_monitor_->isArmed()) {
        const qint64 in_state_s = link_monitor_->msInCurrentState() / 1000;
        switch (link_monitor_->state()) {
            case LinkHealthMonitor::State::Healthy:
                setBotPill(QStringLiteral("BOT LIVE"), QColor(kAccent));
                return;
            case LinkHealthMonitor::State::Reconnecting:
                setBotPill(QStringLiteral("BOT LIVE - SYNCING %1s")
                               .arg(in_state_s),
                           QColor(kAmber));
                return;
            case LinkHealthMonitor::State::Disconnected:
                setBotPill(QStringLiteral("BOT OFFLINE %1s").arg(in_state_s),
                           QColor(kEstopRed));
                return;
            case LinkHealthMonitor::State::Idle:
                break;  // fall through to the odometry-age fallback
        }
    }
    // Fallback (no monitor / pre-arm): raw odometry age.
    if (!ros_->isRunning()) {
        setBotPill(QStringLiteral("BOT —"), QColor(mutedColor(dark_mode_)));
        return;
    }
    const OdomSnapshot odom = ros_->odomSnapshot();
    if (!odom.valid) {
        setBotPill(QStringLiteral("BOT —"), QColor(mutedColor(dark_mode_)));
        return;
    }
    const qint64 age_ms = QDateTime::currentMSecsSinceEpoch() - odom.wall_ms;
    if (age_ms < 2500) {
        setBotPill(QStringLiteral("BOT LIVE"), QColor(kAccent));
    } else if (age_ms < 10000) {
        setBotPill(QStringLiteral("BOT SYNCING %1s").arg(age_ms / 1000),
                   QColor(kAmber));
    } else {
        setBotPill(QStringLiteral("BOT OFFLINE %1s").arg(age_ms / 1000),
                   QColor(kEstopRed));
    }
}

void SatelliteScreen::updateStatePill() {
    const CoverageStatus status = ros_->coverageStatus();
    if (!status.valid) {
        return;
    }
    const QString state = status.state.toUpper();
    QColor color(mutedColor(dark_mode_));
    if (state == QLatin1String("RUNNING")) {
        color = QColor(kAccent);
    } else if (state == QLatin1String("BLOCKED")) {
        color = QColor(kEstopRed);
    } else if (state == QLatin1String("PAUSED") ||
               state == QLatin1String("REPLANNING") ||
               state.contains(QLatin1String("COLLECTION"))) {
        color = QColor(kAmber);
    } else if (state == QLatin1String("COMPLETE")) {
        color = QColor(0x2B, 0x7F, 0xFF);
    }
    setStatePill(status.mode.isEmpty()
                     ? state
                     : QStringLiteral("%1 · %2").arg(state, status.mode),
                 color);
    reason_label_->setText(status.reason);
    if (status.coverage >= 0.0) {
        coverage_bar_->setVisible(true);
        coverage_bar_->setValue(int(qBound(0.0, status.coverage, 1.0) * 1000));
    }
}

void SatelliteScreen::appendLog(const QString& line) {
    log_view_->appendPlainText(line);
}

}  // namespace f2c_cpp
