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
#include "components/bdr_message_box.hpp"
#include "components/offline_finalize_dialog.hpp"
#include "components/satellite_plan_confirm_dialog.hpp"
#include "pan_zoom_image.hpp"
#include "satellite_map_capture.hpp"
#include "satellite_map_widget.hpp"
#include "satellite_mission_controller.hpp"
#include "satellite_palette.hpp"
#include "satellite_ros_link.hpp"
#include "satellite_tile_service.hpp"
#include <QListWidget>
#include "settings_constants.hpp"
#include "units_system.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QGraphicsBlurEffect>
#include <QGraphicsDropShadowEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QSlider>
#include <QStackedWidget>
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
constexpr int kLeftRailWidth = 288;
constexpr int kSearchBarWidth = 420;   // 238:4509
constexpr int kSearchRowHeight = 40;
constexpr int kSendButtonHeight = 44;
constexpr int kEstopButtonHeight = 44;
constexpr int kStepHeaderHeight = 55;
constexpr int kStepBadgeSize = 20;
constexpr int kStepChipHeight = 34;
constexpr int kFooterBarHeight = 65;
constexpr int kFooterButtonHeight = 40;
constexpr int kFooterGhostButtonHeight = 36;
// Step 2 picker (Figma 235:2246 / 234:1954 / 219:291 / 235:3146), 1:1 px.
constexpr int kCorrBarHeight = 45;
constexpr int kCorrSatPaneStretch = 58;  // 1113.6 / 1920
constexpr int kCorrPcdPaneStretch = 42;
// Field Save Plan connectivity check. One HEAD; a hotspot answers well
// inside this, and a dead link should not hold the operator for long.
constexpr int kImageryProbeTimeoutMs = 3000;

/**
 * Per-step operator-facing strings. `arrive` is what the footer promises
 * when this step is the *destination*, so the button always names where it
 * is taking you rather than what you just finished. The destination is
 * resolved against availability, which is why these can't be baked into a
 * single per-step "next" label: the office trim skips Alignment, so step 1's
 * footer reads "Define ROI" there and "Capture Point Cloud" in the field.
 */
struct StepSpec {
    const char* title;
    const char* detail;
    const char* arrive;
};

constexpr StepSpec kStepSpecs[] = {
    {"Satellite Map", "Locate building", "Locate Building"},
    {"3D Alignment", "Match point cloud", "Capture Point Cloud"},
    {"ROI Definition", "Draw the scan area", "Define ROI"},
    {"Edge Review", "Mark fall hazards", "Review Edges"},
    {"Autonomous Scan", "Run the mission", "Autonomous Scan"},
};

/**
 * Measured-mode overrides for the first two steps. The step *means* the same
 * thing in both modes, but naming it "Satellite Map" on a canvas that has no
 * imagery, or "3D Alignment" where the grid origin already IS robot_init and
 * nothing gets aligned, describes work the operator will not be doing.
 * Nullptr keeps the satellite wording.
 */
constexpr StepSpec kMeasuredStepSpecs[] = {
    {"Site Setup", "Name the job", "Set Up Site"},
    {"Robot Map", "Collect the point cloud", "Capture Point Cloud"},
    {nullptr, nullptr, nullptr},
    {nullptr, nullptr, nullptr},
    {nullptr, nullptr, nullptr},
};

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
constexpr const char* kWarnAmber = "#F59E0B";
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
    root->addWidget(buildStepHeader());

    auto* content = new QWidget(this);
    auto* content_layout = new QHBoxLayout(content);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(0);
    rail_scroll_ = buildLeftRail();
    content_layout->addWidget(rail_scroll_);
    // One canvas area, two pages: the plan map and the step-2 correspondence
    // picker (which hides the rail — see applyStepVisibility).
    auto* canvas_column = new QWidget(content);
    auto* canvas_column_layout = new QVBoxLayout(canvas_column);
    canvas_column_layout->setContentsMargins(0, 0, 0, 0);
    canvas_column_layout->setSpacing(0);
    canvas_stack_ = new QStackedWidget(canvas_column);
    // Map page: the map fills the cell and the tool stack shares it,
    // aligned to the right edge, so the layout keeps the tools glued to the
    // canvas through every resize without manual geometry.
    map_page_ = new QWidget(canvas_stack_);
    auto* map_page_layout = new QGridLayout(map_page_);
    map_page_layout->setContentsMargins(0, 0, 0, 0);
    map_page_layout->setSpacing(0);
    map_->setParent(map_page_);
    map_page_layout->addWidget(map_, 0, 0);
    canvas_tools_ = buildCanvasTools(map_page_);
    map_page_layout->addWidget(canvas_tools_, 0, 0,
                               Qt::AlignRight | Qt::AlignTop);
    // Step-3 status tag (222:1391), top-left over the map.
    canvas_tag_ = new QLabel(map_page_);
    canvas_tag_->setObjectName("SatPaneTag");
    canvas_tag_->hide();
    map_page_layout->addWidget(canvas_tag_, 0, 0, Qt::AlignLeft | Qt::AlignTop);
    // Step-1 floating search (238:4509) + layer/provenance chip (238:4531).
    search_host_ = buildSearchBar(map_page_);
    search_host_->hide();
    map_page_layout->addWidget(search_host_, 0, 0,
                               Qt::AlignHCenter | Qt::AlignTop);
    layer_chip_ = new QWidget(map_page_);
    layer_chip_->setObjectName("SatLayerChip");
    layer_chip_->setAttribute(Qt::WA_StyledBackground, true);
    {
        auto* chip_layout = new QHBoxLayout(layer_chip_);
        chip_layout->setContentsMargins(12, 6, 12, 6);
        chip_layout->setSpacing(8);
        auto* icon = new QLabel(layer_chip_);
        icon->setFixedSize(14, 14);
        icon->setPixmap(loadTintedSvg(QStringLiteral(":/assets/satellite/layers.svg"),
                                      14, 14, QStringLiteral("#9f9fa9")));
        chip_layout->addWidget(icon);
        layer_chip_text_ = new QLabel(QStringLiteral("Satellite"), layer_chip_);
        layer_chip_text_->setObjectName("SatLayerChipText");
        chip_layout->addWidget(layer_chip_text_);
    }
    layer_chip_->hide();
    // Sits above the scale bar / attribution strip (bottom 30 px of the map).
    // Plain QWidgets ignore QSS margin, so the offset is a wrapper layout.
    auto* chip_wrap = new QWidget(map_page_);
    chip_wrap->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    auto* chip_wrap_layout = new QVBoxLayout(chip_wrap);
    chip_wrap_layout->setContentsMargins(12, 0, 0, 66);  // clears the scale chip
    chip_wrap_layout->addWidget(layer_chip_);
    map_page_layout->addWidget(chip_wrap, 0, 0,
                               Qt::AlignLeft | Qt::AlignBottom);
    canvas_stack_->addWidget(map_page_);
    correspond_page_ = buildCorrespondPage();
    canvas_stack_->addWidget(correspond_page_);
    canvas_column_layout->addWidget(canvas_stack_, 1);
    // The frame floats the footer over the canvas's bottom 65px. It goes in
    // as a real row instead: the bar is all but opaque anyway, and the map's
    // own scale bar and Esri attribution live in exactly that strip, so
    // overlaying would bury the attribution we are contractually required
    // to show.
    canvas_column_layout->addWidget(buildFooterBar());
    content_layout->addWidget(canvas_column, 1);
    root->addWidget(content, 1);

    // ---- Step acknowledgements ----
    connect(roi_confirm_check_, &QCheckBox::toggled, this, [this](bool on) {
        // Snapshot the geometry that was confirmed, so a later vertex drag
        // revokes it but marking a roof edge does not.
        confirmed_vertices_ = on ? map_->polygon().vertices
                                 : QVector<geo::GeoPoint>{};
        refreshStepUi();
    });
    connect(edge_review_check_, &QCheckBox::toggled, this, [this](bool on) {
        edges_reviewed_ = on;
        refreshStepUi();
    });

    map_capture_ = new MapCaptureRunner(this);
    connect(map_capture_, &MapCaptureRunner::progress, this,
            [this](const QString& message) {
                setAlignStatus(message);
                appendLog(QStringLiteral("[align] %1").arg(message));
            });
    connect(map_capture_, &MapCaptureRunner::finished, this,
            &SatelliteScreen::onMapCaptured);

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
        refreshStepUi();
    });
    // Closing the polygon (click-near-first / right-click) changes no vertex
    // but does flip the step-3 gate, so the footer must re-evaluate.
    connect(map_, &SatelliteMapWidget::interactionChanged, this,
            &SatelliteScreen::refreshStepUi);
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
        if (marker.valid) {
            // Placing the robot is a deliberate act of aiming the canvas.
            canvas_aimed_ = true;
            refreshStepUi();
        }
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
    connect(ros_, &RosLink::motorStatusUpdated, this, [this] {
        if (link_monitor_) {
            link_monitor_->stamp(LinkHealthMonitor::Source::ControllerStatus);
        }
        updateMotorsChip();
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
                tool_rect_button_->setEnabled(!active);
                tool_polygon_button_->setEnabled(!active);
                draw_button_->setEnabled(!active);
                place_robot_button_->setEnabled(!active);
                clear_roi_button_->setEnabled(!active);
                save_button_->setEnabled(!active);
                updateAlignCardUi();
                if (active) {
                    canvas_stack_->setCurrentWidget(map_page_);
                }
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
    selected_step_ = computeStep();
    applyModeVisibility();
}

void SatelliteScreen::configureForScan(PlanMode mode) {
    planning_only_ = false;
    plan_mode_ = mode;
    refreshJobsCombo(QString());
    newJob();
    selected_step_ = computeStep();
    applyModeVisibility();
    if (mode == PlanMode::Measured) {
        // The measured canvas opens centered on its reference origin at a
        // scale where a typical roof fills the view (~60 m across).
        map_->setView(0.0, 0.0, kMeasuredDefaultZoom);
    }
}

void SatelliteScreen::setJobName(const QString& name) {
    job_name_->setText(name);  // textChanged -> refreshStepUi + refreshTitle
}

void SatelliteScreen::devSelectMarker() { map_->setMarkerSelected(true); }

void SatelliteScreen::devFitRoi() { map_->fitToRoi(); }

void SatelliteScreen::devRenderPlanConfirm(const QString& png_path) {
    geo::GeoPoint centroid;
    double roi_radius_m = 0.0;
    if (!map_->roiExtent(&centroid, &roi_radius_m)) {
        return;
    }
    map_->fitToRoi();
    const Job job = jobFromRail();
    const PrefetchRequest request = PrefetchRequest::forSite(
        centroid, roi_radius_m, job_store_.assetsDir(job.id));
    SatellitePlanConfirmDialog dialog(tiles_, job, map_->grab(), request,
                                      false, this);
    // Polish happens on show; a never-shown frameless dialog grabs without
    // its stylesheet background.
    dialog.show();
    QCoreApplication::processEvents();
    dialog.grab().save(png_path);
    dialog.devSetAdvancedOpen(true);
    QCoreApplication::processEvents();
    QString advanced_path = png_path;
    advanced_path.replace(QStringLiteral(".png"), QStringLiteral("_adv.png"));
    dialog.grab().save(advanced_path);
    dialog.close();
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

    // Satisfy step 1 as well. Without a name the whole flow is gated at the
    // first chip, and every shot would render the same step regardless of
    // what the mode seeded.
    job_name_->setText(QStringLiteral("Demo Roof"));
    canvas_aimed_ = true;
    applyModeVisibility();
}

void SatelliteScreen::devSeedDemoAlignment(bool review) {
    // Synthetic stand-ins so the picker's layout, markers and turn-taking can
    // be shot without a robot or a cached site.
    QImage sat(900, 700, QImage::Format_RGB32);
    sat.fill(QColor(0x2a, 0x33, 0x28));
    QPainter sp(&sat);
    sp.setPen(QPen(QColor(0x6b, 0x72, 0x64), 3));
    sp.drawRect(240, 180, 380, 260);
    sp.drawLine(0, 520, 900, 500);
    sp.end();
    sat_image_ = sat;
    site_manifest_.stitch_bounds = QRectF(0.25, 0.35, 0.0004, 0.0003);

    QRectF bounds(-14.0, -10.0, 28.0, 20.0);
    QImage pcd(560, 400, QImage::Format_ARGB32);
    pcd.fill(Qt::transparent);
    QPainter pp(&pcd);
    pp.setPen(QPen(QColor(210, 210, 210, 200), 2));
    pp.drawRect(80, 60, 400, 280);
    pp.end();
    pcd_image_ = pcd;
    pcd_bounds_m_ = bounds;

    capture_gps_.valid = true;
    capture_gps_.lat = 43.600664;
    capture_gps_.lon = -116.249774;
    capture_gps_.fix_type = QStringLiteral("rtk_fixed");
    capture_gps_.hacc_m = 0.021;
    if (plan_mode_ == PlanMode::Measured) {
        // Mirror onMapCaptured: measured mode anchors at the grid origin and
        // stays on the canvas — there is nothing to align against.
        map_->setMapRaster(pcd_image_, pcd_bounds_m_);
        geo::GeoPose marker;
        marker.heading_deg = 90.0;
        marker.valid = true;
        map_->setMarker(marker);
        emit map_->markerChanged();
        map_->setView(0.0, 0.0, map_->zoom());
        updateAlignCardUi();
        return;
    }

    correspondences_.clear();
    const QVector<QPair<QPointF, QPointF>> pairs{
        {{240, 180}, {-10.0, 7.0}},
        {{620, 180}, {10.0, 7.0}},
        {{620, 440}, {10.0, -7.0}},
        {{240, 440}, {-10.0, -7.0}},
    };
    for (const auto& pair : pairs) {
        correspondences_.append(Correspondence{pair.first, pair.second});
    }
    devEnterAlignmentWithDemoSite();
    if (review) {
        onAlignClicked();
    }
    updateAlignCardUi();
}

void SatelliteScreen::devSeedDemoRoiStep() {
    // Step 3 as the frame shows it: aligned, a closed polygon on the canvas,
    // the Edge Dimensions rail live.
    devSeedDemoAlignment(true);
    setSelectedStep(Step::RoiDefinition);
    // Deferred: the canvas has no size until the shot's first layout pass.
    QTimer::singleShot(400, this, [this] { map_->fitToRoi(); });
}

void SatelliteScreen::devSeedDemoAlignmentEmpty() {
    // Step 2 as the operator first meets it: cached site on the left, the
    // capture CTA on the right, nothing picked yet.
    QImage sat(900, 700, QImage::Format_RGB32);
    sat.fill(QColor(0x2a, 0x33, 0x28));
    QPainter sp(&sat);
    sp.setPen(QPen(QColor(0x6b, 0x72, 0x64), 3));
    sp.drawRect(240, 180, 380, 260);
    sp.drawLine(0, 520, 900, 500);
    sp.end();
    sat_image_ = sat;
    site_manifest_.stitch_bounds = QRectF(0.25, 0.35, 0.0004, 0.0003);
    pcd_image_ = QImage();
    correspondences_.clear();
    have_pending_sat_ = false;
    devEnterAlignmentWithDemoSite();
    updateAlignCardUi();
}

void SatelliteScreen::devEnterAlignmentWithDemoSite() {
    // setSelectedStep(Alignment) routes through showCorrespondPage(), whose
    // loadSiteImage() fails without a saved job and clears sat_image_. Keep
    // the synthetic site across that and repaint the panes from it.
    const QImage keep = sat_image_;
    setSelectedStep(Step::Alignment);
    sat_image_ = keep;
    sat_pick_->setEmptyText(QString());
    updateCorrespondenceUi();
    canvas_stack_->setCurrentWidget(correspond_page_);
}

void SatelliteScreen::configureForPlanning() {
    planning_only_ = true;
    plan_mode_ = PlanMode::Satellite;
    refreshJobsCombo(current_job_id_);
    selected_step_ = computeStep();
    applyModeVisibility();
}

void SatelliteScreen::refreshTitle() {
    if (!lbl_title_) {
        return;
    }
    const bool measured = plan_mode_ == PlanMode::Measured;
    const QString base =
        planning_only_ ? QStringLiteral("Plan Job")
                       : (measured ? QStringLiteral("Measured ROI Setup")
                                   : QStringLiteral("Satellite ROI Setup"));
    const QString name = job_name_ ? job_name_->text().trimmed() : QString();
    // Frame: "Satellite ROI Setup  — <plan>" with the plan name muted.
    lbl_title_->setTextFormat(Qt::RichText);
    lbl_title_->setText(
        name.isEmpty()
            ? base
            : QStringLiteral("%1 <span style='font-weight:400;color:%2'>"
                             "&nbsp;— %3</span>")
                  .arg(base, mutedColor(dark_mode_), name.toHtmlEscaped()));
}

void SatelliteScreen::applyModeVisibility() {
    const bool measured = plan_mode_ == PlanMode::Measured;
    map_->setImageryEnabled(!measured);
    refreshTitle();
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
    // The office is one task — draw, place, save — so it gets neither the
    // five-step header nor the footer. Find Robot has no robot to find, and
    // the numeric ROI mirror is redundant with the canvas chips and handles.
    if (step_header_) {
        step_header_->setVisible(!planning_only_);
    }
    if (footer_bar_) {
        footer_bar_->setVisible(!planning_only_);
    }
    if (find_robot_button_) {
        find_robot_button_->setVisible(!planning_only_);
    }
    if (roi_numeric_host_) {
        roi_numeric_host_->setVisible(!planning_only_);
    }
    if (mission_card_) {
        mission_card_->setVisible(!planning_only_);
    }
    if (teleop_card_) {
        teleop_card_->setVisible(!planning_only_);
    }
    // Aligning needs both a robot and real imagery, so the card is hidden on
    // the measured canvas and in the office planning trim.
    updateAlignCardUi();
    // refreshStepUi() first: it clamps selected_step_ when a trim change has
    // made it unavailable, and applyStepVisibility() has to act on the
    // clamped value. Visibility then runs last, narrowing whatever the mode
    // rules left visible down to the active step.
    refreshStepUi();
    applyStepVisibility();
    if (roi_confirm_check_) {
        roi_confirm_check_->blockSignals(true);
        roi_confirm_check_->setChecked(roiMatchesConfirmed());
        roi_confirm_check_->blockSignals(false);
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
    back->setFixedSize(40, 32);
    auto* back_layout = new QHBoxLayout(back);
    back_layout->setContentsMargins(12, 6, 12, 6);
    back_layout->setSpacing(8);
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
    // Frames label the back button ("Dashboard" / "Back", 238:4304,
    // 222:1170); the office trim keeps the icon-only Stage 4/5 button.
    back_label_ = new QLabel(QStringLiteral("Back"), back);
    back_label_->setObjectName("SatBackLabel");
    back_label_->hide();
    back_layout->addWidget(back_label_, 0, Qt::AlignVCenter);
    connect(back, &QPushButton::clicked, this, [this] {
        if (mission_->missionActive()) {
            appendLog(QStringLiteral(
                "[nav] mission active — Complete Mission before leaving"));
            return;
        }
        // Alignment state is per visit: a collected map or a fit must not
        // survive into the next plan. The operator confirms the discard
        // (a capture cost a robot spin) and the screen clears before the
        // dashboard shows. A confirmed anchor is already on the job.
        const bool have_align_session = !pcd_image_.isNull() ||
                                        !correspondences_.isEmpty() ||
                                        have_pending_sat_ || pcd_to_sat_.valid;
        if (have_align_session &&
            !confirmDialog(
                QStringLiteral("Leave to Dashboard?"),
                QStringLiteral(
                    "The collected robot map and any correspondence picks "
                    "will be cleared. A confirmed alignment stays saved "
                    "with the plan; anything else is re-captured next "
                    "time."),
                QStringLiteral("Clear & Leave"))) {
            return;
        }
        if (map_capture_ && map_capture_->busy()) {
            map_capture_->cancel();
        }
        resetAlignmentSession();
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
    // Units are chosen once per mission in the New Scan Information modal
    // (persisted to QSettings); there is deliberately no toggle here.

    layout->addWidget(status_host, 0, Qt::AlignVCenter);
    layout->addSpacing(kTopStatusWindowControlsReservedWidth);
    return top_bar;
}

QWidget* SatelliteScreen::buildAckCard(QWidget* parent,
                                       const QString& object_name,
                                       const QString& icon,
                                       const QString& title,
                                       const QString& description,
                                       const QString& check,
                                       QCheckBox** out_check) {
    auto* card = new QWidget(parent);
    card->setObjectName("SatCard");
    card->setProperty("satCardRole", object_name);
    card->setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 16);
    layout->setSpacing(8);
    layout->addWidget(makeCardHeader(icon, title, card));

    auto* body = new QLabel(description, card);
    body->setObjectName("SatFieldLabel");
    body->setWordWrap(true);
    layout->addWidget(body);

    auto* box = new QCheckBox(check, card);
    box->setObjectName("SatCheck");
    layout->addWidget(box);
    if (out_check) {
        *out_check = box;
    }
    return card;
}

QWidget* SatelliteScreen::buildStepHeader() {
    step_header_ = new QWidget(this);
    step_header_->setObjectName("SatStepHeader");
    step_header_->setAttribute(Qt::WA_StyledBackground, true);
    step_header_->setFixedHeight(kStepHeaderHeight);
    auto* layout = new QHBoxLayout(step_header_);
    layout->setContentsMargins(24, 10, 24, 10);
    layout->setSpacing(8);
    layout->addStretch(1);

    step_chips_.clear();
    for (int i = 0; i < kStepCount; ++i) {
        StepChip chip;
        chip.button = new QPushButton(step_header_);
        chip.button->setObjectName("SatStepChip");
        chip.button->setFlat(true);
        chip.button->setFixedHeight(kStepChipHeight);
        chip.button->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
        auto* chip_layout = new QHBoxLayout(chip.button);
        chip_layout->setContentsMargins(12, 6, 12, 6);
        chip_layout->setSpacing(8);

        chip.badge = new QLabel(QString::number(i + 1), chip.button);
        chip.badge->setObjectName("SatStepBadge");
        chip.badge->setFixedSize(kStepBadgeSize, kStepBadgeSize);
        chip.badge->setAlignment(Qt::AlignCenter);
        chip_layout->addWidget(chip.badge, 0, Qt::AlignVCenter);

        chip.label = new QLabel(QString::fromLatin1(kStepSpecs[i].title),
                                chip.button);
        chip.label->setObjectName("SatStepLabel");
        chip_layout->addWidget(chip.label, 0, Qt::AlignVCenter);

        // Only the active chip shows its detail line, so the header stays
        // readable at five steps wide.
        chip.detail = new QLabel(
            QStringLiteral("— %1").arg(
                QString::fromLatin1(kStepSpecs[i].detail)),
            chip.button);
        chip.detail->setObjectName("SatStepDetail");
        chip.detail->hide();
        chip_layout->addWidget(chip.detail, 0, Qt::AlignVCenter);

        const Step step = Step(i);
        connect(chip.button, &QPushButton::clicked, this,
                [this, step] { setSelectedStep(step); });
        layout->addWidget(chip.button, 0, Qt::AlignVCenter);

        if (i + 1 < kStepCount) {
            chip.chevron = new QLabel(QStringLiteral("›"), step_header_);
            chip.chevron->setObjectName("SatStepChevron");
            chip.chevron->setFixedSize(16, 16);
            chip.chevron->setAlignment(Qt::AlignCenter);
            layout->addWidget(chip.chevron, 0, Qt::AlignVCenter);
        }
        step_chips_.append(chip);
    }

    layout->addStretch(1);
    return step_header_;
}

QWidget* SatelliteScreen::buildFooterBar() {
    footer_bar_ = new QWidget(this);
    footer_bar_->setObjectName("SatFooterBar");
    footer_bar_->setAttribute(Qt::WA_StyledBackground, true);
    footer_bar_->setFixedHeight(kFooterBarHeight);
    auto* layout = new QHBoxLayout(footer_bar_);
    layout->setContentsMargins(24, 12, 24, 12);
    layout->setSpacing(12);

    // Frame (Figma 235:3113): left group = Back (icon 16 + label, 36 tall,
    // 16px padding) and Clear pairs (icon 14, 12px padding); right group =
    // Align (N pairs) (zinc, 40 tall, 20px padding) and Next (green, 24px
    // padding, arrow after the label). Disabled = 40% opacity, not a grey
    // restyle.
    back_button_ = new QPushButton(QStringLiteral("Back"), footer_bar_);
    back_button_->setObjectName("SatGhostButton");
    back_button_->setIcon(QIcon(loadTintedSvg(
        QStringLiteral(":/assets/satellite/footer_back.svg"), 16, 16)));
    back_button_->setIconSize(QSize(16, 16));
    back_button_->setCursor(Qt::PointingHandCursor);
    back_button_->setFixedHeight(kFooterGhostButtonHeight);
    connect(back_button_, &QPushButton::clicked, this, [this] {
        for (int i = int(selected_step_) - 1; i >= 0; --i) {
            if (stepAvailable(Step(i))) {
                setSelectedStep(Step(i));
                return;
            }
        }
    });
    layout->addWidget(back_button_, 0, Qt::AlignVCenter);

    clear_pairs_button_ =
        new QPushButton(QStringLiteral("Clear pairs"), footer_bar_);
    clear_pairs_button_->setObjectName("SatGhostButtonMuted");
    clear_pairs_button_->setIcon(QIcon(loadTintedSvg(
        QStringLiteral(":/assets/satellite/clear_pairs.svg"), 14, 14)));
    clear_pairs_button_->setIconSize(QSize(14, 14));
    clear_pairs_button_->setCursor(Qt::PointingHandCursor);
    clear_pairs_button_->setFixedHeight(kFooterGhostButtonHeight);
    clear_pairs_button_->hide();
    connect(clear_pairs_button_, &QPushButton::clicked, this,
            &SatelliteScreen::onClearCorrespondences);
    layout->addWidget(clear_pairs_button_, 0, Qt::AlignVCenter);
    layout->addStretch(1);

    align_button_ = new QPushButton(QStringLiteral("Align (0 pairs)"), footer_bar_);
    align_button_->setObjectName("SatAlignButton");
    align_button_->setIcon(QIcon(loadTintedSvg(
        QStringLiteral(":/assets/satellite/align.svg"), 16, 16)));
    align_button_->setIconSize(QSize(16, 16));
    align_button_->setCursor(Qt::PointingHandCursor);
    align_button_->setFixedHeight(kFooterButtonHeight);
    align_button_->setEnabled(false);
    align_button_->hide();
    connect(align_button_, &QPushButton::clicked, this,
            &SatelliteScreen::onAlignClicked);
    layout->addWidget(align_button_, 0, Qt::AlignVCenter);

    next_button_ = new QPushButton(footer_bar_);
    next_button_->setObjectName("SatNextButton");
    next_button_->setCursor(Qt::PointingHandCursor);
    next_button_->setFixedHeight(kFooterButtonHeight);
    // Trailing arrow: QPushButton draws its icon before the text, so flip
    // the button's layout direction — the icon lands after the label and
    // the text itself is unaffected.
    next_button_->setLayoutDirection(Qt::RightToLeft);
    next_button_->setIcon(QIcon(loadTintedSvg(
        QStringLiteral(":/assets/satellite/footer_next.svg"), 16, 16)));
    next_button_->setIconSize(QSize(16, 16));
    connect(next_button_, &QPushButton::clicked, this, [this] {
        setSelectedStep(nextAvailableStep(selected_step_));
    });
    layout->addWidget(next_button_, 0, Qt::AlignVCenter);
    return footer_bar_;
}

// ---- Step model -------------------------------------------------------------

bool SatelliteScreen::stepAvailable(Step step) const {
    switch (step) {
        case Step::Alignment:
        case Step::AutonomousScan:
            // Both need a robot on site, so neither is reachable from the
            // office planning trim.
            return !planning_only_;
        case Step::SatelliteMap:
        case Step::RoiDefinition:
        case Step::EdgeReview:
            break;
    }
    return true;
}

bool SatelliteScreen::stepComplete(Step step) const {
    switch (step) {
        case Step::SatelliteMap:
            // Named job plus a deliberately aimed canvas. Cached imagery is
            // deliberately NOT required: it would force a tile download on
            // someone merely drafting, and hard-block a satellite plan
            // started in the field, where there is no internet to cache
            // from. Measured mode has no imagery to aim at at all.
            // The field rail hides the name field; loadJob() still fills it
            // from the chosen plan, so the check holds there too.
            return job_name_ && !job_name_->text().trimmed().isEmpty() &&
                   (plan_mode_ == PlanMode::Measured || canvas_aimed_);
        case Step::Alignment:
            // Measured mode has no correspondence step — its grid origin IS
            // robot_init, so a collected map is already in the canvas frame.
            return plan_mode_ == PlanMode::Measured
                       ? !pcd_image_.isNull()
                       : pcd_to_sat_.valid && alignment_confirmed_;
        case Step::RoiDefinition:
            if (!map_ || !map_->polygon().valid()) {
                return false;
            }
            // Frame 222:1155 has no confirm checkbox: a closed polygon is
            // the deliverable. Office / measured keep the acknowledgement.
            if (!planning_only_ && plan_mode_ == PlanMode::Satellite) {
                return !map_->isDrawing();
            }
            return roiMatchesConfirmed();
        case Step::EdgeReview:
            return edges_reviewed_;
        case Step::AutonomousScan:
            // Terminal: "complete" here means the mission finalized, which
            // tears the screen down anyway.
            return false;
    }
    return false;
}

bool SatelliteScreen::roiMatchesConfirmed() const {
    if (!map_ || confirmed_vertices_.isEmpty()) {
        return false;
    }
    const QVector<geo::GeoPoint>& current = map_->polygon().vertices;
    if (current.size() != confirmed_vertices_.size()) {
        return false;
    }
    // Exact comparison is right here: these are the same doubles copied
    // verbatim unless the operator actually moved something.
    for (int i = 0; i < current.size(); ++i) {
        if (current[i].lat != confirmed_vertices_[i].lat ||
            current[i].lon != confirmed_vertices_[i].lon) {
            return false;
        }
    }
    return true;
}

bool SatelliteScreen::stepReachable(Step step) const {
    if (!stepAvailable(step)) {
        return false;
    }
    for (int i = 0; i < int(step); ++i) {
        const Step earlier = Step(i);
        if (stepAvailable(earlier) && !stepComplete(earlier)) {
            return false;
        }
    }
    return true;
}

SatelliteScreen::Step SatelliteScreen::computeStep() const {
    Step last_available = Step::SatelliteMap;
    for (int i = 0; i < kStepCount; ++i) {
        const Step step = Step(i);
        if (!stepAvailable(step)) {
            continue;
        }
        last_available = step;
        if (!stepComplete(step)) {
            return step;
        }
    }
    return last_available;
}

SatelliteScreen::Step SatelliteScreen::nextAvailableStep(Step step) const {
    // Skip what is already done — measured mode can arrive at step 1 with
    // step 2 already satisfied by a collected map, and a footer offering to
    // "Capture Point Cloud" would be promising work that is finished.
    // Revisiting a completed step is still possible, just via its chip.
    for (int i = int(step) + 1; i < kStepCount; ++i) {
        if (stepAvailable(Step(i)) && !stepComplete(Step(i))) {
            return Step(i);
        }
    }
    for (int i = int(step) + 1; i < kStepCount; ++i) {
        if (stepAvailable(Step(i))) {
            return Step(i);
        }
    }
    return step;
}

void SatelliteScreen::setSelectedStep(Step step) {
    if (!stepReachable(step)) {
        return;
    }
    selected_step_ = step;
    if (canvas_stack_) {
        if (step != Step::Alignment) {
            // Leaving the picker or the review behind must put the canvas
            // back on the map, or the operator lands on step 3 still looking
            // at a side-by-side picker.
            canvas_stack_->setCurrentWidget(map_page_);
        } else if (plan_mode_ == PlanMode::Satellite) {
            // Step 2 IS the two-pane picker (frame): it opens with the point
            // cloud pane in its capture empty state, so the operator never
            // needs a rail card to get the capture started.
            showCorrespondPage();
        }
    }
    applyModeVisibility();
}

void SatelliteScreen::refreshStepUi() {
    if (step_chips_.size() != kStepCount) {
        return;
    }
    if (!stepAvailable(selected_step_)) {
        // Trim changed under us (planning <-> scan).
        selected_step_ = computeStep();
    }

    const bool measured = plan_mode_ == PlanMode::Measured;
    const auto spec = [measured](int index) {
        return measured && kMeasuredStepSpecs[index].title
                   ? kMeasuredStepSpecs[index]
                   : kStepSpecs[index];
    };

    const bool dark = dark_mode_;
    const QString muted = mutedColor(dark);
    const QString idle_badge_bg = dark ? QStringLiteral("#27272a")
                                       : QStringLiteral("#E5E7EB");
    const QString idle_fg = dark ? QStringLiteral("#52525C")
                                 : QStringLiteral("#9CA3AF");
    const QString active_fg = dark ? QStringLiteral("#00D492")
                                   : QStringLiteral("#00A86D");

    for (int i = 0; i < kStepCount; ++i) {
        const Step step = Step(i);
        const StepChip& chip = step_chips_[i];
        const bool active = step == selected_step_;
        const bool available = stepAvailable(step);
        const bool complete = stepComplete(step);
        const bool clickable = stepReachable(step);

        chip.label->setText(QString::fromLatin1(spec(i).title));
        chip.detail->setText(
            QStringLiteral("— %1").arg(QString::fromLatin1(spec(i).detail)));
        chip.button->setEnabled(clickable);
        chip.button->setCursor(clickable && !active ? Qt::PointingHandCursor
                                                    : Qt::ArrowCursor);
        chip.button->setStyleSheet(
            active ? QStringLiteral(
                         "QPushButton#SatStepChip {"
                         " background-color: rgba(0, 188, 125, 0.15);"
                         " border: 1px solid rgba(0, 188, 125, 0.30);"
                         " border-radius: 10px; }")
                   : QStringLiteral(
                         "QPushButton#SatStepChip { background-color:"
                         " transparent; border: none; border-radius: 10px; }"));

        // A finished step reads as done; an unavailable one has to look
        // different from a merely incomplete one, or the office operator
        // spends the day wondering why Alignment never lights up.
        QString fg = idle_fg;
        QString badge_bg = idle_badge_bg;
        QString badge_border = QStringLiteral("none");
        if (active) {
            fg = active_fg;
            badge_bg = QStringLiteral("rgba(0, 188, 125, 0.20)");
            badge_border = QStringLiteral("1px solid rgba(0, 188, 125, 0.50)");
        } else if (complete) {
            fg = muted;
        }
        chip.badge->setText(!available ? QStringLiteral("–")
                                       : (complete && !active
                                              ? QStringLiteral("✓")
                                              : QString::number(i + 1)));
        chip.badge->setStyleSheet(
            QStringLiteral("QLabel#SatStepBadge { background-color: %1;"
                           " border: %2; border-radius: %3px;"
                           " font-family: 'Arimo'; font-weight: 700;"
                           " font-size: 12px; color: %4; }")
                .arg(badge_bg, badge_border)
                .arg(kStepBadgeSize / 2)
                .arg(fg));
        chip.label->setStyleSheet(
            QStringLiteral("QLabel#SatStepLabel { background: transparent;"
                           " font-family: 'Arimo'; font-weight: 500;"
                           " font-size: 14px; color: %1; }")
                .arg(fg));
        chip.label->setToolTip(
            available ? QString()
                      : QStringLiteral("Requires the robot on site"));
        chip.detail->setVisible(active);
        chip.detail->setStyleSheet(
            QStringLiteral("QLabel#SatStepDetail { background: transparent;"
                           " font-family: 'Arimo'; font-weight: 400;"
                           " font-size: 14px; color: %1; }")
                .arg(muted));

        // QPushButton derives its sizeHint from its text, not from a layout
        // placed inside it, so a chip assembled out of child labels collapses
        // to the width of its badge. Drive the width from the layout, and
        // redo it on every refresh because showing or hiding the detail line
        // changes what it needs.
        QLayout* chip_layout = chip.button->layout();
        chip_layout->invalidate();
        chip.button->setFixedWidth(chip_layout->sizeHint().width());
        if (chip.chevron) {
            // A glyph rather than an SVG: the only chevron assets in the
            // tree are the back button's left-pointing pair, and mirroring
            // them costs more than it saves for a 16px separator.
            chip.chevron->setStyleSheet(
                QStringLiteral("QLabel#SatStepChevron { background:"
                               " transparent; font-family: 'Arimo';"
                               " font-size: 14px; color: %1; }")
                    .arg(dark ? QStringLiteral("#3f3f46")
                              : QStringLiteral("#D1D5DC")));
        }
    }

    if (next_button_) {
        const Step next = nextAvailableStep(selected_step_);
        const bool has_next = next != selected_step_ && !planning_only_;
        bool has_prev = false;
        for (int i = int(selected_step_) - 1; i >= 0 && !has_prev; --i) {
            has_prev = stepAvailable(Step(i));
        }
        has_prev = has_prev && !planning_only_;
        footer_bar_->setVisible(has_next || has_prev);
        back_button_->setVisible(has_prev);
        next_button_->setVisible(has_next);
        // Clear pairs / Align belong to the satellite picker, and only once
        // there is a cloud to pick against (frames 219:291 vs 234:1954).
        const bool picker_actions = !planning_only_ &&
                                    selected_step_ == Step::Alignment &&
                                    plan_mode_ == PlanMode::Satellite &&
                                    !pcd_image_.isNull();
        clear_pairs_button_->setVisible(picker_actions);
        // Aligned (235:3146): the footer drops Align; Clear pairs stays as
        // the way back to re-pick.
        align_button_->setVisible(picker_actions && !pcd_to_sat_.valid);
        if (has_next) {
            next_button_->setText(
                QStringLiteral("Next: %1")
                    .arg(QString::fromLatin1(spec(int(next)).arrive)));
            next_button_->setEnabled(stepComplete(selected_step_));
            next_button_->setToolTip(
                stepComplete(selected_step_)
                    ? QString()
                    : QStringLiteral("Finish this step first"));
        }
    }
}

void SatelliteScreen::applyStepVisibility() {
    // Scaffolding stage: the rail still holds today's cards, shown and
    // hidden per step. Each card gets reshaped into the frame's flat
    // sections in its own change, so the step machinery below is not
    // churning at the same time as the widgets it governs.
    const Step step = selected_step_;
    // Field satellite trim follows the Figma frames step by step: step 1 is
    // a full-width canvas with the floating address search (238:4289; the
    // name came from the chosen plan + metadata modal), step 3 the ROI
    // Definition rail (222:1284), step 4 the edge review card. The office
    // and the measured canvas keep the single authoring card.
    const bool frame_rail =
        !planning_only_ && plan_mode_ == PlanMode::Satellite;
    const bool locate_step = frame_rail && step == Step::SatelliteMap;
    if (plan_card_) {
        plan_card_->setVisible(!frame_rail && (step == Step::SatelliteMap ||
                                               step == Step::RoiDefinition ||
                                               step == Step::EdgeReview));
    }
    if (search_host_) {
        search_host_->setVisible(locate_step);
        if (locate_step) {
            search_edit_->setFocus(Qt::OtherFocusReason);
        } else {
            hideSuggestions();
        }
    }
    if (layer_chip_) {
        layer_chip_->setVisible(locate_step);
        // Layer name per frame; "Hybrid" would mean labels over imagery,
        // which World Imagery / Clarity / Wayback do not carry.
        QString layer = tiles_->layer() == TileService::ImageryLayer::Clarity
                            ? QStringLiteral("Clarity")
                            : QStringLiteral("World Imagery");
        if (!tiles_->waybackRelease().isEmpty()) {
            layer = QStringLiteral("Wayback");
        }
        layer_chip_text_->setText(QStringLiteral("Satellite • %1").arg(layer));
    }
    // Frames show a clean canvas on step 1 (238:4289): the saved ROI and
    // robot marker only appear once the operator reaches the ROI work.
    map_->setOverlaysHidden(locate_step);
    // Frame tool stack is zoom-in + fit; zoom-out / ruler are office extras.
    if (zoom_out_button_) {
        zoom_out_button_->setVisible(!frame_rail);
    }
    if (measure_button_) {
        measure_button_->setVisible(!frame_rail);
    }
    if (back_label_) {
        // 238:4304 labels the top-bar back "Dashboard" on step 1; the later
        // frames (222:1170) say "Back".
        back_label_->setText(locate_step ? QStringLiteral("Dashboard")
                                         : QStringLiteral("Back"));
        back_label_->setVisible(frame_rail);
        // QPushButton::sizeHint ignores child layouts, so size it by hand:
        // 118 px with "Dashboard", 79 px with "Back" (frame), 40 icon-only.
        // Text width measured — Arimo runs wider than the frame's Inter.
        back_label_->adjustSize();
        back_label_->parentWidget()->setFixedWidth(
            frame_rail ? 12 + 16 + 8 + back_label_->sizeHint().width() + 12
                       : 40);
    }
    if (roi_card_) {
        const bool roi_step = frame_rail && step == Step::RoiDefinition;
        roi_card_->setVisible(roi_step);
        if (roi_step && !map_->polygon().valid() && !map_->isDrawing()) {
            // No Draw button on this rail: the canvas is armed on entry.
            map_->armPolygonDraw();
        }
        refreshRoiCard();
    }
    // Satellite alignment is the full-width picker: no rail at all. The
    // measured variant keeps the rail card (capture + status), since its
    // canvas stays the grid and the point cloud lands straight on it.
    const bool picker_step = !planning_only_ && step == Step::Alignment &&
                             plan_mode_ == PlanMode::Satellite;
    if (rail_scroll_) {
        rail_scroll_->setVisible(!picker_step && !locate_step);
    }
    if (align_card_) {
        align_card_->setVisible(!planning_only_ && step == Step::Alignment &&
                                plan_mode_ == PlanMode::Measured);
    }
    if (roi_confirm_card_) {
        roi_confirm_card_->setVisible(!frame_rail &&
                                      step == Step::RoiDefinition);
    }
    if (edge_review_card_) {
        edge_review_card_->setVisible(step == Step::EdgeReview);
    }
    if (mission_card_) {
        mission_card_->setVisible(!planning_only_ &&
                                  step == Step::AutonomousScan);
    }
    if (teleop_card_) {
        teleop_card_->setVisible(!planning_only_ &&
                                 step == Step::AutonomousScan);
    }
    if (log_card_) {
        // The frame rails carry no log; it stays on the scan step where the
        // mission events actually land.
        log_card_->setVisible(!frame_rail || step == Step::AutonomousScan);
    }
}

QWidget* SatelliteScreen::buildLeftRail() {
    auto* rail_content = new QWidget;
    rail_content->setObjectName("SatRail");
    rail_content->setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QVBoxLayout(rail_content);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);
    plan_card_ = buildPlanCard(rail_content);
    layout->addWidget(plan_card_);
    roi_card_ = buildRoiCard(rail_content);
    layout->addWidget(roi_card_);
    align_card_ = buildAlignCard(rail_content);
    layout->addWidget(align_card_);
    roi_confirm_card_ = buildAckCard(
        rail_content, QStringLiteral("roiConfirm"),
        QStringLiteral(":/assets/exploration/map.svg"),
        QStringLiteral("Confirm ROI"),
        QStringLiteral("Check the outline against the aligned robot map and "
                       "drag any vertex that does not match the real roof. "
                       "Adjusting the shape clears this confirmation."),
        QStringLiteral("ROI matches the roof"), &roi_confirm_check_);
    layout->addWidget(roi_confirm_card_);
    edge_review_card_ = buildAckCard(
        rail_content, QStringLiteral("edgeReview"),
        QStringLiteral(":/assets/missionplanner/scan_card_telemetry.svg"),
        QStringLiteral("Edge Review"),
        QStringLiteral("Tap every ROI edge with a fall hazard beyond it — "
                       "red edges get a larger planning setback. A roof with "
                       "no hazardous edges is a valid answer; what matters is "
                       "that you have looked."),
        QStringLiteral("Edges reviewed"), &edge_review_check_);
    layout->addWidget(edge_review_card_);
    mission_card_ = buildMissionCard(rail_content);
    layout->addWidget(mission_card_);
    teleop_card_ = buildTeleopCard(rail_content);
    layout->addWidget(teleop_card_);
    log_card_ = buildLogCard(rail_content);
    layout->addWidget(log_card_);
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

QWidget* SatelliteScreen::buildAlignCard(QWidget* parent) {
    auto* card = new QWidget(parent);
    card->setObjectName("SatCard");
    card->setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 16);
    layout->setSpacing(8);
    layout->addWidget(
        makeCardHeader(QStringLiteral(":/assets/exploration/telemetry.svg"),
                       QStringLiteral("Align to Robot Map"), card));

    collect_map_button_ =
        new QPushButton(QStringLiteral("Collect Map from Robot"), card);
    correspond_button_ =
        new QPushButton(QStringLiteral("Pick Correspondences"), card);
    for (QPushButton* button : {collect_map_button_, correspond_button_}) {
        button->setObjectName("SatButton");
        button->setFixedHeight(36);
        button->setCursor(Qt::PointingHandCursor);
        layout->addWidget(button);
    }
    connect(collect_map_button_, &QPushButton::clicked, this,
            &SatelliteScreen::onCollectMap);
    connect(correspond_button_, &QPushButton::clicked, this,
            &SatelliteScreen::showCorrespondPage);

    align_status_ = new QLabel(card);
    align_status_->setObjectName("SatFieldLabel");
    align_status_->setWordWrap(true);
    layout->addWidget(align_status_);
    return card;
}

QWidget* SatelliteScreen::buildCorrespondPage() {
    // Figma "3D Alignment — Match point cloud" (frames 234:1954 empty,
    // 219:291 captured, 235:2246 picked, 235:3146 aligned): a 45px
    // instruction bar under the stepper, then two panes edge to edge —
    // SATELLITE MAP (58%) left, 3D POINT CLOUD (42%) right. Footer actions
    // (Clear pairs / Align) live in the shared footer bar. No side rail.
    auto* page = new QWidget;
    page->setObjectName("SatCanvasPage");
    page->setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ---- Instruction bar (235:2377) ----
    auto* bar = new QWidget(page);
    bar->setObjectName("SatCorrBar");
    bar->setAttribute(Qt::WA_StyledBackground, true);
    bar->setFixedHeight(kCorrBarHeight);
    auto* bar_layout = new QHBoxLayout(bar);
    bar_layout->setContentsMargins(24, 10, 24, 10);
    bar_layout->setSpacing(12);

    auto* info = new QLabel(bar);
    info->setPixmap(loadTintedSvg(
        QStringLiteral(":/assets/satellite/align_info.svg"), 16, 16));
    info->setFixedSize(16, 16);
    bar_layout->addWidget(info, 0, Qt::AlignVCenter);

    corr_instruction_ = new QLabel(bar);
    corr_instruction_->setObjectName("SatCorrText");
    corr_instruction_->setTextFormat(Qt::RichText);
    bar_layout->addWidget(corr_instruction_, 1, Qt::AlignVCenter);

    // Legend (235:2391): 12px dot + label per pane, then the pair chip.
    auto makeLegendEntry = [&](const QString& dot_color, QLabel** out_label) {
        auto* entry = new QWidget(bar);
        auto* entry_layout = new QHBoxLayout(entry);
        entry_layout->setContentsMargins(0, 0, 0, 0);
        entry_layout->setSpacing(8);
        auto* dot = new QLabel(entry);
        dot->setFixedSize(12, 12);
        dot->setStyleSheet(
            QStringLiteral("background-color: %1; border-radius: 6px;")
                .arg(dot_color));
        entry_layout->addWidget(dot, 0, Qt::AlignVCenter);
        *out_label = new QLabel(entry);
        (*out_label)->setObjectName("SatCorrLegend");
        entry_layout->addWidget(*out_label, 0, Qt::AlignVCenter);
        return entry;
    };
    bar_layout->addWidget(
        makeLegendEntry(QStringLiteral("#2b7fff"), &corr_sat_count_), 0,
        Qt::AlignVCenter);
    bar_layout->addSpacing(4);
    bar_layout->addWidget(
        makeLegendEntry(QStringLiteral("#fe9a00"), &corr_pcd_count_), 0,
        Qt::AlignVCenter);
    bar_layout->addSpacing(4);
    corr_pairs_chip_ = new QLabel(bar);
    corr_pairs_chip_->setObjectName("SatCorrPairsChip");
    corr_pairs_chip_->setFixedHeight(24);
    corr_pairs_chip_->setAlignment(Qt::AlignCenter);
    bar_layout->addWidget(corr_pairs_chip_, 0, Qt::AlignVCenter);
    layout->addWidget(bar);

    // ---- Two panes ----
    auto* split = new QWidget(page);
    split->setObjectName("SatCorrSplit");
    split->setAttribute(Qt::WA_StyledBackground, true);
    auto* split_layout = new QHBoxLayout(split);
    split_layout->setContentsMargins(0, 0, 0, 0);
    split_layout->setSpacing(1);  // the 1 px divider is the split's own bg

    sat_pick_ = new PanZoomImageWidget(split);
    sat_pick_->setCornerTag(QStringLiteral("SATELLITE MAP"));
    sat_pick_->setEmptyText(QString());
    split_layout->addWidget(sat_pick_, kCorrSatPaneStretch);

    pcd_pane_stack_ = new QStackedWidget(split);

    // Empty state (234:2209): 48px frame icon, muted title, green CTA with a
    // 16px refresh glyph, 12px hint. Column is 235px in the frame.
    pcd_empty_ = new QWidget(pcd_pane_stack_);
    pcd_empty_->setObjectName("SatPcdPane");
    pcd_empty_->setAttribute(Qt::WA_StyledBackground, true);
    {
        auto* grid = new QGridLayout(pcd_empty_);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setSpacing(0);
        auto* tag = new QLabel(QStringLiteral("3D POINT CLOUD"), pcd_empty_);
        tag->setObjectName("SatPaneTag");
        grid->addWidget(tag, 0, 0, Qt::AlignLeft | Qt::AlignTop);

        auto* column = new QWidget(pcd_empty_);
        column->setFixedWidth(300);  // hint is 247px wide at 12px
        auto* col = new QVBoxLayout(column);
        col->setContentsMargins(0, 0, 0, 0);
        col->setSpacing(0);
        auto* icon = new QLabel(column);
        icon->setPixmap(loadTintedSvg(
            QStringLiteral(":/assets/satellite/scan_frame.svg"), 48, 48));
        icon->setFixedSize(48, 48);
        col->addWidget(icon, 0, Qt::AlignHCenter);
        col->addSpacing(16);
        pcd_empty_title_ =
            new QLabel(QStringLiteral("Point cloud not yet captured"), column);
        pcd_empty_title_->setObjectName("SatPcdEmptyTitle");
        pcd_empty_title_->setAlignment(Qt::AlignCenter);
        pcd_empty_title_->setWordWrap(true);
        col->addWidget(pcd_empty_title_);
        col->addSpacing(16);
        capture_button_ =
            new QPushButton(QStringLiteral("Capture Point Cloud"), column);
        capture_button_->setObjectName("SatNextButton");
        capture_button_->setIcon(QIcon(loadTintedSvg(
            QStringLiteral(":/assets/satellite/refresh.svg"), 16, 16)));
        capture_button_->setIconSize(QSize(16, 16));
        capture_button_->setFixedHeight(kFooterButtonHeight);
        capture_button_->setCursor(Qt::PointingHandCursor);
        col->addWidget(capture_button_, 0, Qt::AlignHCenter);
        col->addSpacing(16);
        pcd_empty_hint_ = new QLabel(
            QStringLiteral("Robot will rotate 360° to scan surroundings"),
            column);
        pcd_empty_hint_->setObjectName("SatPcdEmptyHint");
        pcd_empty_hint_->setAlignment(Qt::AlignCenter);
        pcd_empty_hint_->setWordWrap(true);
        col->addWidget(pcd_empty_hint_);
        grid->addWidget(column, 0, 0, Qt::AlignCenter);
        connect(capture_button_, &QPushButton::clicked, this,
                &SatelliteScreen::onCollectMap);
    }
    pcd_pane_stack_->addWidget(pcd_empty_);

    // Pick view with the success card (235:4011) floated over its centre
    // once a fit is in. The card is a sibling in the same grid cell so it
    // tracks the pane through resizes without manual geometry.
    pcd_pick_host_ = new QWidget(pcd_pane_stack_);
    {
        auto* grid = new QGridLayout(pcd_pick_host_);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setSpacing(0);
        pcd_pick_ = new PanZoomImageWidget(pcd_pick_host_);
        pcd_pick_->setCornerTag(QStringLiteral("3D POINT CLOUD"));
        // The point-cloud raster is sparse single-pixel hits; smoothing
        // averages them into the transparent background and they disappear.
        pcd_pick_->setSmoothScaling(false);
        grid->addWidget(pcd_pick_, 0, 0);

        align_success_card_ = new QWidget(pcd_pick_host_);
        align_success_card_->setObjectName("SatAlignSuccess");
        align_success_card_->setAttribute(Qt::WA_StyledBackground, true);
        // Let clicks pass to the pane behind: the card is a receipt, and a
        // pick that lands under it should still count.
        align_success_card_->setAttribute(Qt::WA_TransparentForMouseEvents,
                                          true);
        auto* card = new QVBoxLayout(align_success_card_);
        card->setContentsMargins(24, 24, 24, 24);
        card->setSpacing(0);
        auto* check = new QLabel(align_success_card_);
        check->setPixmap(loadTintedSvg(
            QStringLiteral(":/assets/satellite/align_success.svg"), 40, 40));
        check->setFixedSize(40, 40);
        card->addWidget(check, 0, Qt::AlignHCenter);
        card->addSpacing(8);
        auto* title =
            new QLabel(QStringLiteral("Alignment Successful"), align_success_card_);
        title->setObjectName("SatAlignSuccessTitle");
        title->setAlignment(Qt::AlignCenter);
        card->addWidget(title);
        card->addSpacing(4);
        align_success_rmse_ = new QLabel(align_success_card_);
        align_success_rmse_->setObjectName("SatAlignSuccessRmse");
        align_success_rmse_->setAlignment(Qt::AlignCenter);
        card->addWidget(align_success_rmse_);
        align_success_card_->hide();
        grid->addWidget(align_success_card_, 0, 0, Qt::AlignCenter);
    }
    pcd_pane_stack_->addWidget(pcd_pick_host_);
    split_layout->addWidget(pcd_pane_stack_, kCorrPcdPaneStretch);
    layout->addWidget(split, 1);

    connect(sat_pick_, &PanZoomImageWidget::pointPicked, this,
            &SatelliteScreen::onSatellitePicked);
    connect(pcd_pick_, &PanZoomImageWidget::pointPicked, this,
            &SatelliteScreen::onPcdPicked);

    // The frame has no Undo control; Clear pairs is the visible reset. A
    // single misclick should not cost every pick though, so Undo stays as
    // the platform shortcut while the picker is showing.
    auto* undo = new QShortcut(QKeySequence::Undo, page);
    undo->setContext(Qt::WidgetWithChildrenShortcut);
    connect(undo, &QShortcut::activated, this,
            &SatelliteScreen::onUndoCorrespondence);
    return page;
}

QWidget* SatelliteScreen::buildPlanCard(QWidget* parent) {
    auto* card = new QWidget(parent);
    card->setObjectName("SatCard");
    card->setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 16);
    layout->setSpacing(8);
    layout->addWidget(makeCardHeader(QStringLiteral(":/assets/exploration/map.svg"),
                                     QStringLiteral("Plan"), card));

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
    // Step 1's gate reads this field, so the footer has to re-evaluate as it
    // is typed rather than only on save.
    connect(job_name_, &QLineEdit::textChanged, this, [this](const QString&) {
        refreshStepUi();
        refreshTitle();
    });
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
    // In the field the operator opens a plan and needs the canvas on the
    // robot, not on wherever the plan was last panned to in the office. The
    // seed fix from map collection is the only position we have before the
    // correspondence fit lands, so it drives this.
    find_robot_button_ =
        new QPushButton(QStringLiteral("Find Robot"), geo_tools_host_);
    find_robot_button_->setObjectName("SatButton");
    find_robot_button_->setFixedHeight(36);
    find_robot_button_->setCursor(Qt::PointingHandCursor);
    connect(find_robot_button_, &QPushButton::clicked, this,
            &SatelliteScreen::onFindRobot);
    geo_layout->addWidget(find_robot_button_);

    // Source-imagery provenance. Lives inside geo_tools_host_ so
    // applyModeVisibility() hides it on the measured canvas for free.
    imagery_label_ = new QLabel(QStringLiteral("Imagery: —"), geo_tools_host_);
    imagery_label_->setObjectName("SatFieldLabel");
    imagery_label_->setWordWrap(true);
    geo_layout->addWidget(imagery_label_);

    layout->addWidget(geo_tools_host_);

    // ROI drawing, Stage 5 style: shape toggle + one arm button.
    auto* shape_row = new QWidget(card);
    auto* shape_layout = new QHBoxLayout(shape_row);
    shape_layout->setContentsMargins(0, 0, 0, 0);
    shape_layout->setSpacing(8);
    tool_rect_button_ = new QPushButton(QStringLiteral("Rectangle"), shape_row);
    tool_polygon_button_ = new QPushButton(QStringLiteral("Polygon"), shape_row);
    for (QPushButton* button : {tool_rect_button_, tool_polygon_button_}) {
        button->setObjectName("SatToggle");
        button->setCheckable(true);
        button->setAutoExclusive(true);
        button->setFixedHeight(32);
        button->setCursor(Qt::PointingHandCursor);
        shape_layout->addWidget(button, 1);
    }
    tool_rect_button_->setChecked(true);
    // Switching tools mid-draw re-arms with the new tool so the operator
    // is never left holding the other shape's cursor.
    const auto rearmIfDrawing = [this] {
        if (map_->isDrawing()) {
            draw_button_->click();
        }
    };
    connect(tool_rect_button_, &QPushButton::clicked, this, rearmIfDrawing);
    connect(tool_polygon_button_, &QPushButton::clicked, this, rearmIfDrawing);
    layout->addWidget(shape_row);

    auto* draw_row = new QWidget(card);
    auto* draw_layout = new QHBoxLayout(draw_row);
    draw_layout->setContentsMargins(0, 0, 0, 0);
    draw_layout->setSpacing(8);
    draw_button_ = new QPushButton(QStringLiteral("Draw ROI"), draw_row);
    clear_roi_button_ = new QPushButton(QStringLiteral("Clear"), draw_row);
    for (QPushButton* button : {draw_button_, clear_roi_button_}) {
        button->setObjectName("SatButton");
        button->setFixedHeight(36);
        button->setCursor(Qt::PointingHandCursor);
    }
    draw_layout->addWidget(draw_button_, 2);
    draw_layout->addWidget(clear_roi_button_, 1);
    connect(draw_button_, &QPushButton::clicked, this, [this] {
        if (map_->isDrawing()) {
            map_->cancelInteraction();
            return;
        }
        if (tool_polygon_button_->isChecked()) {
            map_->armPolygonDraw();
            appendLog(QStringLiteral(
                "[plan] click each roof corner in order; right-click to "
                "close"));
        } else {
            map_->armRectangleDraw();
            appendLog(QStringLiteral(
                "[plan] drag across the roof from one corner to the "
                "opposite corner"));
        }
    });
    connect(clear_roi_button_, &QPushButton::clicked, this, [this] {
        map_->cancelInteraction();
        map_->setRoi(RoiRect{});
        map_->clearPolygon();
    });
    connect(map_, &SatelliteMapWidget::interactionChanged, this,
            &SatelliteScreen::refreshDrawButton);
    connect(map_, &SatelliteMapWidget::roiChanged, this,
            &SatelliteScreen::refreshDrawButton);
    layout->addWidget(draw_row);

    place_robot_button_ = new QPushButton(QStringLiteral("Place Robot"), card);
    place_robot_button_->setObjectName("SatButton");
    place_robot_button_->setFixedHeight(36);
    place_robot_button_->setCursor(Qt::PointingHandCursor);
    connect(place_robot_button_, &QPushButton::clicked, this, [this] {
        map_->armMarkerPlacement();
        appendLog(QStringLiteral(
            "[plan] click the canvas where the robot physically sits; "
            "click the marker to rotate it, drag to move it"));
    });
    layout->addWidget(place_robot_button_);

    roi_numeric_host_ = new QWidget(card);
    auto* numeric_layout = new QVBoxLayout(roi_numeric_host_);
    numeric_layout->setContentsMargins(0, 0, 0, 0);
    numeric_layout->setSpacing(8);
    const QString length_suffix =
        QStringLiteral(" ") + units::lengthUnitSuffix();
    roi_length_ = makeSpin(2.0, 2000.0, 0.5, 1, length_suffix);
    roi_width_ = makeSpin(2.0, 2000.0, 0.5, 1, length_suffix);
    roi_heading_ = makeSpin(0.0, 359.9, 1.0, 1, QStringLiteral(" °"));
    roi_heading_->setWrapping(true);
    robot_heading_ = makeSpin(0.0, 359.9, 1.0, 1, QStringLiteral(" °"));
    robot_heading_->setWrapping(true);
    numeric_layout->addWidget(
        makeFieldRow(QStringLiteral("ROI along"), roi_length_, roi_numeric_host_));
    numeric_layout->addWidget(
        makeFieldRow(QStringLiteral("ROI across"), roi_width_, roi_numeric_host_));
    numeric_layout->addWidget(makeFieldRow(QStringLiteral("ROI heading"),
                                           roi_heading_, roi_numeric_host_));
    numeric_layout->addWidget(makeFieldRow(QStringLiteral("Robot heading"),
                                           robot_heading_, roi_numeric_host_));
    layout->addWidget(roi_numeric_host_);

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

QWidget* SatelliteScreen::buildSearchBar(QWidget* parent) {
    // Figma 238:4509: 420×48 pill floating 40 px under the step header,
    // search glyph · input · green "Search". Suggestions drop beneath it.
    auto* host = new QWidget(parent);
    host->setObjectName("SatSearchHost");
    host->setAttribute(Qt::WA_TranslucentBackground, true);
    host->setFixedWidth(kSearchBarWidth);
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 40, 0, 0);
    layout->setSpacing(8);

    auto* bar = new QWidget(host);
    bar->setObjectName("SatSearchBar");
    bar->setAttribute(Qt::WA_StyledBackground, true);
    bar->setFixedHeight(48);
    // 238:4509 shadow: 0 25px 50px -12px rgba(0,0,0,.6). The host's bottom
    // margin below leaves room for the blur to paint.
    auto* shadow = new QGraphicsDropShadowEffect(bar);
    shadow->setBlurRadius(50);
    shadow->setOffset(0, 12);
    shadow->setColor(QColor(0, 0, 0, 153));
    bar->setGraphicsEffect(shadow);
    auto* bar_layout = new QHBoxLayout(bar);
    bar_layout->setContentsMargins(16, 0, 16, 0);
    bar_layout->setSpacing(12);
    auto* glyph = new QLabel(bar);
    glyph->setFixedSize(16, 16);
    glyph->setPixmap(loadTintedSvg(QStringLiteral(":/assets/satellite/search.svg"),
                                   16, 16, QStringLiteral("#9f9fa9")));
    bar_layout->addWidget(glyph);
    search_edit_ = new QLineEdit(bar);
    search_edit_->setObjectName("SatSearchEdit");
    search_edit_->setPlaceholderText(
        QStringLiteral("Search address or \"lat, lon\""));
    search_edit_->setFrame(false);
    search_edit_->installEventFilter(this);
    bar_layout->addWidget(search_edit_, 1);
    auto* go = new QPushButton(QStringLiteral("Search"), bar);
    go->setObjectName("SatSearchGo");
    go->setFlat(true);
    go->setCursor(Qt::PointingHandCursor);
    bar_layout->addWidget(go);
    layout->addWidget(bar);

    search_popup_ = new QListWidget(host);
    search_popup_->setObjectName("SatSearchPopup");
    search_popup_->setFocusPolicy(Qt::NoFocus);  // the edit keeps the caret
    search_popup_->setFrameShape(QFrame::NoFrame);
    search_popup_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    search_popup_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    search_popup_->setMouseTracking(true);
    search_popup_->setCursor(Qt::PointingHandCursor);
    search_popup_->hide();
    layout->addWidget(search_popup_);
    layout->addStretch(1);

    // Debounced type-ahead: 300 ms after the last keystroke, ≥ 3 chars.
    search_timer_ = new QTimer(this);
    search_timer_->setSingleShot(true);
    search_timer_->setInterval(300);
    connect(search_timer_, &QTimer::timeout, this,
            &SatelliteScreen::requestSuggestions);
    connect(search_edit_, &QLineEdit::textEdited, this, [this](const QString& t) {
        if (t.trimmed().size() < 3) {
            hideSuggestions();
            search_timer_->stop();
            return;
        }
        search_timer_->start();
    });
    const auto submit = [this] {
        const int row = search_popup_->isVisible() ? search_popup_->currentRow() : -1;
        if (row >= 0) {
            acceptSuggestion(row);
        } else {
            hideSuggestions();
            goToAddress(search_edit_->text(), QString());
        }
    };
    connect(search_edit_, &QLineEdit::returnPressed, this, submit);
    connect(go, &QPushButton::clicked, this, submit);
    connect(search_popup_, &QListWidget::itemClicked, this,
            [this](QListWidgetItem* item) {
                acceptSuggestion(search_popup_->row(item));
            });
    connect(search_popup_, &QListWidget::itemEntered, this,
            [this](QListWidgetItem* item) {
                search_popup_->setCurrentItem(item);
            });
    return host;
}

TileService::GeocodeBias SatelliteScreen::geocodeBias() const {
    TileService::GeocodeBias bias;
    bias.valid = true;
    bias.lat = map_->centerLat();
    bias.lon = map_->centerLon();
    return bias;
}

void SatelliteScreen::requestSuggestions() {
    const QString text = search_edit_->text().trimmed();
    if (text.size() < 3 || plan_mode_ != PlanMode::Satellite) {
        return;
    }
    const quint64 seq = ++suggest_seq_;
    tiles_->suggest(text, geocodeBias(),
                    [this, seq](QVector<TileService::Suggestion> items) {
                        if (seq != suggest_seq_ || !search_edit_->hasFocus()) {
                            return;  // stale, or the operator moved on
                        }
                        suggestions_ = std::move(items);
                        search_popup_->clear();
                        if (suggestions_.isEmpty()) {
                            hideSuggestions();
                            return;
                        }
                        for (const auto& s : suggestions_) {
                            auto* item = new QListWidgetItem(s.text);
                            item->setSizeHint(QSize(0, kSearchRowHeight));
                            search_popup_->addItem(item);
                        }
                        search_popup_->setCurrentRow(-1);
                        search_popup_->setFixedHeight(
                            suggestions_.size() * kSearchRowHeight + 2);
                        search_popup_->show();
                    });
}

void SatelliteScreen::hideSuggestions() {
    if (search_popup_) {
        search_popup_->hide();
        search_popup_->clear();
    }
    suggestions_.clear();
}

void SatelliteScreen::acceptSuggestion(int row) {
    if (row < 0 || row >= suggestions_.size()) {
        return;
    }
    const TileService::Suggestion s = suggestions_[row];
    search_edit_->setText(s.text);
    hideSuggestions();
    goToAddress(s.text, s.magic_key);
}

void SatelliteScreen::goToAddress(const QString& raw, const QString& magic_key) {
    const QString query = raw.trimmed();
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
            canvas_aimed_ = true;
            refreshStepUi();
            return;
        }
    }
    appendLog(QStringLiteral("[geo] searching '%1'…").arg(query));
    TileService::GeocodeBias bias = geocodeBias();
    bias.magic_key = magic_key;
    tiles_->geocode(query, bias,
                    [this](bool ok, double lat, double lon, QString label,
                           bool rooftop) {
                        appendLog(QStringLiteral("[geo] %1").arg(label));
                        if (!ok) {
                            return;
                        }
                        // Rooftop-grade match: land on the roof. An
                        // interpolated one can be a parcel off, so stay a
                        // notch wider and let the operator pick the building.
                        map_->setView(lat, lon, rooftop ? 19 : 18);
                        canvas_aimed_ = true;
                        // Field rail has no address field: the search that
                        // aimed the canvas is the plan's reference address.
                        if (job_address_->text().trimmed().isEmpty()) {
                            job_address_->setText(label.section(QStringLiteral("  ["), 0, 0));
                        }
                        refreshStepUi();
                    });
}

QWidget* SatelliteScreen::buildRoiCard(QWidget* parent) {
    // Figma 222:1284 — 16px padding, 255px content column.
    auto* card = new QWidget(parent);
    card->setObjectName("SatRoiCard");
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* title = new QLabel(QStringLiteral("ROI Definition"), card);
    title->setObjectName("SatRoiTitle");
    layout->addWidget(title);
    auto* blurb = new QLabel(
        QStringLiteral("Click on the roof area to add polygon vertices. "
                       "Click near the first point to close the region."),
        card);
    blurb->setObjectName("SatRoiBlurb");
    blurb->setWordWrap(true);
    layout->addSpacing(4);
    layout->addWidget(blurb);

    // Stats box (222:1294).
    auto* stats = new QWidget(card);
    stats->setObjectName("SatRoiStats");
    stats->setAttribute(Qt::WA_StyledBackground, true);
    auto* stats_layout = new QVBoxLayout(stats);
    stats_layout->setContentsMargins(12, 12, 12, 12);
    stats_layout->setSpacing(6);
    const auto statRow = [&](const QString& key, QLabel** value) {
        auto* row = new QWidget(stats);
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        auto* k = new QLabel(key, row);
        k->setObjectName("SatRoiStatKey");
        *value = new QLabel(row);
        (*value)->setObjectName("SatRoiStatValue");
        rl->addWidget(k);
        rl->addStretch(1);
        rl->addWidget(*value);
        stats_layout->addWidget(row);
    };
    statRow(QStringLiteral("Vertices"), &roi_stat_vertices_);
    statRow(QStringLiteral("Status"), &roi_stat_status_);
    statRow(QStringLiteral("Area"), &roi_stat_area_);
    layout->addSpacing(16);
    layout->addWidget(stats);

    // Edge Dimensions (222:1318).
    auto* edges_title = new QLabel(QStringLiteral("EDGE DIMENSIONS"), card);
    edges_title->setObjectName("SatRoiSection");
    layout->addSpacing(16);
    layout->addWidget(edges_title);
    auto* rows_host = new QWidget(card);
    edge_rows_layout_ = new QVBoxLayout(rows_host);
    edge_rows_layout_->setContentsMargins(0, 8, 0, 0);
    edge_rows_layout_->setSpacing(4);
    layout->addWidget(rows_host);
    auto* edges_hint = new QLabel(
        QStringLiteral("Click a value to edit it. Endpoint moves to match."),
        card);
    edges_hint->setObjectName("SatRoiHint");
    edges_hint->setWordWrap(true);
    edges_hint->setContentsMargins(4, 6, 0, 0);
    layout->addWidget(edges_hint);

    // Clear ROI (222:1367).
    roi_clear_button_ = new QPushButton(QStringLiteral("Clear ROI"), card);
    roi_clear_button_->setObjectName("SatRoiClearButton");
    roi_clear_button_->setIcon(QIcon(loadTintedSvg(
        QStringLiteral(":/assets/satellite/clear_roi.svg"), 14, 14)));
    roi_clear_button_->setIconSize(QSize(14, 14));
    roi_clear_button_->setFixedHeight(36);
    roi_clear_button_->setCursor(Qt::PointingHandCursor);
    connect(roi_clear_button_, &QPushButton::clicked, this, [this] {
        map_->cancelInteraction();
        map_->setRoi(RoiRect{});
        map_->clearPolygon();
        // The frame has no Draw button: an empty step 3 canvas is always
        // ready to take vertices.
        map_->armPolygonDraw();
    });
    layout->addSpacing(16);
    layout->addWidget(roi_clear_button_);

    // Boundary note (222:1380).
    auto* note = new QWidget(card);
    note->setObjectName("SatRoiNote");
    note->setAttribute(Qt::WA_StyledBackground, true);
    auto* note_layout = new QHBoxLayout(note);
    note_layout->setContentsMargins(12, 12, 12, 12);
    note_layout->setSpacing(8);
    auto* note_icon = new QLabel(note);
    note_icon->setPixmap(loadTintedSvg(
        QStringLiteral(":/assets/satellite/roi_hint.svg"), 16, 16));
    note_icon->setFixedSize(16, 16);
    note_layout->addWidget(note_icon, 0, Qt::AlignTop);
    auto* note_text = new QLabel(
        QStringLiteral("Stay within the building boundary. The robot will "
                       "autonomously scan the entire ROI."),
        note);
    note_text->setObjectName("SatRoiBlurb");
    note_text->setWordWrap(true);
    note_layout->addWidget(note_text, 1);
    layout->addSpacing(24);
    layout->addWidget(note);

    connect(map_, &SatelliteMapWidget::roiChanged, this,
            &SatelliteScreen::refreshRoiCard);
    connect(map_, &SatelliteMapWidget::interactionChanged, this,
            &SatelliteScreen::refreshRoiCard);
    // The row whose chip is open turns green with it.
    connect(map_, &SatelliteMapWidget::edgeEditChanged, this, [this](int edge) {
        for (int i = 0; i < edge_rows_.size(); ++i) {
            edge_rows_[i]->setProperty("editing", i == edge);
            edge_rows_[i]->style()->unpolish(edge_rows_[i]);
            edge_rows_[i]->style()->polish(edge_rows_[i]);
            edge_value_buttons_[i]->style()->unpolish(edge_value_buttons_[i]);
            edge_value_buttons_[i]->style()->polish(edge_value_buttons_[i]);
        }
        if (edge < 0) {
            refreshRoiCard();
        }
    });
    connect(UnitsProvider::instance(), &UnitsProvider::unitsChanged, this,
            [this] { refreshRoiCard(); });
    refreshRoiCard();
    return card;
}

void SatelliteScreen::refreshRoiCard() {
    if (!roi_card_) {
        return;
    }
    const RoiPolygon& poly = map_->polygon();
    const QVector<double> lengths = map_->edgeLengthsM();
    const int n = poly.vertices.size();
    const bool drawing = map_->isDrawing();
    const bool closed = poly.valid() && !drawing;

    roi_stat_vertices_->setText(QString::number(n));
    roi_stat_status_->setText(closed  ? QStringLiteral("Closed ✓")
                              : drawing ? QStringLiteral("Drawing…")
                                        : QStringLiteral("No ROI"));
    roi_stat_status_->setProperty("state", closed ? "closed" : drawing ? "drawing" : "none");
    roi_stat_status_->style()->unpolish(roi_stat_status_);
    roi_stat_status_->style()->polish(roi_stat_status_);
    double area = 0.0;
    if (closed) {
        // Shoelace on ENU metres about the first vertex.
        for (int i = 0; i < n; ++i) {
            const QPointF a = geo::enuFromGeo(poly.vertices[0], poly.vertices[i]);
            const QPointF b =
                geo::enuFromGeo(poly.vertices[0], poly.vertices[(i + 1) % n]);
            area += a.x() * b.y() - b.x() * a.y();
        }
        area = std::abs(area) * 0.5;
    }
    roi_stat_area_->setText(closed ? units::formatArea(area, 1)
                                   : QStringLiteral("—"));
    if (canvas_tag_) {
        canvas_tag_->setText(
            closed ? QStringLiteral("ROI DEFINED — click edge labels to edit "
                                    "dimensions")
            : drawing ? QStringLiteral("DRAWING ROI — click corners; click "
                                       "the first point to close")
                      : QStringLiteral("NO ROI — click the roof to start"));
        canvas_tag_->setVisible(!roi_card_->isHidden());
    }

    // Rows: only rebuild when the count changes; retitle otherwise.
    const int rows_wanted = closed ? lengths.size() : 0;
    if (edge_rows_.size() != rows_wanted) {
        for (QWidget* row : edge_rows_) {
            row->deleteLater();
        }
        edge_rows_.clear();
        edge_value_buttons_.clear();
        for (int i = 0; i < rows_wanted; ++i) {
            auto* row = new QWidget(edge_rows_layout_->parentWidget());
            row->setObjectName("SatRoiEdgeRow");
            row->setAttribute(Qt::WA_StyledBackground, true);
            row->setFixedHeight(28);
            auto* rl = new QHBoxLayout(row);
            rl->setContentsMargins(10, 0, 10, 0);
            auto* name = new QLabel(QStringLiteral("Edge %1").arg(i + 1), row);
            name->setObjectName("SatRoiEdgeName");
            rl->addWidget(name);
            rl->addStretch(1);
            auto* value = new QPushButton(row);
            value->setObjectName("SatRoiEdgeValue");
            value->setCursor(Qt::PointingHandCursor);
            value->setFlat(true);
            connect(value, &QPushButton::clicked, this,
                    [this, i] { map_->beginEdgeLengthEdit(i); });
            // Hovering the row lights the matching chip on the canvas.
            row->installEventFilter(this);
            row->setProperty("edgeIndex", i);
            rl->addWidget(value);
            edge_rows_layout_->addWidget(row);
            edge_rows_.append(row);
            edge_value_buttons_.append(value);
        }
    }
    for (int i = 0; i < rows_wanted; ++i) {
        edge_value_buttons_[i]->setText(units::formatLength(lengths[i], 2));
    }
}

QWidget* SatelliteScreen::buildCanvasTools(QWidget* parent) {
    // Ported from the Stage 5 PlotWidget tool stack: view tools live on the
    // canvas edge, not in the rail, because they are about looking rather
    // than planning. Icons come from the same missionplanner set.
    // Figma 238:4518: separate 32 px pills, 6 px apart, 12 px in from the
    // canvas corner. The frames carry zoom-in + fit only; the field trim
    // hides the rest (wheel still zooms out), the office keeps all four.
    auto* host = new QWidget(parent);
    host->setObjectName("SatCanvasTools");
    host->setAttribute(Qt::WA_TranslucentBackground, true);
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 12, 12, 0);
    layout->setSpacing(6);

    const auto makeTool = [&](const QString& icon, const QString& glyph,
                              const QString& tooltip) {
        auto* button = new QPushButton(glyph, host);
        button->setObjectName("SatCanvasTool");
        button->setFixedSize(32, 32);
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(tooltip);
        button->setIconSize(QSize(14, 14));
        layout->addWidget(button);
        canvas_tool_buttons_.append({button, icon});
        return button;
    };

    auto* zoom_in = makeTool(
        QStringLiteral(":/assets/satellite/tool_zoom_in.svg"), QString(),
        QStringLiteral("Zoom in"));
    connect(zoom_in, &QPushButton::clicked, map_, &SatelliteMapWidget::zoomIn);
    zoom_out_button_ = makeTool(QString(), QStringLiteral("−"),
                                QStringLiteral("Zoom out"));
    connect(zoom_out_button_, &QPushButton::clicked, map_,
            &SatelliteMapWidget::zoomOut);
    auto* fit = makeTool(QStringLiteral(":/assets/satellite/tool_fit.svg"),
                         QString(), QStringLiteral("Fit to ROI"));
    connect(fit, &QPushButton::clicked, this, [this] {
        if (!map_->fitToRoi()) {
            appendLog(QStringLiteral("[view] draw an ROI first"));
        }
    });
    measure_button_ = makeTool(
        QStringLiteral(":/assets/missionplanner/tool_ruler.svg"), QString(),
        QStringLiteral("Measure — click two points; Esc clears"));
    measure_button_->setCheckable(true);
    connect(measure_button_, &QPushButton::clicked, this, [this](bool on) {
        if (on) {
            map_->startMeasure();
        } else {
            map_->clearMeasure();
        }
    });
    // The ruler can also end from the canvas (Esc, right-click, another
    // tool arming), so the button mirrors the widget rather than owning it.
    connect(map_, &SatelliteMapWidget::interactionChanged, this, [this] {
        measure_button_->setChecked(map_->isMeasuring());
    });
    // A roof drawn at site-overview zoom is a few dozen pixels wide — too
    // small to grab a vertex or read an edge chip. Frame it as soon as the
    // gesture completes; the operator can wheel back out if they want.
    connect(map_, &SatelliteMapWidget::drawFinished, this,
            [this] { map_->fitToRoi(); });

    refreshCanvasToolIcons();
    return host;
}

void SatelliteScreen::refreshCanvasToolIcons() {
    const QString color = textColor(dark_mode_);
    for (const CanvasTool& tool : canvas_tool_buttons_) {
        if (tool.icon.isEmpty()) {
            continue;
        }
        tool.button->setIcon(QIcon(loadTintedSvg(tool.icon, 16, 16, color)));
    }
}

void SatelliteScreen::refreshDrawButton() {
    if (!draw_button_) {
        return;
    }
    if (map_->isDrawing()) {
        draw_button_->setText(tool_polygon_button_->isChecked()
                                  ? QStringLiteral("Drawing… (right-click to close)")
                                  : QStringLiteral("Drawing… (drag)"));
        return;
    }
    draw_button_->setText(map_->polygon().valid()
                              ? QStringLiteral("Redraw ROI")
                              : QStringLiteral("Draw ROI"));
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
    end_button_ = new QPushButton(QStringLiteral("Complete Mission"), row1);
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
            &SatelliteScreen::onCompleteMission);
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
QLabel#SatBackLabel { background: transparent; font-size: 14px; color: @TEXT@; }
#SatMotorsChip {
    background-color: transparent; border: 1px solid @CARD_BORDER@; border-radius: 10px;
}
#SatStepHeader {
    background-color: @SURFACE@; border-bottom: 1px solid @SURFACE_BORDER@;
}
#SatFooterBar {
    background-color: @SURFACE@; border-top: 1px solid @SURFACE_BORDER@;
}
QPushButton#SatNextButton {
    background-color: #009966; border: none; border-radius: 10px;
    font-family: 'Arimo'; font-weight: 700; font-size: 14px; color: #FFFFFF;
    padding: 0 24px;
}
QPushButton#SatNextButton:hover { background-color: #00A86D; }
QPushButton#SatNextButton:disabled {
    background-color: rgba(0, 153, 102, 0.40); color: rgba(255, 255, 255, 0.40);
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
QPushButton#SatToggle {
    background-color: transparent; border: 1px solid @INPUT_BORDER@; border-radius: 8px;
    font-family: 'Arimo'; font-weight: 600; font-size: 12px; color: @MUTED@;
    padding: 0 10px;
}
QPushButton#SatToggle:hover { background-color: @BUTTON_HOVER@; }
QPushButton#SatToggle:checked {
    background-color: rgba(0, 188, 125, 0.15); border-color: #00BC7D; color: @TEXT@;
}
QPushButton#SatToggle:disabled { color: @MUTED@; background-color: transparent; }
#SatCanvasTools { background: transparent; }
QPushButton#SatCanvasTool {
    background-color: rgba(24, 24, 27, 0.90); border: 1px solid #3f3f47; border-radius: 10px;
    font-family: 'Arimo'; font-weight: 700; font-size: 16px; color: @TEXT@;
}
QPushButton#SatCanvasTool:hover { background-color: @BUTTON_HOVER@; }
QPushButton#SatCanvasTool:checked { background-color: rgba(0, 188, 125, 0.18); color: #00BC7D; }
QPushButton#SatCanvasTool:disabled { color: @MUTED@; }
QPushButton#SatSendButton {
    background-color: #00BC7D; border: none; border-radius: 10px;
    font-family: 'Arimo'; font-weight: 700; font-size: 14px; color: #FFFFFF;
    padding: 0 16px;
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
#SatCanvasPage { background-color: @PAGE@; }
/* ---- Step 2 picker (Figma 235:2246 family; dark-only surfaces) ---- */
#SatCorrBar {
    background-color: rgba(39, 39, 42, 0.80); border-bottom: 1px solid #3f3f47;
}
QLabel#SatCorrText { background: transparent; font-family: 'Arimo'; font-size: 14px; }
QLabel#SatCorrLegend {
    background: transparent; font-family: 'Arimo'; font-size: 14px; color: #9f9fa9;
}
QLabel#SatCorrPairsChip {
    background-color: rgba(63, 63, 71, 0.40); border-radius: 4px; padding: 0 8px;
    font-family: 'Liberation Mono', 'DejaVu Sans Mono', monospace; font-size: 14px;
    color: #9f9fa9;
}
QLabel#SatCorrPairsChip[satisfied="true"] {
    background-color: rgba(0, 188, 125, 0.10); color: #00d492;
}
/* Footer ghost buttons (235:3115 / 235:3121): no border, icon + label. */
QPushButton#SatGhostButton, QPushButton#SatGhostButtonMuted {
    background-color: transparent; border: none; border-radius: 10px;
    font-family: 'Arimo'; font-weight: 500; font-size: 14px; color: #9f9fa9;
    padding: 0 16px;
}
QPushButton#SatGhostButtonMuted { color: #71717b; padding: 0 12px; }
QPushButton#SatGhostButton:hover, QPushButton#SatGhostButtonMuted:hover {
    background-color: @BUTTON_HOVER@;
}
QPushButton#SatGhostButton:disabled, QPushButton#SatGhostButtonMuted:disabled {
    color: rgba(113, 113, 123, 0.40);
}
/* Align (235:3131): zinc primary, 40% opacity when disabled. */
QPushButton#SatAlignButton {
    background-color: #3f3f47; border: none; border-radius: 10px;
    font-family: 'Arimo'; font-weight: 700; font-size: 14px; color: #FFFFFF;
    padding: 0 20px;
}
QPushButton#SatAlignButton:hover { background-color: #52525c; }
QPushButton#SatAlignButton:disabled {
    background-color: rgba(63, 63, 71, 0.40); color: rgba(255, 255, 255, 0.40);
}
#SatCorrSplit { background-color: #27272a; }
#SatPcdPane { background-color: #0b0b0b; }
QLabel#SatPaneTag {
    background-color: rgba(24, 24, 27, 0.90); border: 1px solid #3f3f47; border-radius: 4px;
    font-family: 'Liberation Mono', 'DejaVu Sans Mono', monospace; font-size: 12px;
    color: #d4d4d8; padding: 4px 10px; margin: 8px 12px;
}
QLabel#SatPcdEmptyTitle {
    background: transparent; font-family: 'Arimo'; font-size: 14px; color: #71717b;
}
QLabel#SatPcdEmptyHint {
    background: transparent; font-family: 'Arimo'; font-size: 12px; color: #52525c;
}
#SatAlignSuccess {
    background-color: #18181b; border: 1px solid rgba(0, 188, 125, 0.50); border-radius: 14px;
}
QLabel#SatAlignSuccessTitle {
    background: transparent; font-family: 'Arimo'; font-weight: 600; font-size: 16px;
    color: #FFFFFF;
}
QLabel#SatAlignSuccessRmse {
    background: transparent; font-family: 'Liberation Mono', 'DejaVu Sans Mono', monospace;
    font-size: 14px; color: #9f9fa9;
}

/* ---- Step 3 ROI Definition rail (Figma 222:1284) ---- */
QLabel#SatRoiTitle { background: transparent; font-size: 18px; font-weight: 600; color: @TEXT@; }
QLabel#SatRoiBlurb { background: transparent; font-size: 12px; color: #71717b; line-height: 19px; }
#SatRoiStats { background: #27272a; border: 1px solid #3f3f47; border-radius: 10px; }
QLabel#SatRoiStatKey, QLabel#SatRoiStatValue {
    background: transparent; font-family: 'Liberation Mono', 'DejaVu Sans Mono', monospace; font-size: 12px;
}
QLabel#SatRoiStatKey { color: #9f9fa9; }
QLabel#SatRoiStatValue { color: @TEXT@; }
QLabel#SatRoiStatValue[state="closed"] { color: #00d492; }
QLabel#SatRoiStatValue[state="drawing"] { color: #fe9a00; }
QLabel#SatRoiSection { background: transparent; font-size: 12px; color: #71717b; letter-spacing: 0.6px; }
#SatRoiEdgeRow { background: #27272a; border-radius: 4px; }
#SatRoiEdgeRow:hover { background: #3f3f47; }
#SatRoiEdgeRow[editing="true"] { background: rgba(0,153,102,0.25); border: 1px solid #00d492; }
QLabel#SatRoiEdgeName { background: transparent; font-size: 12px; color: #9f9fa9; }
QPushButton#SatRoiEdgeValue {
    background: transparent; border: none; padding: 0px;
    font-family: 'Liberation Mono', 'DejaVu Sans Mono', monospace; font-size: 12px; font-weight: 500;
    color: @TEXT@; text-decoration: underline;
}
QPushButton#SatRoiEdgeValue:hover { color: #00d492; }
#SatRoiEdgeRow[editing="true"] QPushButton#SatRoiEdgeValue { color: #00d492; }
QLabel#SatRoiHint { background: transparent; font-size: 12px; color: #52525c; }
QPushButton#SatRoiClearButton {
    background: #27272a; border: none; border-radius: 10px; padding: 8px 12px;
    font-size: 14px; font-weight: 500; color: #9f9fa9;
}
QPushButton#SatRoiClearButton:hover { background: #3f3f47; color: @TEXT@; }
#SatRoiNote { background: rgba(39,39,42,0.6); border-radius: 10px; }

/* ---- Step 1 floating search (Figma 238:4509) + layer chip (238:4531) ---- */
#SatSearchBar { background: rgba(24,24,27,0.95); border: 1px solid #3f3f47; border-radius: 24px; }
QLineEdit#SatSearchEdit {
    background: transparent; border: none; padding: 0px;
    font-size: 14px; color: #ffffff; selection-background-color: #009966;
}
QPushButton#SatSearchGo {
    background: transparent; border: none; padding: 0px 2px 0px 0px;
    font-size: 12px; font-weight: 600; color: #00d492;
}
QPushButton#SatSearchGo:hover { color: #5ee9b5; }
QListWidget#SatSearchPopup {
    background: rgba(24,24,27,0.95); border: 1px solid #3f3f47; border-radius: 12px;
    padding: 0px; outline: none;
}
QListWidget#SatSearchPopup::item {
    height: 40px; padding: 0px 16px; font-size: 14px; color: #e4e4e7; border: none;
}
QListWidget#SatSearchPopup::item:hover,
QListWidget#SatSearchPopup::item:selected { background: #27272a; color: #00d492; }
#SatLayerChip { background: rgba(24,24,27,0.9); border: 1px solid #3f3f47; border-radius: 10px; }
QLabel#SatLayerChipText {
    background: transparent; font-family: 'Liberation Mono', 'DejaVu Sans Mono', monospace;
    font-size: 12px; color: #9f9fa9;
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
    refreshCanvasToolIcons();
    // Step chips carry per-element colours for the same reason the pills do,
    // so they need re-rendering here too — applyTheme()'s sheet does not
    // reach them.
    refreshStepUi();
    // The imagery label carries an inline colour too. Re-resolving is free
    // (the provenance cache answers without a request) and repaints it
    // against the new palette on the next slow_timer_ tick.
    imagery_query_pending_ = true;
}

// ---- Jobs -------------------------------------------------------------------

void SatelliteScreen::populateJobsCombo(const QString& select_id) {
    jobs_ = job_store_.loadAll();
    jobs_combo_->clear();
    jobs_combo_->addItem(QStringLiteral("— unsaved plan —"), QString());
    // PLANNED first, then a separator, then COMPLETED (newest scan first) —
    // the same split Scan Setup shows. loadAll() is updated-desc already.
    int select_index = 0;
    auto add = [&](const Job& job) {
        jobs_combo_->addItem(job.name.isEmpty() ? job.id : job.name, job.id);
        if (!select_id.isEmpty() && job.id == select_id) {
            select_index = jobs_combo_->count() - 1;
        }
    };
    for (const Job& job : jobs_) {
        if (!job.executed()) {
            add(job);
        }
    }
    QVector<const Job*> completed;
    for (const Job& job : jobs_) {
        if (job.executed()) {
            completed.append(&job);
        }
    }
    if (!completed.isEmpty()) {
        std::sort(completed.begin(), completed.end(),
                  [](const Job* a, const Job* b) {
                      return a->last_executed_at > b->last_executed_at;
                  });
        jobs_combo_->insertSeparator(jobs_combo_->count());
        for (const Job* job : completed) {
            add(*job);
        }
    }
    jobs_combo_->setCurrentIndex(select_index);
}

void SatelliteScreen::refreshJobsCombo(const QString& select_id) {
    populateJobsCombo(select_id);
    const QString id = jobs_combo_->currentData().toString();
    if (id.isEmpty()) {
        return;
    }
    for (const Job& job : jobs_) {
        if (job.id == id) {
            loadJob(job);
            return;
        }
    }
}

void SatelliteScreen::markCurrentPlanCompleted() {
    if (current_job_id_.isEmpty()) {
        return;  // unsaved plan — nothing on disk to archive
    }
    const QString id = current_job_id_;
    bool stamped = false;
    for (Job& job : jobs_) {
        if (job.id == id) {
            job.last_executed_at = QDateTime::currentDateTime();
            QString error;
            stamped = job_store_.save(job, &error);
            appendLog(stamped
                          ? QStringLiteral("[plan] '%1' marked completed")
                                .arg(job.name)
                          : QStringLiteral("[plan] could not mark '%1' "
                                           "completed: %2")
                                .arg(job.name, error));
            break;
        }
    }
    if (!stamped) {
        return;
    }
    const QStringList pruned =
        job_store_.pruneCompleted(JobStore::kCompletedPlansKept);
    if (!pruned.isEmpty()) {
        appendLog(QStringLiteral("[plan] pruned %1 older completed plan(s): %2")
                      .arg(pruned.size())
                      .arg(pruned.join(QStringLiteral(", "))));
    }
    // Rebuild the office combo (hidden in the field) without reloading the
    // canvas — teardown owns the screen state from here.
    populateJobsCombo(id);
}

void SatelliteScreen::loadJob(const Job& job) {
    if (job.id != current_job_id_) {
        // A different plan: the collected map, picks and fit belonged to the
        // previous one. Re-selecting the same id (every save routes back
        // through here) keeps the session — mid-alignment saves happen.
        resetAlignmentSession();
    }
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
    // A plan that already carries geometry or a GPS seed was aimed when it
    // was authored; re-aiming it in the field would be busywork. The ROI
    // confirmation deliberately does NOT carry over — an office-drawn
    // outline is exactly what step 3 exists to check against the real roof.
    canvas_aimed_ = job.polygon.valid() || job.roi.valid || job.gps.valid ||
                    job.robot.valid;
    confirmed_vertices_.clear();
    edges_reviewed_ = false;
    // A saved plan's stored fit was solved against a map collected on a
    // previous outing; the robot is somewhere else now. Alignment is a field
    // step every time.
    alignment_confirmed_ = false;
    appendLog(QStringLiteral("[plan] loaded '%1'").arg(job.name));
}

void SatelliteScreen::newJob() {
    resetAlignmentSession();
    current_job_id_.clear();
    jobs_combo_->setCurrentIndex(0);
    job_name_->clear();
    job_address_->clear();
    map_->setRoi(RoiRect{});
    map_->clearPolygon();
    map_->setMarker(geo::GeoPose{});
    canvas_aimed_ = false;
    confirmed_vertices_.clear();
    edges_reviewed_ = false;
    alignment_confirmed_ = false;
    selected_step_ = Step::SatelliteMap;
    emit map_->roiChanged();
    emit map_->markerChanged();
}

Job SatelliteScreen::jobFromRail() const {
    Job job;
    job.name = job_name_->text().trimmed();
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
    return job;
}

bool SatelliteScreen::persistJob(const Job& job) {
    QString error;
    if (!job_store_.save(job, &error)) {
        appendLog(QStringLiteral("[plan] save failed: %1").arg(error));
        return false;
    }
    current_job_id_ = job.id;
    refreshJobsCombo(job.id);
    return true;
}

void SatelliteScreen::adoptImageryManifest(
    Job& job, const TileService::SiteManifest& manifest) {
    job.imagery_cache.cached = manifest.cached;
    job.imagery_cache.max_zoom = manifest.max_zoom;
    job.imagery_cache.captured = manifest.captured;
    job.imagery_cache.res_m = manifest.res_m;
    job.imagery_cache.layer = manifest.layer;
    job.imagery_cache.wayback_release = manifest.wayback_release;
    job.imagery_cache.min_date = manifest.min_date;
    job.imagery_cache.stitch_relpath = manifest.stitch_relpath;
    job.imagery_captured = manifest.captured;
    job.imagery_res_m = manifest.res_m;
    job.imagery_zoom = manifest.max_zoom;
    // Hand the canvas to the job's tile tree so the operator sees the
    // offline pyramid they just paid for, capped where it ends.
    applyImageryManifest(manifest, job_store_.assetsDir(job.id));
}

void SatelliteScreen::saveJob() {
    if (job_name_->text().trimmed().isEmpty()) {
        appendLog(QStringLiteral("[plan] give the plan a name before saving"));
        job_name_->setFocus();
        return;
    }
    map_->cancelInteraction();
    Job job = jobFromRail();
    // An operator Save is a statement of intent to scan this plan again:
    // a COMPLETED plan returns to PLANNED. (jobFromRail carries the stamp
    // forward for the non-operator saves — GPS, alignment, prefetch.)
    job.last_executed_at = QDateTime();

    if (plan_mode_ == PlanMode::Measured) {
        // A measured plan was never drawn against imagery: geometry only.
        if (persistJob(job)) {
            appendLog(QStringLiteral("[plan] saved '%1'").arg(job.name));
        }
        return;
    }
    if (planning_only_) {
        // Office: the save IS the imagery step.
        saveSatelliteWithImagery(job);
        return;
    }
    if (job.imagery_cache.cached) {
        // Field edit of a plan that already carries its site pyramid: the
        // roof has no internet, and a geometry tweak must never fetch.
        if (persistJob(job)) {
            appendLog(QStringLiteral("[plan] saved '%1'").arg(job.name));
        }
        return;
    }
    // Field, satellite, nothing cached — the plan was created on site. Whether
    // it can become field-ready is a connectivity question, not a trim one:
    // a laptop on hotspot in the parking lot can cache the site right here.
    save_button_->setEnabled(false);
    save_button_->setText(QStringLiteral("Checking connection…"));
    probeImageryReachable([this, job](bool online) {
        save_button_->setEnabled(true);
        save_button_->setText(QStringLiteral("Save Plan"));
        if (online) {
            saveSatelliteWithImagery(job);
            return;
        }
        if (persistJob(job)) {
            appendLog(QStringLiteral(
                          "[plan] saved '%1' WITHOUT imagery — no connection; "
                          "3D Alignment needs the site cached once")
                          .arg(job.name));
            BdrMessageBox::warning(
                this, QStringLiteral("Saved without imagery"),
                QStringLiteral(
                    "No internet connection, so the satellite site could not "
                    "be cached with this plan.\n\n3D Alignment needs that "
                    "cached site image. Re-save this plan once the laptop has "
                    "a connection (hotspot is fine) and it will download "
                    "then."));
        }
    });
}

void SatelliteScreen::probeImageryReachable(std::function<void(bool)> done) {
    if (!probe_nam_) {
        probe_nam_ = new QNetworkAccessManager(this);
    }
    QNetworkRequest request(TileService::connectivityProbeUrl());
    request.setTransferTimeout(kImageryProbeTimeoutMs);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    QNetworkReply* reply = probe_nam_->head(request);
    connect(reply, &QNetworkReply::finished, this,
            [reply, done = std::move(done)] {
                reply->deleteLater();
                done(reply->error() == QNetworkReply::NoError);
            });
}

void SatelliteScreen::saveSatelliteWithImagery(Job job) {
    // Without an ROI there is nothing to size the download by.
    geo::GeoPoint centroid;
    double roi_radius_m = 0.0;
    if (!map_->roiExtent(&centroid, &roi_radius_m)) {
        appendLog(QStringLiteral(
            "[plan] draw the ROI before saving — it decides which imagery "
            "gets cached for the field"));
        return;
    }
    map_->fitToRoi();
    map_->setMarkerSelected(false);
    const QPixmap thumbnail = map_->grab();

    const QString assets = job_store_.assetsDir(job.id);
    PrefetchRequest request =
        PrefetchRequest::forSite(centroid, roi_radius_m, assets);
    if (job.imagery_cache.cached) {
        // Re-saving a cached plan: keep whatever layer / release the
        // operator chose the first time unless they open Advanced.
        request.clarity = job.imagery_cache.layer == QLatin1String("clarity");
        request.wayback_release = job.imagery_cache.wayback_release;
    }
    const QString shared_cache = tiles_->cacheRoot();

    auto* blur = new QGraphicsBlurEffect(this);
    blur->setBlurRadius(8.0);
    setGraphicsEffect(blur);
    SatellitePlanConfirmDialog dialog(tiles_, job, thumbnail, request,
                                      job.imagery_cache.cached, this);
    dialog.exec();
    setGraphicsEffect(nullptr);  // deletes the effect

    switch (dialog.outcome()) {
        case SatellitePlanConfirmDialog::Outcome::Cancelled:
            // The prefetch may have redirected the cache root before the
            // operator stopped it; put the canvas back on the shared cache.
            if (!job.imagery_cache.cached) {
                tiles_->setCacheRoot(shared_cache);
                tiles_->setMaxZoomCap(0);
            }
            if (current_job_id_.isEmpty()) {
                // Never-saved plan: a stopped download must not leave an
                // orphan assets folder behind for an id nothing references.
                QDir(assets).removeRecursively();
            }
            appendLog(QStringLiteral("[plan] save cancelled"));
            return;
        case SatellitePlanConfirmDialog::Outcome::SavedWithImagery:
            adoptImageryManifest(job, dialog.manifest());
            if (persistJob(job)) {
                appendLog(QStringLiteral("[plan] saved '%1' — imagery cached "
                                         "to z%2 (%3)")
                              .arg(job.name)
                              .arg(job.imagery_cache.max_zoom)
                              .arg(job.imagery_cache.captured.isValid()
                                       ? job.imagery_cache.captured.toString(
                                             Qt::ISODate)
                                       : QStringLiteral("date unknown")));
            }
            return;
        case SatellitePlanConfirmDialog::Outcome::SavedWithoutImagery:
            if (!job.imagery_cache.cached) {
                tiles_->setCacheRoot(shared_cache);
                tiles_->setMaxZoomCap(0);
            }
            if (persistJob(job)) {
                appendLog(QStringLiteral(
                              "[plan] saved '%1' WITHOUT imagery — not "
                              "field-ready until re-saved with a connection")
                              .arg(job.name));
            }
            return;
    }
}

// ---- Navigation -------------------------------------------------------------

void SatelliteScreen::onGoToAddress() {
    goToAddress(address_edit_->text(), QString());
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

    // Provenance describes the tiles actually on screen. Past the fetch
    // ceiling those are the ceiling level scaled up, and Esri has no
    // metadata cell at the (view) zoom anyway.
    const int zoom = std::min(map_->zoom(), map_->fetchZoomCeiling());
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
                if (layer_chip_) {
                    layer_chip_->setToolTip(
                        QStringLiteral("Imagery capture date unavailable"));
                }
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
            if (layer_chip_) {
                // Step 1 has no rail: the provenance rides the layer chip as
                // its tooltip; the chip text stays the layer name (238:4537).
                layer_chip_->setToolTip(QStringLiteral("Imagery: %1").arg(detail));
                layer_chip_text_->setStyleSheet(
                    stale ? QStringLiteral("color: %1;").arg(QLatin1String(kAmber))
                          : QString());
            }

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

void SatelliteScreen::onFindRobot() {
    // Best-to-worst: the confirmed marker (post-alignment, survey grade),
    // this session's collection fix, then the seed saved with the plan.
    const geo::GeoPose marker = map_->marker();
    GpsFix seed = capture_gps_;
    if (!seed.valid) {
        for (const Job& job : jobs_) {
            if (job.id == current_job_id_) {
                seed = job.gps;
                break;
            }
        }
    }
    const int zoom = std::max(map_->zoom(), 19);
    if (marker.valid) {
        map_->setView(marker.lat, marker.lon, zoom);
        canvas_aimed_ = true;
        refreshStepUi();
        appendLog(QStringLiteral("[geo] centred on the aligned robot anchor"));
        return;
    }
    if (seed.valid) {
        map_->setView(seed.lat, seed.lon, zoom);
        canvas_aimed_ = true;
        refreshStepUi();
        appendLog(QStringLiteral("[geo] centred on the GPS seed (%1, hacc %2)")
                      .arg(seed.fix_type.isEmpty()
                               ? QStringLiteral("fix")
                               : seed.fix_type)
                      .arg(units::formatLength(seed.hacc_m, 2)));
        return;
    }
    appendLog(QStringLiteral(
        "[geo] no robot position yet — collect a map or place the marker"));
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

// ---- Alignment --------------------------------------------------------------

int SatelliteScreen::minCorrespondences() const {
    return capture_gps_.valid ? 3 : 5;
}

QString SatelliteScreen::loadSiteImage() {
    if (current_job_id_.isEmpty()) {
        return QStringLiteral(
            "Save the plan first — the site image lives with the job.");
    }
    const QString assets = job_store_.assetsDir(current_job_id_);
    site_manifest_ = TileService::readSiteManifest(
        assets + QStringLiteral("/imagery.json"));
    if (site_manifest_.stitch_relpath.isEmpty()) {
        return QStringLiteral(
            "No cached site image for this plan.\nGo back to Satellite Map "
            "and Save Plan while the laptop has a connection — the site "
            "downloads as part of the save.");
    }
    if (!sat_image_.load(assets + QLatin1Char('/') +
                         site_manifest_.stitch_relpath)) {
        return QStringLiteral("Could not read the stitched site image.");
    }
    if (!site_manifest_.stitch_bounds.isValid()) {
        // Without the Web Mercator extent a fit maps into pixels that mean
        // nothing geographically, so the confirm step could not place the
        // robot. Older manifests predate the field.
        sat_image_ = QImage();
        return QStringLiteral(
            "The cached site image has no geographic bounds — re-save the "
            "plan in the office to regenerate it.");
    }
    return QString();
}

void SatelliteScreen::onCollectMap() {
    if (map_capture_->busy()) {
        map_capture_->cancel();
        return;
    }
    // Measured mode has nothing to align against, so it needs no site image —
    // the collected map lands directly in the grid frame.
    if (plan_mode_ != PlanMode::Measured) {
        const QString image_error = loadSiteImage();
        if (!image_error.isEmpty()) {
            setAlignStatus(image_error);
            appendLog(QStringLiteral("[align] %1").arg(image_error));
            return;
        }
    }
    QString target_error;
    const RobotTarget target =
        MissionController::resolveRobotTarget(&target_error);
    if (!target.valid) {
        setAlignStatus(target_error);
        return;
    }
    if (!confirmDialog(
            QStringLiteral("Collect Map from Robot"),
            QStringLiteral(
                "The robot will ARM ITS MOTORS, turn 360° in place, then "
                "drive a short forward/back leg to resolve a GPS heading.\n\n"
                "Confirm the area around the robot is clear."),
            QStringLiteral("Collect Map"))) {
        return;
    }
    const QString local_dir = job_store_.assetsDir(current_job_id_) +
                              QStringLiteral("/robot_map");
    QString error;
    if (!map_capture_->start(target.host, target.ssh_user, local_dir, &error)) {
        setAlignStatus(error);
        appendLog(QStringLiteral("[align] %1").arg(error));
        return;
    }
    setAlignStatus(QStringLiteral("Starting map collection…"));
    updateAlignCardUi();
}

void SatelliteScreen::onMapCaptured(const MapCapture& capture) {
    if (!capture.valid()) {
        appendLog(QStringLiteral("[align] %1").arg(capture.error));
        updateAlignCardUi();
        setAlignStatus(capture.error);  // after: the refresh resets the title
        return;
    }
    pcd_image_ = capture.image;
    pcd_bounds_m_ = capture.bounds_m;
    capture_gps_ = capture.gps;
    correspondences_.clear();
    have_pending_sat_ = false;
    pcd_to_sat_ = Similarity2D{};
    align_rmse_m_ = 0.0;

    geo::GeoPose marker;
    const bool measured = plan_mode_ == PlanMode::Measured;
    if (measured) {
        // No imagery to align against, and none needed: the measured grid
        // origin IS robot_init, so the collected map already sits in the
        // canvas frame. Anchor the marker there and let the operator draw the
        // ROI around the cloud. Heading 90 makes grid east == robot +x, which
        // is what the body-frame export at Send assumes.
        map_->setMapRaster(pcd_image_, pcd_bounds_m_);
        marker.lat = 0.0;
        marker.lon = 0.0;
        marker.heading_deg = 90.0;
        marker.valid = true;
        map_->setMarker(marker);
        emit map_->markerChanged();
        map_->setView(0.0, 0.0, map_->zoom());
    }

    if (!current_job_id_.isEmpty()) {
        for (Job& job : jobs_) {
            if (job.id != current_job_id_) {
                continue;
            }
            if (capture_gps_.valid) {
                job.gps = capture_gps_;
            }
            if (measured) {
                job.robot = marker;
            }
            job_store_.save(job);
            break;
        }
    }
    appendLog(QStringLiteral("[align] map collected: %1 (%2)")
                  .arg(capture.label,
                       capture_gps_.valid
                           ? QStringLiteral("GPS %1, %2")
                                 .arg(capture_gps_.lat, 0, 'f', 6)
                                 .arg(capture_gps_.lon, 0, 'f', 6)
                           : QStringLiteral("no GPS fix")));
    updateAlignCardUi();
    if (!measured) {
        showCorrespondPage();
    }
}

void SatelliteScreen::showCorrespondPage() {
    // The picker is step 2's whole surface, so it opens even before there is
    // anything to pick: the satellite pane shows the cached site (or why it
    // cannot), and the point-cloud pane shows the capture CTA until a map
    // lands. Nothing here is a hard error — the panes explain themselves.
    const QString image_error = loadSiteImage();
    if (!image_error.isEmpty()) {
        sat_image_ = QImage();
        sat_pick_->setEmptyText(image_error);
        appendLog(QStringLiteral("[align] %1").arg(image_error));
    } else {
        sat_pick_->setEmptyText(QString());
    }
    updateCorrespondenceUi();
    if (selected_step_ != Step::Alignment) {
        // Picking correspondences IS step 2, so the header follows the canvas
        // rather than leaving the operator on a picker while the chips still
        // claim they are somewhere else. setSelectedStep routes back here.
        setSelectedStep(Step::Alignment);
        return;
    }
    canvas_stack_->setCurrentWidget(correspond_page_);
}

void SatelliteScreen::onSatellitePicked(QPointF image_pt) {
    if (have_pending_sat_) {
        return;
    }
    pending_sat_px_ = image_pt;
    have_pending_sat_ = true;
    updateCorrespondenceUi();
}

void SatelliteScreen::onPcdPicked(QPointF image_pt) {
    if (!have_pending_sat_) {
        return;
    }
    Correspondence pair;
    pair.sat_px = pending_sat_px_;
    pair.pcd_m = pcdImageToWorld(pcd_bounds_m_, pcd_image_.size(), image_pt);
    correspondences_.append(pair);
    have_pending_sat_ = false;
    updateCorrespondenceUi();
}

void SatelliteScreen::onUndoCorrespondence() {
    if (pcd_to_sat_.valid) {
        // Undoing into a solved fit means the fit's evidence is changing —
        // it goes with the pick, same as Clear.
        pcd_to_sat_ = Similarity2D{};
        alignment_confirmed_ = false;
    }
    if (have_pending_sat_) {
        have_pending_sat_ = false;
    } else if (!correspondences_.isEmpty()) {
        correspondences_.removeLast();
    }
    updateCorrespondenceUi();
}

void SatelliteScreen::onClearCorrespondences() {
    correspondences_.clear();
    have_pending_sat_ = false;
    pcd_to_sat_ = Similarity2D{};
    align_rmse_m_ = 0.0;
    alignment_confirmed_ = false;
    updateCorrespondenceUi();
}

void SatelliteScreen::resetAlignmentSession() {
    if (map_capture_ && map_capture_->busy()) {
        map_capture_->cancel();
    }
    pcd_image_ = QImage();
    pcd_bounds_m_ = QRectF();
    capture_gps_ = GpsFix{};
    sat_image_ = QImage();
    site_manifest_ = TileService::SiteManifest{};
    correspondences_.clear();
    have_pending_sat_ = false;
    pcd_to_sat_ = Similarity2D{};
    align_rmse_m_ = 0.0;
    alignment_confirmed_ = false;
    if (sat_pick_) {
        sat_pick_->clearOverlays();
        sat_pick_->setImage(QImage());
        pcd_pick_->clearOverlays();
        pcd_pick_->setImage(QImage());
        updateCorrespondenceUi();  // also drops both panes' turn highlight
    }
    setAlignStatus(QString());
}

void SatelliteScreen::refreshCorrespondenceMarkers() {
    QVector<QPointF> sat_points;
    QVector<QPointF> pcd_points;
    QVector<int> numbers;
    for (int i = 0; i < correspondences_.size(); ++i) {
        sat_points.append(correspondences_[i].sat_px);
        pcd_points.append(worldToPcdImage(pcd_bounds_m_, pcd_image_.size(),
                                          correspondences_[i].pcd_m));
        numbers.append(i + 1);
    }
    sat_pick_->setMarkers(sat_points, numbers);
    pcd_pick_->setMarkers(pcd_points, numbers);
    sat_pick_->setPendingMarker(pending_sat_px_, have_pending_sat_);
    // A solved fit draws the robot's origin on the satellite pane — the
    // in-place check that the frame replaces the old review page with.
    if (pcd_to_sat_.valid) {
        const QPointF origin_px = pcd_to_sat_.apply(QPointF(0.0, 0.0));
        const QPointF dir = pcd_to_sat_.apply(QPointF(1.0, 0.0)) - origin_px;
        sat_pick_->setRobotPose(origin_px, std::atan2(dir.y(), dir.x()), true);
    } else {
        sat_pick_->setRobotPose(QPointF(), 0.0, false);
    }
}

void SatelliteScreen::updateCorrespondenceUi() {
    const bool have_pcd = !pcd_image_.isNull();
    const bool can_pick = have_pcd && !sat_image_.isNull();
    const bool aligned = pcd_to_sat_.valid;
    sat_pick_->setImage(sat_image_);
    pcd_pick_->setImage(pcd_image_);
    pcd_pane_stack_->setCurrentWidget(have_pcd ? pcd_pick_host_ : pcd_empty_);

    // Strict alternation: satellite first, then the matching map point. The
    // inactive pane is dimmed and refuses clicks so a pair can never be
    // half-formed on the wrong side. Before a point cloud exists neither
    // pane picks — the satellite pane is just the site for orientation.
    // Once aligned both panes stay pickable: an extra pair re-solves.
    sat_pick_->setPickEnabled(can_pick && !have_pending_sat_);
    pcd_pick_->setPickEnabled(can_pick && have_pending_sat_);
    // Turn highlight: the pane to click gets the legend-colour ring + a
    // "YOUR TURN" chip, the other pane dims hard. Off once aligned (both
    // panes stay pickable for a refining pair, no ring needed).
    {
        using Turn = PanZoomImageWidget::Turn;
        const int next = correspondences_.size() + 1;
        const bool show_turn = can_pick && !aligned;
        const QColor sat_accent(0x2b, 0x7f, 0xff);
        const QColor pcd_accent(0xfe, 0x9a, 0x00);
        sat_pick_->setTurn(
            !show_turn ? Turn::None
            : have_pending_sat_ ? Turn::Waiting
                                : Turn::Active,
            sat_accent,
            have_pending_sat_
                ? QStringLiteral("Point %1 picked — now click the point cloud")
                      .arg(next)
                : QStringLiteral("YOUR TURN — click point %1 here").arg(next));
        pcd_pick_->setTurn(
            !show_turn ? Turn::None
            : have_pending_sat_ ? Turn::Active
                                : Turn::Waiting,
            pcd_accent,
            have_pending_sat_
                ? QStringLiteral("YOUR TURN — click the same feature (%1)")
                      .arg(next)
                : QStringLiteral("Waiting — pick point %1 on the satellite first")
                      .arg(next));
    }
    sat_pick_->setCornerTag(can_pick ? QStringLiteral(
                                           "SATELLITE MAP — click to add "
                                           "correspondences")
                                     : QStringLiteral("SATELLITE MAP"));
    pcd_pick_->setCornerTag(can_pick ? QStringLiteral(
                                           "3D POINT CLOUD — click to add "
                                           "correspondences")
                                     : QStringLiteral("3D POINT CLOUD"));
    // The turn chips beside the pane tags carry the prompt; no bottom pill.
    sat_pick_->setStatusText(QString());
    pcd_pick_->setStatusText(QString());
    refreshCorrespondenceMarkers();

    const int required = minCorrespondences();
    const int pairs = correspondences_.size();
    const int sat_count = pairs + (have_pending_sat_ ? 1 : 0);
    const QString muted = QStringLiteral("#71717b");
    const QString text = QStringLiteral("#e4e4e7");

    // Instruction (235:2385): prompt in light grey, requirement muted.
    corr_instruction_->setText(
        have_pcd
            ? QStringLiteral(
                  "<span style='color:%1'>Click matching features on both "
                  "views to create correspondence pairs</span> "
                  "<span style='color:%2'>(min %3 pairs required%4)</span>")
                  .arg(text, muted)
                  .arg(required)
                  .arg(capture_gps_.valid ? QString()
                                          : QStringLiteral(" — no GPS seed"))
            : QStringLiteral(
                  "<span style='color:%1'>Capture a point cloud from the "
                  "robot</span> <span style='color:%2'>— then click matching "
                  "features on both views to align it</span>")
                  .arg(text, muted));
    corr_sat_count_->setText(QStringLiteral("Satellite (%1)").arg(sat_count));
    corr_pcd_count_->setText(QStringLiteral("Point Cloud (%1)").arg(pairs));
    // Pair chip (235:2402): mono "n/N pairs", green tint once satisfied.
    corr_pairs_chip_->setText(
        QStringLiteral("%1/%2 pairs").arg(pairs).arg(required));
    corr_pairs_chip_->setProperty("satisfied", pairs >= required);
    corr_pairs_chip_->style()->unpolish(corr_pairs_chip_);
    corr_pairs_chip_->style()->polish(corr_pairs_chip_);

    // Footer actions (shared bar): Align (N pairs), Clear pairs.
    if (align_button_) {
        align_button_->setText(QStringLiteral("Align (%1 pair%2)")
                                   .arg(pairs)
                                   .arg(pairs == 1 ? QString()
                                                   : QStringLiteral("s")));
        align_button_->setEnabled(can_pick && pairs >= required && !aligned);
        align_button_->setToolTip(
            aligned ? QStringLiteral("Aligned — add or clear pairs to re-solve")
            : pairs >= required
                ? QString()
                : QStringLiteral("Pick at least %1 pairs").arg(required));
        clear_pairs_button_->setEnabled(pairs > 0 || have_pending_sat_);
    }

    // Success card (235:4011) over the point cloud.
    if (align_success_card_) {
        align_success_card_->setVisible(aligned);
        if (aligned) {
            align_success_rmse_->setText(
                QStringLiteral("RMSE: %1")
                    .arg(units::formatLength(align_rmse_m_, 3)));
        }
    }
    // The step gate and the footer's Align visibility both read pcd_image_ /
    // pcd_to_sat_, which only change on the transitions that end up here.
    refreshStepUi();
}

void SatelliteScreen::setAlignStatus(const QString& status) {
    const bool busy = map_capture_ && map_capture_->busy();
    if (align_status_) {
        align_status_->setText(status);
    }
    if (collect_map_button_) {
        collect_map_button_->setText(
            busy ? QStringLiteral("Cancel Collection")
                 : QStringLiteral("Collect Map from Robot"));
    }
    // Point-cloud pane empty state: the title carries progress while a
    // capture runs, the CTA flips to Cancel, and the hint changes from
    // "what will happen" to "what to keep clear".
    if (pcd_empty_title_) {
        pcd_empty_title_->setText(
            busy || !status.isEmpty()
                ? status
                : QStringLiteral("Point cloud not yet captured"));
    }
    if (capture_button_) {
        capture_button_->setText(busy ? QStringLiteral("Cancel Capture")
                                      : QStringLiteral("Capture Point Cloud"));
        capture_button_->setEnabled(!mission_->missionActive());
    }
    if (pcd_empty_hint_) {
        pcd_empty_hint_->setText(
            busy ? QStringLiteral(
                       "Keep the area around the robot clear until it stops")
                 : QStringLiteral(
                       "Robot will rotate 360° to scan surroundings"));
    }
}

void SatelliteScreen::onAlignClicked() {
    if (correspondences_.size() < minCorrespondences()) {
        return;
    }
    QVector<QPointF> pcd;
    QVector<QPointF> sat;
    pcd.reserve(correspondences_.size());
    sat.reserve(correspondences_.size());
    for (const Correspondence& c : correspondences_) {
        pcd.append(c.pcd_m);
        sat.append(c.sat_px);
    }
    const auto fit = estimateSimilarity2D(pcd, sat);
    if (!fit || !fit->transform.valid) {
        const QString why = QStringLiteral(
            "Could not estimate a 2D transform. Spread the points around "
            "the site and try again.");
        appendLog(QStringLiteral("[align] %1").arg(why));
        corr_instruction_->setText(
            QStringLiteral("<span style='color:#FF6467'>%1</span>").arg(why));
        return;
    }
    pcd_to_sat_ = fit->transform;
    align_rmse_m_ = fit->rmse_m;
    appendLog(QStringLiteral("[align] fit from %1 pairs (%2), RMSE %3, "
                             "%4 px/m")
                  .arg(correspondences_.size())
                  .arg(pcd_to_sat_.reflected
                           ? QStringLiteral("scale, rotation, reflection")
                           : QStringLiteral("scale, rotation"))
                  .arg(units::formatLength(align_rmse_m_, 3))
                  .arg(pcd_to_sat_.scalePxPerM(), 0, 'f', 2));
    applyAlignmentAnchor();
    updateCorrespondenceUi();
}

void SatelliteScreen::applyAlignmentAnchor() {
    if (!pcd_to_sat_.valid || sat_image_.isNull()) {
        return;
    }
    const QPointF origin_px = pcd_to_sat_.apply(QPointF(0.0, 0.0));
    const QPointF x_axis_px = pcd_to_sat_.apply(QPointF(1.0, 0.0));
    const geo::GeoPoint origin = TileService::geoFromStitchPixel(
        site_manifest_, sat_image_.size(), origin_px);
    const geo::GeoPoint ahead = TileService::geoFromStitchPixel(
        site_manifest_, sat_image_.size(), x_axis_px);

    geo::GeoPose marker;
    marker.lat = origin.lat;
    marker.lon = origin.lon;
    const QPointF enu = geo::enuFromGeo(origin, ahead);
    marker.heading_deg =
        std::fmod(std::atan2(enu.x(), enu.y()) / geo::kDegToRad + 360.0, 360.0);
    marker.valid = true;
    map_->setMarker(marker);
    emit map_->markerChanged();
    map_->setView(origin.lat, origin.lon, map_->zoom());

    if (!current_job_id_.isEmpty()) {
        for (Job& job : jobs_) {
            if (job.id == current_job_id_) {
                job.robot = marker;
                job.alignment = pcd_to_sat_;
                job.align_rmse_m = align_rmse_m_;
                job_store_.save(job);
                break;
            }
        }
    }
    appendLog(QStringLiteral(
                  "[align] robot anchored at %1, %2 heading %3° (RMSE %4)")
                  .arg(origin.lat, 0, 'f', 7)
                  .arg(origin.lon, 0, 'f', 7)
                  .arg(marker.heading_deg, 0, 'f', 1)
                  .arg(units::formatLength(align_rmse_m_, 3)));
    alignment_confirmed_ = true;
    updateAlignCardUi();
}

void SatelliteScreen::updateAlignCardUi() {
    // Step 2's gate is pcd_image_ / pcd_to_sat_, both of which change on the
    // same transitions this function is called for, so the footer follows
    // from here rather than needing a call at every one of those sites.
    if (next_button_) {
        refreshStepUi();
    }
    const bool measured = plan_mode_ == PlanMode::Measured;
    // Card visibility is the step model's job now (the card belongs to step
    // 2, which is itself unavailable in the office planning trim). Setting it
    // here as well would let a mid-alignment refresh put the card back on a
    // step that is not showing it.
    applyStepVisibility();
    const bool busy = map_capture_ && map_capture_->busy();
    collect_map_button_->setEnabled(!mission_->missionActive());
    // The rail's "Pick Correspondences" is redundant now that step 2 opens
    // straight onto the picker; it only survives for the measured card,
    // where it is hidden anyway.
    correspond_button_->setVisible(false);
    if (busy) {
        return;  // the runner's progress messages own the label
    }
    if (measured) {
        setAlignStatus(
            pcd_image_.isNull()
                ? QStringLiteral(
                      "Collect a robot map to place the robot at the grid "
                      "origin, then draw the ROI around it.")
                : QStringLiteral(
                      "Robot at grid origin%1 — draw the ROI around it.")
                      .arg(capture_gps_.valid
                               ? QStringLiteral(" (GPS %1, %2)")
                                     .arg(capture_gps_.lat, 0, 'f', 6)
                                     .arg(capture_gps_.lon, 0, 'f', 6)
                               : QString()));
        return;
    }
    // Satellite: the picker's instruction bar and pane states carry the
    // story; the empty-state title only needs to speak up for an error,
    // which setAlignStatus is called with directly at the failure site.
    setAlignStatus(QString());
    if (correspond_page_ && canvas_stack_->currentWidget() == correspond_page_) {
        updateCorrespondenceUi();
    }
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
    // The plan is NOT stamped here. Launching is not collecting: the plan
    // moves to COMPLETED only when finalize succeeds
    // (markCurrentPlanCompleted), so an aborted mission leaves it PLANNED.
}

bool SatelliteScreen::isRobotLinkUnreachable() const {
    return link_monitor_ && link_monitor_->isArmed() &&
           link_monitor_->state() == LinkHealthMonitor::State::Disconnected;
}

void SatelliteScreen::onCompleteMission() {
    if (complete_mission_in_flight_) {
        return;
    }
    if (!confirmDialog(
            QStringLiteral("Complete Mission"),
            QStringLiteral(
                "Autonomy will be disabled, the motors disarmed, the mission "
                "finalized on the robot, and both launch trees stopped."),
            QStringLiteral("Complete Mission"))) {
        return;
    }
    complete_mission_in_flight_ = true;
    setAutonomyEnabled(false);

    // Data-first branch. On a truly dead link the disarm wait and the
    // finalize RPC would burn ~10 s on dead calls and still leave
    // mission_finalized_at null — the data IS on disk, the operator just
    // needs a way to land the metadata. Strict check: Reconnecting is a few
    // seconds of patience, not a reason to pop a modal.
    if (isRobotLinkUnreachable()) {
        appendLog(QStringLiteral(
            "[mission] link offline — offering offline finalize"));
        OfflineFinalizeDialog dialog(link_monitor_, this);
        dialog.move(mapToGlobal(rect().center()) - dialog.rect().center());
        dialog.exec();
        switch (dialog.chosen()) {
            case OfflineFinalizeDialog::Choice::Cancelled:
                // The robot's 10-minute idle watchdog is the safety net for an
                // abandoned mission, so staying put is a legitimate choice.
                appendLog(QStringLiteral(
                    "[mission] complete cancelled — robot watchdog will "
                    "finalize if the mission is abandoned"));
                complete_mission_in_flight_ = false;
                return;
            case OfflineFinalizeDialog::Choice::WaitReconnected:
                appendLog(QStringLiteral(
                    "[mission] link recovered while waiting — normal path"));
                executeCompleteMissionNormalPath();
                return;
            case OfflineFinalizeDialog::Choice::FinalizeOverSsh:
                executeCompleteMissionSshFallback();
                return;
        }
        complete_mission_in_flight_ = false;
        return;
    }
    executeCompleteMissionNormalPath();
}

void SatelliteScreen::beginMotorsIdleWait(
    std::function<void(bool timed_out)> on_done) {
    ros_->requestAxisState(RosLink::kAxisIdle);
    if (!motors_idle_timer_) {
        motors_idle_timer_ = new QTimer(this);
        motors_idle_timer_->setInterval(100);
    }
    motors_idle_ticks_ = 0;
    disconnect(motors_idle_timer_, &QTimer::timeout, nullptr, nullptr);
    connect(motors_idle_timer_, &QTimer::timeout, this, [this, on_done] {
        ++motors_idle_ticks_;
        if (ros_->motorsIdle()) {
            motors_idle_timer_->stop();
            on_done(false);
            return;
        }
        // 100 ms x 60 = 6 s ceiling. Killing the launch tree does not by
        // itself disarm the axes, so it is worth waiting — but never at the
        // cost of stranding the operator on this screen.
        if (motors_idle_ticks_ >= 60) {
            motors_idle_timer_->stop();
            on_done(true);
        }
    });
    motors_idle_timer_->start();
}

void SatelliteScreen::executeCompleteMissionNormalPath() {
    appendLog(QStringLiteral("[mission] disarming motors…"));
    beginMotorsIdleWait([this](bool timed_out) {
        appendLog(timed_out
                      ? QStringLiteral("[mission] motors did not confirm IDLE "
                                       "within 6 s — continuing")
                      : QStringLiteral("[mission] motors IDLE"));
        ros_->finalizeMission([this](bool ok, const QString& detail) {
            appendLog(ok ? QStringLiteral("[mission] finalized: %1").arg(detail)
                         : QStringLiteral(
                               "[mission] finalize failed (%1) — the robot's "
                               "idle watchdog will finalize instead")
                               .arg(detail));
            if (ok) {
                // The coordinator closed and tagged the mission folder —
                // that is the OCU's "data collected" signal.
                markCurrentPlanCompleted();
            }
            mission_->teardownMission();
            complete_mission_in_flight_ = false;
        });
    });
}

void SatelliteScreen::executeCompleteMissionSshFallback() {
    QString error;
    const RobotTarget target = MissionController::resolveRobotTarget(&error);
    if (target.valid) {
        // Non-interactive SSH does not source the ROS env, so `ros2` is not on
        // PATH. finalize_mission_local.py is pure file I/O with no rclpy
        // import, so invoking python3 directly is both correct and sufficient.
        const QString script =
            QStringLiteral("/home/%1/pilot_ws/install/pilot_control/lib/"
                           "pilot_control/finalize_mission_local.py")
                .arg(target.ssh_user);
        QProcess proc;
        QStringList args;
        args << "-o" << "ConnectTimeout=4"
             << "-o" << "StrictHostKeyChecking=no"
             << "-o" << "UserKnownHostsFile=/dev/null"
             << "-o" << "BatchMode=yes"
             << QStringLiteral("%1@%2").arg(target.ssh_user, target.host)
             << QStringLiteral("python3 %1").arg(script);
        proc.start(QStringLiteral("ssh"), args);
        if (!proc.waitForFinished(8000)) {
            proc.kill();
            proc.waitForFinished(500);
            appendLog(QStringLiteral(
                "[mission] offline finalize timed out (>8 s)"));
        } else {
            appendLog(QStringLiteral("[mission] offline finalize rc=%1 %2")
                          .arg(proc.exitCode())
                          .arg(QString::fromUtf8(
                                   proc.readAllStandardOutput().trimmed())));
            if (proc.exitStatus() == QProcess::NormalExit &&
                proc.exitCode() == 0) {
                // rc=0 means finalize_mission_local.py wrote
                // mission_finalized_at — the same guarantee as the RPC.
                markCurrentPlanCompleted();
            }
        }
    } else {
        appendLog(
            QStringLiteral("[mission] no robot host resolved (%1) — skipping "
                           "remote finalize")
                .arg(error));
    }
    // Skip the disarm wait and the finalize RPC; both would just time out on
    // a dead link. Killing the launch tree disarms via the controller's exit
    // handlers.
    mission_->teardownMission();
    complete_mission_in_flight_ = false;
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
    // Step-1 search: arrow keys walk the suggestions, Esc closes them, and
    // leaving the field closes them too (clicks on the popup itself never
    // take focus — NoFocus policy — so this cannot swallow a pick).
    if (watched == search_edit_) {
        if (event->type() == QEvent::FocusOut) {
            hideSuggestions();
        } else if (event->type() == QEvent::KeyPress &&
                   search_popup_->isVisible()) {
            auto* key = static_cast<QKeyEvent*>(event);
            const int n = search_popup_->count();
            if (key->key() == Qt::Key_Down) {
                search_popup_->setCurrentRow((search_popup_->currentRow() + 1) % n);
                return true;
            }
            if (key->key() == Qt::Key_Up) {
                const int row = search_popup_->currentRow();
                search_popup_->setCurrentRow(row <= 0 ? n - 1 : row - 1);
                return true;
            }
            if (key->key() == Qt::Key_Escape) {
                hideSuggestions();
                return true;
            }
        }
        return QWidget::eventFilter(watched, event);
    }
    // Step-3 edge rows: hover lights the matching canvas chip green.
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
        const QVariant idx = watched->property("edgeIndex");
        if (idx.isValid()) {
            map_->setHighlightedEdge(event->type() == QEvent::Enter ? idx.toInt()
                                                                    : -1);
            return false;
        }
    }
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

void SatelliteScreen::updateMotorsChip() {
    // Real controller_status wins over the axis-state RPC's optimistic ack:
    // the request being accepted is not the same as the axes having moved.
    const MotorStatus motors = ros_->motorStatus();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool fresh =
        motors.left_wall_ms > 0 && motors.right_wall_ms > 0 &&
        now - motors.left_wall_ms <= RosLink::kControllerStatusStaleMs &&
        now - motors.right_wall_ms <= RosLink::kControllerStatusStaleMs;
    if (!fresh) {
        setMotorsChip(QStringLiteral("MOTORS —"),
                      QColor(mutedColor(dark_mode_)));
        return;
    }
    if (motors.left_axis_state == RosLink::kAxisClosedLoop &&
        motors.right_axis_state == RosLink::kAxisClosedLoop) {
        setMotorsChip(QStringLiteral("MOTORS ARMED"), QColor(kAccent));
    } else if (motors.left_axis_state == RosLink::kAxisIdle &&
               motors.right_axis_state == RosLink::kAxisIdle) {
        setMotorsChip(QStringLiteral("MOTORS DISARMED"),
                      QColor(mutedColor(dark_mode_)));
    } else {
        // Split state (one axis armed, one not) is worth surfacing rather
        // than rounding to either extreme.
        setMotorsChip(QStringLiteral("MOTORS %1/%2")
                          .arg(motors.left_axis_state)
                          .arg(motors.right_axis_state),
                      QColor(kWarnAmber));
    }
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
