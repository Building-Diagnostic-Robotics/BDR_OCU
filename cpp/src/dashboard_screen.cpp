#include "dashboard_screen.hpp"
#include "robot_registry.hpp"
#include "settings_constants.hpp"
#include "update/update_log.hpp"
#include "version_info.hpp"

#include <QApplication>
#include <QColor>
#include <QDateTime>
#include <QFile>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include "components/tilt_calibration_dialog.hpp"
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QShowEvent>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <QVariant>
#include <QVBoxLayout>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QSvgRenderer>
#else
#include <QtSvg/QSvgRenderer>
#endif

namespace f2c_cpp {

namespace {

// Battery thresholds and timers — ported from the legacy CoverageGUI MQTT
// monitor. Keeping the same numbers means operators see consistent state
// labels across the legacy planner and the Stage 3 dashboard.
constexpr int kBatteryReconnectMs = 5000;
constexpr int kBatteryStaleTimerMs = 2000;
constexpr double kBatteryLowPct = 25.0;
constexpr double kBatteryCriticalPct = 12.0;

// Calibration probe cadence. SSH round-trips are cheap but not free; once
// every 5 minutes is plenty for a value that only changes when the
// operator runs `Calibrate Tilt`.
constexpr int kCalibrationRefreshMs = 5 * 60 * 1000;
constexpr int kCalibrationProbeTimeoutMs = 8000;

// Tilt-calibration policy: blink the calibration card and show "Due now"
// once `kCalibrationDueAfterScans` scans have happened since the last
// calibration. Soft reminder, not a hard requirement. Three matches the
// current field-deployment guidance — bump in one place if it changes.
constexpr int kCalibrationDueAfterScans = 3;


QPixmap loadSvgPixmap(const QString& resourcePath, int w, int h,
                      const QString& strokeColor = QString()) {
    QFile f(resourcePath);
    if (!f.open(QIODevice::ReadOnly)) {
        return QPixmap();
    }
    QByteArray data = f.readAll();
    f.close();

    if (!strokeColor.isEmpty()) {
        int strokeStart = data.indexOf("stroke=\"");
        if (strokeStart >= 0) {
            int valueStart = strokeStart + 8;
            int valueEnd = data.indexOf('"', valueStart);
            if (valueEnd > valueStart) {
                QByteArray newColor = strokeColor.toUtf8();
                if (!newColor.startsWith('#')) {
                    newColor.prepend('#');
                }
                data = data.left(valueStart) + newColor + data.mid(valueEnd);
            }
        }
    }

    QSvgRenderer renderer(data);
    if (!renderer.isValid()) {
        return QPixmap();
    }
    QPixmap pix(w, h);
    pix.fill(Qt::transparent);
    QPainter painter(&pix);
    renderer.render(&painter);
    return pix;
}

void applyDropShadow(QWidget* widget, int blurRadius, int yOffset, int alpha) {
    if (!widget) {
        return;
    }
    auto* effect = new QGraphicsDropShadowEffect(widget);
    effect->setBlurRadius(blurRadius);
    effect->setOffset(0, yOffset);
    effect->setColor(QColor(0, 0, 0, alpha));
    widget->setGraphicsEffect(effect);
}

QWidget* makeStatusCard(QWidget* parent,
                       const QString& objectName,
                       const QString& iconBgColor,
                       const QString& iconResourcePath,
                       const QString& iconColor,
                       const QString& labelText,
                       QLabel*& outValue) {
    auto* card = new QWidget(parent);
    card->setObjectName(objectName);
    card->setFixedHeight(94);
    auto* cardLayout = new QHBoxLayout(card);
    cardLayout->setContentsMargins(24, 24, 24, 14);
    cardLayout->setSpacing(24);

    auto* iconBox = new QLabel(card);
    iconBox->setObjectName(objectName + "Icon");
    iconBox->setAlignment(Qt::AlignCenter);
    iconBox->setFixedSize(56, 56);
    iconBox->setStyleSheet(QString("background: %1; border-radius: 10px;").arg(iconBgColor));
    QPixmap iconPix = loadSvgPixmap(iconResourcePath, 32, 32, iconColor);
    if (!iconPix.isNull()) {
        iconBox->setPixmap(iconPix);
    }
    cardLayout->addWidget(iconBox, 0, Qt::AlignTop);

    auto* textCol = new QWidget(card);
    auto* textLayout = new QVBoxLayout(textCol);
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(4);
    textLayout->setAlignment(Qt::AlignTop);

    auto* label = new QLabel(labelText, textCol);
    label->setObjectName(objectName + "Label");
    label->setStyleSheet("font-size: 14px; line-height: 20px; color: #4A5565;");
    textLayout->addWidget(label);

    outValue = new QLabel(textCol);
    outValue->setObjectName(objectName + "Value");
    textLayout->addWidget(outValue);

    cardLayout->addWidget(textCol, 1, Qt::AlignTop);
    return card;
}

QPushButton* makeActionButton(QWidget* parent,
                              const QString& objectName,
                              const QString& borderColor,
                              const QString& titleColor,
                              const QString& iconResourcePath,
                              const QString& title,
                              const QString& description) {
    auto* btn = new QPushButton(parent);
    btn->setObjectName(objectName);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setFlat(true);  // so native style doesn't paint over icon/title/desc
    btn->setFixedHeight(168);
    const QString selector = QString("#%1").arg(objectName);
    btn->setStyleSheet(QString(
        "%1 {"
        "  background: transparent;"
        "  border: 2px solid %2;"
        "  border-radius: 10px;"
        "  text-align: left;"
        "}"
        "%1:hover:enabled { background: rgba(0,0,0,0.03); }"
        "%1:focus { outline: none; }"
        ).arg(selector, borderColor));
    auto* layout = new QVBoxLayout(btn);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);
    layout->setAlignment(Qt::AlignCenter);

    auto* icon = new QLabel(btn);
    icon->setObjectName(objectName + "Icon");
    icon->setAlignment(Qt::AlignCenter);
    icon->setFixedSize(40, 40);
    icon->setScaledContents(true);
    QPixmap iconPix = loadSvgPixmap(iconResourcePath, 40, 40, titleColor);
    if (!iconPix.isNull()) {
        icon->setPixmap(iconPix);
    }
    layout->addWidget(icon, 0, Qt::AlignCenter);

    auto* titleLbl = new QLabel(title, btn);
    titleLbl->setObjectName(objectName + "Title");
    titleLbl->setStyleSheet(QString(
        "font-family: 'Arimo'; font-weight: 700; font-size: 18px; line-height: 28px; color: %1;")
        .arg(titleColor));
    titleLbl->setAlignment(Qt::AlignCenter);
    layout->addWidget(titleLbl, 0, Qt::AlignCenter);

    auto* descLbl = new QLabel(description, btn);
    descLbl->setObjectName(objectName + "Desc");
    descLbl->setStyleSheet(
        "font-family: 'Arimo'; font-size: 14px; line-height: 20px; color: #4A5565;");
    descLbl->setAlignment(Qt::AlignCenter);
    descLbl->setWordWrap(true);
    layout->addWidget(descLbl, 0, Qt::AlignCenter);

    return btn;
}

}  // namespace

DashboardScreen::DashboardScreen(QWidget* parent)
    : QWidget(parent) {
    setObjectName("DashboardRoot");
    setAttribute(Qt::WA_StyledBackground, true);
    setAutoFillBackground(true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addSpacing(45);

    // ── Header ─────────────────────────────────────────────────────────────
    header_ = new QWidget(this);
    header_->setObjectName("DashboardHeader");
    header_->setFixedHeight(127);
    auto* headerLayout = new QVBoxLayout(header_);
    headerLayout->setContentsMargins(40, 24, 40, 24);
    headerLayout->setSpacing(0);

    auto* headerRow = new QHBoxLayout();
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(0);

    auto* titleCol = new QVBoxLayout();
    titleCol->setContentsMargins(0, 0, 0, 0);
    titleCol->setSpacing(4);
    lbl_title_ = new QLabel("Roofus Dashboard", header_);
    lbl_title_->setObjectName("DashboardTitle");
    titleCol->addWidget(lbl_title_);
    lbl_subtitle_ = new QLabel("Coverage Planning Suite", header_);
    lbl_subtitle_->setObjectName("DashboardSubtitle");
    titleCol->addWidget(lbl_subtitle_);
    headerRow->addLayout(titleCol, 1);
    headerRow->addStretch(1);

    btn_logout_ = new QPushButton(header_);
    btn_logout_->setObjectName("DashboardLogoutButton");
    btn_logout_->setFixedHeight(40);
    btn_logout_->setCursor(Qt::PointingHandCursor);
    QPixmap logoutPix = loadSvgPixmap(":/assets/dashboard/logout.svg", 20, 20);
    if (!logoutPix.isNull()) {
        btn_logout_->setIcon(QIcon(logoutPix));
        btn_logout_->setIconSize(QSize(20, 20));
    }
    btn_logout_->setText(" Logout");
    connect(btn_logout_, &QPushButton::clicked, this, &DashboardScreen::onLogoutClicked);
    headerRow->addWidget(btn_logout_, 0, Qt::AlignRight | Qt::AlignVCenter);

    headerLayout->addLayout(headerRow);
    root->addWidget(header_);

    // ── Content ────────────────────────────────────────────────────────────
    auto* content = new QWidget(this);
    content->setObjectName("DashboardContent");
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(32, 37, 32, 32);
    contentLayout->setSpacing(47);

    // Status cards row
    auto* cardsRow = new QHBoxLayout();
    cardsRow->setContentsMargins(0, 0, 0, 0);
    cardsRow->setSpacing(24);

    card_status_ = makeStatusCard(content, "DashboardCardStatus",
        "#D0FAE5", ":/assets/dashboard/heartbeat.svg", "#009966", "System Status", lbl_status_value_);
    lbl_status_value_->setText("READY");
    lbl_status_value_->setStyleSheet(
        "font-family: 'Arimo'; font-weight: 700; font-size: 24px; line-height: 32px; color: #009966;");
    cardsRow->addWidget(card_status_, 1);

    card_scans_ = makeStatusCard(content, "DashboardCardScans",
        "#DBEAFE", ":/assets/dashboard/location_pin.svg", "#155DFC", "Total Scans", lbl_scans_value_);
    lbl_scans_value_->setText(QStringLiteral("—"));
    lbl_scans_value_->setStyleSheet(
        "font-family: 'Arimo'; font-weight: 700; font-size: 24px; line-height: 32px; color: #1E2939;");
    cardsRow->addWidget(card_scans_, 1);

    card_battery_top_ = makeStatusCard(content, "DashboardCardBatteryTop",
        "#F3E8FF", ":/assets/dashboard/battery.svg", "#9810FA", "Battery", lbl_battery_card_value_);
    lbl_battery_card_value_->setText("—");
    lbl_battery_card_value_->setStyleSheet(
        "font-family: 'Arimo'; font-weight: 700; font-size: 24px; line-height: 32px; color: #9810FA;");
    cardsRow->addWidget(card_battery_top_, 1);

    card_calibration_ = makeStatusCard(content, "DashboardCardCalibration",
        "#FEF3C6", ":/assets/dashboard/settings.svg", "#E17100", "Next Calibration", lbl_calibration_value_);
    lbl_calibration_value_->setText(QStringLiteral("—"));
    lbl_calibration_value_->setStyleSheet(
        "font-family: 'Arimo'; font-weight: 700; font-size: 24px; line-height: 32px; color: #E17100;");
    cardsRow->addWidget(card_calibration_, 1);
    // Click anywhere on the calibration card → trigger the Calibrate Tilt
    // flow. Blink + clickable surface together make the "due now" state
    // discoverable without forcing the operator to hunt for the button.
    card_calibration_->setCursor(Qt::PointingHandCursor);
    card_calibration_->setToolTip(QStringLiteral("Click to run Calibrate Tilt"));
    card_calibration_->installEventFilter(this);

    contentLayout->addLayout(cardsRow);

    // Quick Actions
    auto* actionsCard = new QWidget(this);
    actionsCard->setObjectName("DashboardActionsCard");
    auto* actionsLayout = new QVBoxLayout(actionsCard);
    actionsLayout->setContentsMargins(32, 32, 32, 32);
    actionsLayout->setSpacing(24);

    auto* actionsTitle = new QLabel("Quick Actions", actionsCard);
    actionsTitle->setObjectName("DashboardActionsTitle");
    actionsTitle->setStyleSheet(
        "font-family: 'Arimo'; font-weight: 700; font-size: 24px; line-height: 32px; color: #1E2939;");
    actionsLayout->addWidget(actionsTitle);

    auto* actionsRow = new QHBoxLayout();
    actionsRow->setContentsMargins(0, 0, 0, 0);
    actionsRow->setSpacing(24);

    btn_start_scan_ = makeActionButton(actionsCard, "DashboardBtnStartScan", "#009966", "#009966",
        ":/assets/dashboard/location_pin.svg", "Start New Scan", "Begin a new coverage mapping session");
    connect(btn_start_scan_, &QPushButton::clicked, this, &DashboardScreen::onStartNewScanClicked);
    actionsRow->addWidget(btn_start_scan_, 1);

    btn_run_diagnostics_ = makeActionButton(actionsCard, "DashboardBtnDiagnostics", "#155DFC", "#155DFC",
        ":/assets/dashboard/heartbeat.svg", "Run Diagnostics", "Check system health and sensors");
    connect(btn_run_diagnostics_, &QPushButton::clicked, this, &DashboardScreen::onRunDiagnosticsClicked);
    actionsRow->addWidget(btn_run_diagnostics_, 1);

    btn_view_recordings_ = makeActionButton(actionsCard, "DashboardBtnRecordings", "#9810FA", "#9810FA",
        ":/assets/dashboard/camera.svg", "View Recordings", "Access previous scan data");
    connect(btn_view_recordings_, &QPushButton::clicked, this, &DashboardScreen::onViewRecordingsClicked);
    actionsRow->addWidget(btn_view_recordings_, 1);

    btn_calibrate_tilt_ = makeActionButton(actionsCard, "DashboardBtnCalibrateTilt", "#E17100", "#E17100",
        ":/assets/dashboard/settings.svg", "Calibrate Tilt", "Align LiDAR mount for odometry and maps");
    connect(btn_calibrate_tilt_, &QPushButton::clicked, this, &DashboardScreen::onCalibrateTiltRequested);
    actionsRow->addWidget(btn_calibrate_tilt_, 1);

    actionsLayout->addLayout(actionsRow);
    contentLayout->addWidget(actionsCard);

    // System Information
    auto* infoCard = new QWidget(this);
    infoCard->setObjectName("DashboardInfoCard");
    auto* infoLayout = new QVBoxLayout(infoCard);
    infoLayout->setContentsMargins(24, 24, 24, 24);
    infoLayout->setSpacing(8);

    auto* infoTitle = new QLabel("System Information", infoCard);
    infoTitle->setObjectName("DashboardInfoTitle");
    infoTitle->setStyleSheet(
        "font-family: 'Arimo'; font-weight: 700; font-size: 18px; line-height: 27px; color: #1C398E;");
    infoLayout->addWidget(infoTitle);

    auto* infoRow = new QHBoxLayout();
    infoRow->setContentsMargins(0, 0, 0, 0);
    infoRow->setSpacing(32);

    auto addInfoPair = [&infoRow](QWidget* parent, const QString& labelText,
                                  QLabel*& outValue) -> QWidget* {
        auto* block = new QWidget(parent);
        auto* v = new QVBoxLayout(block);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(4);
        auto* lbl = new QLabel(labelText, block);
        lbl->setStyleSheet(
            "font-family: 'Arimo'; font-weight: 700; font-size: 14px; line-height: 20px; color: #1447E6;");
        outValue = new QLabel(block);
        outValue->setStyleSheet(
            "font-family: 'Arimo'; font-size: 14px; line-height: 20px; color: #155DFC;");
        v->addWidget(lbl);
        v->addWidget(outValue);
        infoRow->addWidget(block);
        return block;
    };

    addInfoPair(infoCard, "Robot ID", lbl_robot_id_value_);
    addInfoPair(infoCard, "Firmware", lbl_firmware_value_);
    addInfoPair(infoCard, "Last Calibration", lbl_calibration_value_info_);
    addInfoPair(infoCard, "Battery", lbl_battery_value_);
    addInfoPair(infoCard, "Uptime", lbl_uptime_value_);

    lbl_robot_id_value_->setText("—");
    // Firmware = OCU build version. Single source of truth is the CMake
    // project(... VERSION X.Y.Z) line in cpp/CMakeLists.txt; PROJECT_VERSION
    // flows into APP_SEMVER -> kAppSemver via version_info.hpp.in. Bump that
    // one line to bump the displayed version everywhere.
    lbl_firmware_value_->setText(
        QStringLiteral("v%1").arg(QString::fromLatin1(version::kAppSemver)));
    lbl_calibration_value_info_->setText("—");
    lbl_battery_value_->setText("—");
    lbl_uptime_value_->setText("—");

    // Load battery topic / port overrides up-front so the values are stable
    // for the lifetime of the widget. Robot host comes from RobotRegistry
    // (same pattern as SetupScreen) so per-customer config flows through
    // robots.json instead of hardcoded IPs.
    {
        QSettings settings(kSettingsOrgName, kSettingsAppName);
        battery_topic_ = settings.value(QStringLiteral("battery_mqtt_topic"),
                                        QStringLiteral("pilot/battery/state"))
                             .toString()
                             .trimmed();
        if (battery_topic_.isEmpty()) {
            battery_topic_ = QStringLiteral("pilot/battery/state");
        }
        battery_port_ = settings.value(QStringLiteral("battery_mqtt_port"), 1883).toInt();
        if (battery_port_ <= 0 || battery_port_ > 65535) {
            battery_port_ = 1883;
        }
    }
    loadRobotProfileFromRegistry();

    battery_stale_timer_ = new QTimer(this);
    battery_stale_timer_->setInterval(kBatteryStaleTimerMs);
    connect(battery_stale_timer_, &QTimer::timeout, this,
            &DashboardScreen::onBatteryStaleTimerTick);

    calibration_refresh_timer_ = new QTimer(this);
    calibration_refresh_timer_->setInterval(kCalibrationRefreshMs);
    connect(calibration_refresh_timer_, &QTimer::timeout, this,
            &DashboardScreen::onCalibrationRefreshTimerTick);

    // System Status card refresh — 1 Hz is enough for a rollup that only
    // changes when MQTT freshness flips or preflight is re-run. Cheap.
    status_refresh_timer_ = new QTimer(this);
    status_refresh_timer_->setInterval(1000);
    connect(status_refresh_timer_, &QTimer::timeout, this,
            &DashboardScreen::onStatusCardRefreshTimerTick);

    refreshSystemStatusCard();

    infoLayout->addLayout(infoRow);
    contentLayout->addWidget(infoCard);

    contentLayout->addStretch(1);
    root->addWidget(content, 1);

    applyDropShadow(header_, 8, 4, 25);
    applyDropShadow(card_status_, 6, 2, 25);
    applyDropShadow(card_scans_, 6, 2, 25);
    applyDropShadow(card_battery_top_, 6, 2, 25);
    applyDropShadow(card_calibration_, 6, 2, 25);
    applyDropShadow(actionsCard, 6, 2, 25);

    applyStyle();
}

void DashboardScreen::setRobotId(const QString& robotId) {
    robot_id_ = robotId.trimmed();
    if (lbl_robot_id_value_) {
        lbl_robot_id_value_->setText(robot_id_.isEmpty() ? "—" : robot_id_.toUpper());
    }
}

void DashboardScreen::setDarkMode(bool dark_mode) {
    if (dark_mode_ == dark_mode) {
        return;
    }
    dark_mode_ = dark_mode;
    applyStyle();
}

void DashboardScreen::onLogoutClicked() {
    emit logoutRequested();
}

void DashboardScreen::onStartNewScanClicked() {
    emit startNewScanRequested();
}

void DashboardScreen::onRunDiagnosticsClicked() {
    emit runDiagnosticsRequested();
}

void DashboardScreen::onViewRecordingsClicked() {
    emit viewRecordingsRequested();
}

void DashboardScreen::onCalibrateTiltRequested() {
    TiltCalibrationDialog dlg(robotHostFromSettings(), this);
    dlg.exec();
}

QString DashboardScreen::robotHostFromSettings() const {
    if (!robot_host_.isEmpty()) {
        return robot_host_;
    }
    QSettings settings(kSettingsOrgName, kSettingsAppName);
    const QString from_settings = settings.value("robot_ip", "").toString().trimmed();
    return from_settings.isEmpty() ? QStringLiteral("192.168.168.101") : from_settings;
}

void DashboardScreen::loadRobotProfileFromRegistry() {
    RobotRegistry registry;
    QString reg_err;
    if (!registry.load(&reg_err)) {
        update::log::warn("dashboard",
                          QStringLiteral("registry load failed: %1").arg(reg_err));
        return;
    }
    const auto profiles = registry.robots();
    if (profiles.isEmpty()) {
        update::log::warn(
            "dashboard",
            QStringLiteral("registry has no robots: %1").arg(registry.sourcePath()));
        return;
    }
    robot_host_ = profiles.first().host.trimmed();
    robot_ssh_user_ = profiles.first().ssh_user.trimmed();
    update::log::info(
        "dashboard",
        QStringLiteral("registry resolved host=%1 ssh_user=%2 (from %3)")
            .arg(robot_host_)
            .arg(robot_ssh_user_)
            .arg(registry.sourcePath()));
}

DashboardScreen::~DashboardScreen() {
    stopBatteryMonitor();
    stopCalibrationProbe();
}

void DashboardScreen::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (dashboard_first_shown_ms_ == 0) {
        dashboard_first_shown_ms_ = QDateTime::currentMSecsSinceEpoch();
    }
    // Start the battery monitor on every show; the legacy CoverageGUI keeps
    // it running for the entire app lifetime, but Stage 3 is a single
    // screen in a stacked widget — kicking off only when visible avoids
    // spawning mosquitto_sub during Stage 1/2 where it can't help anyway.
    startBatteryMonitor();
    if (battery_stale_timer_) {
        battery_stale_timer_->start();
    }

    // Calibration: probe immediately, then refresh every 5 minutes.
    startCalibrationProbe();
    if (calibration_refresh_timer_) {
        calibration_refresh_timer_->start();
    }

    if (status_refresh_timer_) {
        status_refresh_timer_->start();
    }
    refreshSystemStatusCard();
    refreshUptimeDisplay();
}

void DashboardScreen::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (battery_stale_timer_) {
        battery_stale_timer_->stop();
    }
    stopBatteryMonitor();
    if (calibration_refresh_timer_) {
        calibration_refresh_timer_->stop();
    }
    stopCalibrationProbe();
    if (status_refresh_timer_) {
        status_refresh_timer_->stop();
    }
    // Pause the blink animation while Stage 3 isn't visible — saves a
    // tiny amount of CPU and avoids flicker when the operator comes
    // back to the screen (we'll restart the animation if the next
    // probe still says "due").
    if (calibration_blink_anim_) {
        calibration_blink_anim_->stop();
    }
}

// =============================================================================
// Battery monitor (mosquitto_sub via QProcess)
// =============================================================================

void DashboardScreen::startBatteryMonitor() {
    battery_last_start_attempt_ms_ = QDateTime::currentMSecsSinceEpoch();
    stopBatteryMonitor();

    battery_has_payload_ = false;
    battery_soc_pct_.reset();
    battery_voltage_v_.reset();
    battery_current_a_.reset();
    battery_warn_flag_ = false;
    battery_critical_flag_ = false;
    battery_payload_updated_at_ms_ = 0;
    battery_payload_stale_after_ms_ = 5000;
    battery_stdout_buf_.clear();

    if (robot_host_.isEmpty()) {
        setBatteryDisplay(QStringLiteral("—"),
                          QStringLiteral("No robot host configured (robots.json)."),
                          QStringLiteral("#666"));
        return;
    }

    const QString mosquitto_sub = QStandardPaths::findExecutable(QStringLiteral("mosquitto_sub"));
    if (mosquitto_sub.isEmpty()) {
        setBatteryDisplay(
            QStringLiteral("—"),
            QStringLiteral("Install `mosquitto-clients` on the laptop to read battery."),
            QStringLiteral("#a66f00"));
        return;
    }

    setBatteryDisplay(QStringLiteral("…"),
                      QStringLiteral("Connecting to battery telemetry on %1:%2 …")
                          .arg(robot_host_)
                          .arg(battery_port_),
                      QStringLiteral("#666"));

    battery_proc_ = new QProcess(this);
    battery_proc_->setProcessChannelMode(QProcess::SeparateChannels);

    connect(battery_proc_, &QProcess::readyReadStandardOutput, this,
            &DashboardScreen::onBatteryProcessReadyRead);
    connect(battery_proc_,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            &DashboardScreen::onBatteryProcessFinished);

    QStringList args;
    args << QStringLiteral("-h") << robot_host_
         << QStringLiteral("-p") << QString::number(battery_port_)
         << QStringLiteral("-q") << QStringLiteral("1")
         << QStringLiteral("-t") << battery_topic_;
    battery_proc_->start(mosquitto_sub, args);

    if (!battery_proc_->waitForStarted(1500)) {
        const QString err = battery_proc_->errorString();
        stopBatteryMonitor();
        setBatteryDisplay(
            QStringLiteral("—"),
            QStringLiteral("Battery telemetry: %1")
                .arg(err.isEmpty() ? QStringLiteral("failed to start") : err),
            QStringLiteral("#a66f00"));
    }
}

void DashboardScreen::stopBatteryMonitor() {
    battery_stdout_buf_.clear();
    if (!battery_proc_) {
        return;
    }
    battery_proc_->blockSignals(true);
    if (battery_proc_->state() != QProcess::NotRunning) {
        battery_proc_->terminate();
        if (!battery_proc_->waitForFinished(750)) {
            battery_proc_->kill();
            battery_proc_->waitForFinished(750);
        }
    }
    battery_proc_->deleteLater();
    battery_proc_ = nullptr;
}

void DashboardScreen::onBatteryProcessReadyRead() {
    if (!battery_proc_) {
        return;
    }
    battery_stdout_buf_.append(battery_proc_->readAllStandardOutput());
    while (true) {
        const int newline_idx = battery_stdout_buf_.indexOf('\n');
        if (newline_idx < 0) {
            break;
        }
        const QByteArray raw_line = battery_stdout_buf_.left(newline_idx);
        battery_stdout_buf_.remove(0, newline_idx + 1);
        const QString line = QString::fromUtf8(raw_line).trimmed();
        if (line.isEmpty()) {
            continue;
        }
        handleBatteryPayload(line);
    }
}

void DashboardScreen::onBatteryProcessFinished() {
    // Subscriber died — display a transient state and let the stale timer
    // respawn it after kBatteryReconnectMs.
    setBatteryDisplay(QStringLiteral("—"),
                      QStringLiteral("Battery subscriber stopped, retrying…"),
                      QStringLiteral("#a66f00"));
}

void DashboardScreen::handleBatteryPayload(const QString& jsonLine) {
    QJsonParseError parse_error;
    const QJsonDocument doc =
        QJsonDocument::fromJson(jsonLine.toUtf8(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        update::log::warn(
            "dashboard",
            QStringLiteral("battery: invalid JSON: %1").arg(jsonLine));
        return;
    }
    const QJsonObject obj = doc.object();
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    battery_payload_updated_at_ms_ = static_cast<qint64>(
        obj.value(QStringLiteral("updated_at_ms")).toDouble(static_cast<double>(now_ms)));
    battery_payload_stale_after_ms_ = static_cast<qint64>(
        obj.value(QStringLiteral("stale_after_ms"))
            .toDouble(static_cast<double>(battery_payload_stale_after_ms_)));
    if (obj.contains(QStringLiteral("soc_percent"))) {
        battery_soc_pct_ = obj.value(QStringLiteral("soc_percent")).toDouble();
    }
    if (obj.contains(QStringLiteral("voltage_v"))) {
        battery_voltage_v_ = obj.value(QStringLiteral("voltage_v")).toDouble();
    }
    if (obj.contains(QStringLiteral("current_a"))) {
        battery_current_a_ = obj.value(QStringLiteral("current_a")).toDouble();
    }
    battery_warn_flag_ = obj.contains(QStringLiteral("warn"))
        ? obj.value(QStringLiteral("warn")).toBool()
        : (battery_soc_pct_.has_value() && battery_soc_pct_.value() <= kBatteryLowPct);
    battery_critical_flag_ = obj.contains(QStringLiteral("critical"))
        ? obj.value(QStringLiteral("critical")).toBool()
        : (battery_soc_pct_.has_value() && battery_soc_pct_.value() <= kBatteryCriticalPct);
    battery_has_payload_ = true;
    refreshBatteryDisplay();
}

void DashboardScreen::onBatteryStaleTimerTick() {
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    if ((!battery_proc_ || battery_proc_->state() == QProcess::NotRunning) &&
        !robot_host_.isEmpty() &&
        (now_ms - battery_last_start_attempt_ms_) >= kBatteryReconnectMs) {
        startBatteryMonitor();
        return;
    }
    refreshBatteryDisplay();
}

void DashboardScreen::refreshBatteryDisplay() {
    if (!battery_has_payload_) {
        if (battery_proc_ && battery_proc_->state() != QProcess::NotRunning) {
            setBatteryDisplay(
                QStringLiteral("…"),
                QStringLiteral("Waiting for first battery payload on %1 …")
                    .arg(battery_topic_),
                QStringLiteral("#666"));
        }
        return;
    }
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    const bool stale = battery_payload_updated_at_ms_ <= 0 ||
                       (now_ms - battery_payload_updated_at_ms_) >
                           battery_payload_stale_after_ms_;

    QString state_label;
    QString color;
    if (stale) {
        state_label = QStringLiteral("STALE");
        color = QStringLiteral("#a66f00");
    } else if (battery_critical_flag_) {
        state_label = QStringLiteral("CRITICAL");
        color = QStringLiteral("red");
    } else if (battery_warn_flag_) {
        state_label = QStringLiteral("LOW");
        color = QStringLiteral("#a66f00");
    } else {
        state_label = QStringLiteral("OK");
        color = QStringLiteral("green");
    }

    const QString pct_text = battery_soc_pct_.has_value()
        ? QStringLiteral("%1%  %2")
              .arg(QString::number(battery_soc_pct_.value(), 'f', 0))
              .arg(state_label)
        : QStringLiteral("—  %1").arg(state_label);

    QStringList tooltip_lines;
    if (battery_voltage_v_.has_value()) {
        tooltip_lines << QStringLiteral("Voltage: %1 V")
                             .arg(QString::number(battery_voltage_v_.value(), 'f', 1));
    }
    if (battery_current_a_.has_value()) {
        tooltip_lines << QStringLiteral("Current: %1 A")
                             .arg(QString::number(battery_current_a_.value(), 'f', 2));
    }
    tooltip_lines << QStringLiteral("Topic: %1 @ %2:%3")
                         .arg(battery_topic_)
                         .arg(robot_host_)
                         .arg(battery_port_);
    if (stale) {
        tooltip_lines << QStringLiteral(
            "Payload is older than its stale_after_ms threshold.");
    }
    setBatteryDisplay(pct_text, tooltip_lines.join(QChar('\n')), color);
}

void DashboardScreen::setBatteryDisplay(const QString& valueText,
                                        const QString& tooltip,
                                        const QString& color) {
    if (lbl_battery_value_) {
        lbl_battery_value_->setText(valueText);
        lbl_battery_value_->setToolTip(tooltip);
        // Inline color override on top of the base info-pair stylesheet — the
        // base style picks #155DFC; battery state takes precedence.
        lbl_battery_value_->setStyleSheet(
            QStringLiteral(
                "font-family: 'Arimo'; font-size: 14px; line-height: 20px; "
                "color: %1; font-weight: 700;")
                .arg(color));
    }
    // Top Battery card mirrors only the percent portion of the
    // value text (everything before the double-space separator). The
    // card title already says "Battery" so the inline state label would
    // be redundant; color carries the state instead.
    if (lbl_battery_card_value_) {
        const int sep = valueText.indexOf(QStringLiteral("  "));
        const QString pct_only =
            (sep > 0) ? valueText.left(sep) : valueText;
        lbl_battery_card_value_->setText(pct_only);
        lbl_battery_card_value_->setToolTip(tooltip);
        // The top card uses a much larger font set in applyStyle()'s
        // `value_style` template, so preserve everything but override
        // the color when battery state is non-nominal. Nominal (green)
        // leaves the default light/dark theme color so the card
        // doesn't shout at the operator for healthy batteries.
        const bool nominal = (color == QStringLiteral("green"));
        if (!nominal) {
            const QString existing = lbl_battery_card_value_->styleSheet();
            // Replace `color: #...;` if present, otherwise append.
            QString patched = existing;
            const int color_idx = patched.indexOf(QStringLiteral("color:"));
            if (color_idx >= 0) {
                const int semicolon =
                    patched.indexOf(QChar(';'), color_idx);
                if (semicolon > color_idx) {
                    patched.replace(color_idx, semicolon - color_idx,
                                    QStringLiteral("color: %1").arg(color));
                }
            } else {
                patched += QStringLiteral(" color: %1;").arg(color);
            }
            lbl_battery_card_value_->setStyleSheet(patched);
        } else {
            // Reset to theme-driven color by re-running applyStyle on
            // next paint cycle; cheaper to just clear the inline color
            // override here.
            QString existing = lbl_battery_card_value_->styleSheet();
            const int color_idx = existing.indexOf(QStringLiteral("color:"));
            if (color_idx >= 0) {
                const int semicolon =
                    existing.indexOf(QChar(';'), color_idx);
                if (semicolon > color_idx) {
                    existing.remove(color_idx,
                                    semicolon - color_idx + 1);
                    lbl_battery_card_value_->setStyleSheet(existing.trimmed());
                }
            }
        }
    }
    // Status rollup may flip when battery state changes.
    refreshSystemStatusCard();
}

// =============================================================================
// System Status card
// =============================================================================

bool DashboardScreen::robotReachableViaMqttBattery() const {
    if (!battery_has_payload_ || battery_payload_updated_at_ms_ <= 0) {
        return false;
    }
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    return (now_ms - battery_payload_updated_at_ms_) <=
           battery_payload_stale_after_ms_;
}

void DashboardScreen::setPreflightResult(const QString& result) {
    const QString trimmed = result.trimmed().toUpper();
    if (preflight_status_ == trimmed) {
        return;
    }
    preflight_status_ = trimmed;
    refreshSystemStatusCard();
}

void DashboardScreen::onStatusCardRefreshTimerTick() {
    refreshSystemStatusCard();
    refreshUptimeDisplay();
}

void DashboardScreen::refreshUptimeDisplay() {
    if (!lbl_uptime_value_) {
        return;
    }
    auto* app = qApp;
    if (!app) {
        lbl_uptime_value_->setText(QStringLiteral("—"));
        lbl_uptime_value_->setToolTip({});
        return;
    }
    const QVariant v = app->property(kOcuStartEpochMsProperty);
    bool ok = false;
    const qint64 start_ms = v.toLongLong(&ok);
    if (!ok || start_ms <= 0) {
        lbl_uptime_value_->setText(QStringLiteral("—"));
        lbl_uptime_value_->setToolTip({});
        return;
    }
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    const qint64 elapsed_ms = qMax(qint64{0}, now_ms - start_ms);
    const qint64 total_sec = elapsed_ms / 1000;
    const qint64 days = total_sec / 86400;
    const qint64 h = (total_sec % 86400) / 3600;
    const qint64 m = (total_sec % 3600) / 60;
    const qint64 s = total_sec % 60;
    QString text;
    if (days > 0) {
        text = QStringLiteral("%1d %2h %3m").arg(days).arg(h).arg(m);
    } else if (h > 0) {
        text = QStringLiteral("%1h %2m").arg(h).arg(m);
    } else if (m > 0) {
        text = QStringLiteral("%1m %2s").arg(m).arg(s);
    } else {
        text = QStringLiteral("%1s").arg(qMax(qint64{1}, s));
    }
    lbl_uptime_value_->setText(text);
    const QString started_local =
        QDateTime::fromMSecsSinceEpoch(start_ms, Qt::LocalTime)
            .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    lbl_uptime_value_->setToolTip(
        QStringLiteral("OCU session started %1 (local time).").arg(started_local));
}

void DashboardScreen::refreshSystemStatusCard() {
    if (!lbl_status_value_) {
        return;
    }

    // 5 s grace period after first show — give the MQTT subscriber a
    // chance to deliver its first payload before flipping the card to
    // NOT READY just because we have no telemetry yet.
    constexpr qint64 kInitGraceMs = 5000;
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    const bool in_grace =
        dashboard_first_shown_ms_ > 0 &&
        (now_ms - dashboard_first_shown_ms_) < kInitGraceMs &&
        !battery_has_payload_;

    // Robot-reachability proxy: fresh MQTT payload = robot's network +
    // telemetry pipeline are alive.
    const bool reachable = robotReachableViaMqttBattery();

    // Three battery tiers for the System Status rollup. Decoupled from
    // the MQTT card thresholds (kBatteryCriticalPct = 12, kBatteryLowPct
    // = 25) per operator policy: the System Status card flips to NOT
    // READY at SOC < 20 %.
    constexpr double kStatusBatteryCriticalPct = 20.0;
    constexpr double kStatusBatteryLowPct = 25.0;

    bool battery_critical_for_status = false;
    bool battery_warning_for_status = false;
    if (battery_has_payload_ && battery_soc_pct_.has_value()) {
        const double soc = battery_soc_pct_.value();
        battery_critical_for_status = soc < kStatusBatteryCriticalPct;
        battery_warning_for_status =
            !battery_critical_for_status && soc < kStatusBatteryLowPct;
    }
    const bool battery_stale_for_status =
        battery_has_payload_ &&
        battery_payload_updated_at_ms_ > 0 &&
        (now_ms - battery_payload_updated_at_ms_) >
            battery_payload_stale_after_ms_;

    const bool preflight_fail = (preflight_status_ == QStringLiteral("FAIL"));
    const bool preflight_warn = (preflight_status_ == QStringLiteral("WARN"));

    SystemStatus s;
    QStringList tooltip_lines;
    if (in_grace) {
        s = SystemStatus::Initializing;
        tooltip_lines << QStringLiteral(
            "Waiting for first battery telemetry payload …");
    } else {
        const bool not_ready = preflight_fail || !reachable ||
                               battery_critical_for_status;
        const bool warning = preflight_warn || battery_warning_for_status ||
                             battery_stale_for_status;
        if (not_ready) {
            s = SystemStatus::NotReady;
            if (preflight_fail) {
                tooltip_lines << QStringLiteral(
                    "Preflight diagnostics reported FAIL.");
            }
            if (!reachable) {
                tooltip_lines << QStringLiteral(
                    "Robot unreachable (no fresh battery telemetry).");
            }
            if (battery_critical_for_status) {
                tooltip_lines << QStringLiteral(
                    "Battery below 20%% (current %1%).")
                                     .arg(QString::number(
                                         battery_soc_pct_.value_or(0.0),
                                         'f', 0));
            }
        } else if (warning) {
            s = SystemStatus::Warning;
            if (preflight_warn) {
                tooltip_lines << QStringLiteral(
                    "Preflight diagnostics reported WARN.");
            }
            if (battery_warning_for_status) {
                tooltip_lines << QStringLiteral(
                    "Battery at %1%% (warning band 20–25%%).")
                                     .arg(QString::number(
                                         battery_soc_pct_.value_or(0.0),
                                         'f', 0));
            }
            if (battery_stale_for_status) {
                tooltip_lines << QStringLiteral(
                    "Battery telemetry stale (last payload > "
                    "stale_after_ms ago).");
            }
        } else {
            s = SystemStatus::Ready;
            tooltip_lines << QStringLiteral(
                "Preflight OK, robot reachable, battery healthy.");
        }
    }

    lbl_status_value_->setText(QString::fromLatin1(statusCardText(s)));
    lbl_status_value_->setToolTip(tooltip_lines.join(QChar('\n')));
    // Override the theme color set by applyStyle's value_style template.
    lbl_status_value_->setStyleSheet(
        QStringLiteral(
            "font-family: 'Arimo'; font-weight: 700; font-size: 24px; "
            "line-height: 32px; color: %1;")
            .arg(QString::fromLatin1(statusCardColorHex(s))));
}

const char* DashboardScreen::statusCardText(SystemStatus s) {
    switch (s) {
        case SystemStatus::Initializing: return "INITIALIZING";
        case SystemStatus::Ready:        return "READY";
        case SystemStatus::Warning:      return "WARNING";
        case SystemStatus::NotReady:     return "NOT READY";
    }
    return "—";
}

const char* DashboardScreen::statusCardColorHex(SystemStatus s) {
    switch (s) {
        case SystemStatus::Initializing: return "#9F9FA9";
        case SystemStatus::Ready:        return "#009966";
        case SystemStatus::Warning:      return "#E17100";
        case SystemStatus::NotReady:     return "#E74C3C";
    }
    return "#9F9FA9";
}

// =============================================================================
// Last-calibration SSH probe
// =============================================================================

void DashboardScreen::startCalibrationProbe() {
    stopCalibrationProbe();

    if (robot_host_.isEmpty() || robot_ssh_user_.isEmpty()) {
        setCalibrationDisplay(
            QStringLiteral("—"),
            QStringLiteral(
                "No robot host / ssh_user configured (robots.json)."));
        return;
    }

    calibration_proc_ = new QProcess(this);
    calibration_proc_->setProcessChannelMode(QProcess::SeparateChannels);
    connect(calibration_proc_,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            &DashboardScreen::onCalibrationProbeFinished);

    // BatchMode=yes prevents SSH from blocking on a password prompt — if
    // keys aren't set up we get a clean exit-code failure rather than a
    // hung QProcess. ConnectTimeout caps the radio round-trip.
    //
    // Combined probe: single SSH round-trip returns three values
    //   <cal_mtime_epoch_or_0> <total_scans> <scans_since_cal>
    // separated by whitespace. Three round-trips would be wasteful when
    // they all share an SSH session — let the robot's shell aggregate.
    QStringList args;
    args << QStringLiteral("-o") << QStringLiteral("BatchMode=yes")
         << QStringLiteral("-o") << QStringLiteral("ConnectTimeout=5")
         << QStringLiteral("-o") << QStringLiteral("StrictHostKeyChecking=accept-new")
         << QStringLiteral("%1@%2").arg(robot_ssh_user_).arg(robot_host_)
         << QStringLiteral(
                // shellcheck-tolerant one-liner; quotes are escaped for Qt.
                "LATEST_CAL=$(ls -t /R_DATA/tilt_calibration/tilt_correction_matrices_*.npz "
                "2>/dev/null | head -1); "
                "CAL_M=0; "
                "if [ -n \"$LATEST_CAL\" ]; then "
                "  CAL_M=$(stat -c '%Y' \"$LATEST_CAL\" 2>/dev/null || echo 0); "
                "fi; "
                "TOTAL=$(find /R_DATA -maxdepth 2 -type d -name 'Section_*' "
                "2>/dev/null | wc -l); "
                "SINCE=0; "
                "if [ -n \"$LATEST_CAL\" ]; then "
                "  SINCE=$(find /R_DATA -maxdepth 2 -type d -name 'Section_*' "
                "-newer \"$LATEST_CAL\" 2>/dev/null | wc -l); "
                "else "
                "  SINCE=$TOTAL; "
                "fi; "
                "echo \"$CAL_M $TOTAL $SINCE\"");
    calibration_proc_->start(QStringLiteral("ssh"), args);

    // Hard deadline in case the connection itself stalls before SSH's own
    // ConnectTimeout fires.
    QTimer::singleShot(kCalibrationProbeTimeoutMs, calibration_proc_, [this]() {
        if (calibration_proc_ && calibration_proc_->state() != QProcess::NotRunning) {
            calibration_proc_->kill();
        }
    });

    if (!calibration_proc_->waitForStarted(1500)) {
        update::log::warn(
            "dashboard",
            QStringLiteral("calibration probe: ssh failed to start: %1")
                .arg(calibration_proc_->errorString()));
        stopCalibrationProbe();
        setCalibrationDisplay(QStringLiteral("—"),
                              QStringLiteral("Could not start SSH probe."));
    }
}

void DashboardScreen::stopCalibrationProbe() {
    if (!calibration_proc_) {
        return;
    }
    calibration_proc_->blockSignals(true);
    if (calibration_proc_->state() != QProcess::NotRunning) {
        calibration_proc_->kill();
        calibration_proc_->waitForFinished(750);
    }
    calibration_proc_->deleteLater();
    calibration_proc_ = nullptr;
}

void DashboardScreen::onCalibrationProbeFinished() {
    if (!calibration_proc_) {
        return;
    }
    const int exit_code = calibration_proc_->exitCode();
    const QByteArray stdout_text = calibration_proc_->readAllStandardOutput();
    const QByteArray stderr_text = calibration_proc_->readAllStandardError();

    if (exit_code != 0) {
        update::log::warn(
            "dashboard",
            QStringLiteral(
                "calibration probe: ssh exit=%1 stderr=%2")
                .arg(exit_code)
                .arg(QString::fromUtf8(stderr_text).trimmed()));
        setCalibrationDisplay(QStringLiteral("unreachable"),
                              QStringLiteral("SSH probe failed (exit %1).\n%2")
                                  .arg(exit_code)
                                  .arg(QString::fromUtf8(stderr_text).trimmed()));
        setScansAndCalibrationDisplays(-1, -1);
        stopCalibrationProbe();
        return;
    }

    const QString line = QString::fromUtf8(stdout_text).trimmed();
    static const QRegularExpression kWhitespaceRx(QStringLiteral("\\s+"));
    const QStringList parts = line.split(kWhitespaceRx, Qt::SkipEmptyParts);
    // Expected: "<cal_mtime> <total_scans> <scans_since_cal>"
    if (parts.size() < 3) {
        update::log::warn(
            "dashboard",
            QStringLiteral(
                "calibration probe: unexpected output (%1 fields): %2")
                .arg(parts.size())
                .arg(line));
        setCalibrationDisplay(
            QStringLiteral("—"),
            QStringLiteral("Unexpected probe output: %1").arg(line));
        setScansAndCalibrationDisplays(-1, -1);
        stopCalibrationProbe();
        return;
    }

    bool ok_m = false, ok_t = false, ok_s = false;
    const qint64 mtime_secs = parts[0].toLongLong(&ok_m);
    const int total_scans = parts[1].toInt(&ok_t);
    const int scans_since_cal = parts[2].toInt(&ok_s);

    if (mtime_secs <= 0 || !ok_m) {
        // No file means the operator has never run tilt calibration on
        // this robot — explicit "never" is more useful than empty.
        setCalibrationDisplay(QStringLiteral("never"),
                              QStringLiteral(
                                  "No tilt_correction_matrices_*.npz found in "
                                  "/R_DATA/tilt_calibration/. Run Calibrate Tilt "
                                  "to generate one."));
    } else {
        calibration_last_mtime_ = QDateTime::fromSecsSinceEpoch(mtime_secs);
        const QString relative = formatRelativeTime(calibration_last_mtime_);
        const QString absolute =
            calibration_last_mtime_.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
        setCalibrationDisplay(
            relative,
            QStringLiteral("Last tilt calibration: %1").arg(absolute));
    }

    setScansAndCalibrationDisplays(ok_t ? total_scans : -1,
                                   ok_s ? scans_since_cal : -1);
    stopCalibrationProbe();
}

void DashboardScreen::onCalibrationRefreshTimerTick() {
    startCalibrationProbe();
}

void DashboardScreen::setCalibrationDisplay(const QString& valueText,
                                            const QString& tooltip) {
    if (!lbl_calibration_value_info_) {
        return;
    }
    lbl_calibration_value_info_->setText(valueText);
    lbl_calibration_value_info_->setToolTip(tooltip);
}

void DashboardScreen::setScansAndCalibrationDisplays(int totalScans,
                                                     int scansSinceCal) {
    // Top-row Total Scans card. Negative sentinel → probe failed, fall
    // back to em-dash so the operator can tell live vs stale at a glance.
    if (lbl_scans_value_) {
        if (totalScans < 0) {
            lbl_scans_value_->setText(QStringLiteral("—"));
            lbl_scans_value_->setToolTip(
                QStringLiteral("Robot unreachable — last live count unavailable."));
        } else {
            lbl_scans_value_->setText(QString::number(totalScans));
            lbl_scans_value_->setToolTip(
                QStringLiteral("Total Section_* folders under /R_DATA/."));
        }
    }

    // Top-row Next Calibration card. We count scans created after the
    // latest tilt-calibration file's mtime. Once that exceeds the soft
    // limit the card flips to "Due now" and starts blinking — operator
    // can click the card to run Calibrate Tilt.
    if (!lbl_calibration_value_) {
        return;
    }
    bool due = false;
    if (scansSinceCal < 0) {
        lbl_calibration_value_->setText(QStringLiteral("—"));
        lbl_calibration_value_->setToolTip(
            QStringLiteral("Robot unreachable — calibration countdown unavailable."));
    } else if (scansSinceCal >= kCalibrationDueAfterScans) {
        due = true;
        lbl_calibration_value_->setText(QStringLiteral("Due now"));
        lbl_calibration_value_->setToolTip(
            QStringLiteral(
                "%1 scans since last tilt calibration (policy: every %2). "
                "Click the card to run Calibrate Tilt.")
                .arg(scansSinceCal)
                .arg(kCalibrationDueAfterScans));
    } else {
        const int remaining = kCalibrationDueAfterScans - scansSinceCal;
        lbl_calibration_value_->setText(
            remaining == 1 ? QStringLiteral("1 scan")
                           : QStringLiteral("%1 scans").arg(remaining));
        lbl_calibration_value_->setToolTip(
            QStringLiteral("%1 scans since last tilt calibration; %2 until next "
                           "recommended (policy: every %3).")
                .arg(scansSinceCal)
                .arg(remaining)
                .arg(kCalibrationDueAfterScans));
    }
    setCalibrationDueBlink(due);
}

void DashboardScreen::setCalibrationDueBlink(bool blink) {
    if (!card_calibration_) {
        return;
    }
    if (blink == calibration_blink_active_ && calibration_blink_anim_) {
        return;  // already in the requested state
    }
    calibration_blink_active_ = blink;

    if (!blink) {
        if (calibration_blink_anim_) {
            calibration_blink_anim_->stop();
        }
        if (calibration_blink_effect_) {
            calibration_blink_effect_->setOpacity(1.0);
        }
        return;
    }

    // Lazy-init the effect + animation. We attach a single
    // QGraphicsOpacityEffect to the card and pulse its opacity in a loop
    // so the whole tile fades. QPropertyAnimation handles the timing on
    // the Qt event loop — no extra QTimer needed.
    if (!calibration_blink_effect_) {
        calibration_blink_effect_ = new QGraphicsOpacityEffect(card_calibration_);
        calibration_blink_effect_->setOpacity(1.0);
        card_calibration_->setGraphicsEffect(calibration_blink_effect_);
    }
    if (!calibration_blink_anim_) {
        calibration_blink_anim_ =
            new QPropertyAnimation(calibration_blink_effect_, "opacity", this);
        calibration_blink_anim_->setDuration(900);
        calibration_blink_anim_->setStartValue(1.0);
        calibration_blink_anim_->setKeyValueAt(0.5, 0.45);
        calibration_blink_anim_->setEndValue(1.0);
        calibration_blink_anim_->setLoopCount(-1);  // forever until stopped
    }
    calibration_blink_anim_->start();
}

bool DashboardScreen::eventFilter(QObject* watched, QEvent* event) {
    if (watched == card_calibration_ && event &&
        event->type() == QEvent::MouseButtonRelease) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton &&
            card_calibration_->rect().contains(me->pos())) {
            onCalibrateTiltRequested();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

QString DashboardScreen::formatRelativeTime(const QDateTime& past) {
    if (!past.isValid()) {
        return QStringLiteral("—");
    }
    const qint64 secs = past.secsTo(QDateTime::currentDateTimeUtc());
    if (secs < 0) {
        // Robot clock ahead of laptop clock — render as "just now" rather
        // than a confusing negative.
        return QStringLiteral("just now");
    }
    if (secs < 60) {
        return QStringLiteral("just now");
    }
    if (secs < 3600) {
        const qint64 mins = secs / 60;
        return QStringLiteral("%1 min%2 ago").arg(mins).arg(mins == 1 ? "" : "s");
    }
    if (secs < 86400) {
        const qint64 hours = secs / 3600;
        return QStringLiteral("%1 hour%2 ago").arg(hours).arg(hours == 1 ? "" : "s");
    }
    if (secs < 7 * 86400) {
        const qint64 days = secs / 86400;
        return QStringLiteral("%1 day%2 ago").arg(days).arg(days == 1 ? "" : "s");
    }
    if (secs < 30 * 86400) {
        const qint64 weeks = secs / (7 * 86400);
        return QStringLiteral("%1 week%2 ago").arg(weeks).arg(weeks == 1 ? "" : "s");
    }
    if (secs < 365 * 86400) {
        const qint64 months = secs / (30 * 86400);
        return QStringLiteral("%1 month%2 ago").arg(months).arg(months == 1 ? "" : "s");
    }
    const qint64 years = secs / (365 * 86400);
    return QStringLiteral("%1 year%2 ago").arg(years).arg(years == 1 ? "" : "s");
}

void DashboardScreen::applyStyle() {
    setProperty("theme", QVariant(dark_mode_ ? QStringLiteral("dark") : QStringLiteral("light")));
    setStyleSheet(R"(
        #DashboardRoot {
            background-color: #FAFAFA;
            font-family: "Arimo";
        }
        #DashboardHeader {
            background: #FFFFFF;
            border-bottom: 1px solid #E4E4E7;
        }
        #DashboardTitle {
            font-weight: 700;
            font-size: 30px;
            line-height: 36px;
            color: #18181B;
        }
        #DashboardSubtitle {
            font-size: 16px;
            line-height: 24px;
            color: #71717B;
        }
        #DashboardLogoutButton {
            background: #FFFFFF;
            border: 1px solid #E4E4E7;
            border-radius: 4px;
            padding: 0 16px;
            min-width: 113px;
            height: 40px;
            color: #71717B;
            font-size: 16px;
            line-height: 24px;
        }
        #DashboardLogoutButton:hover {
            background: #F4F4F5;
        }
        #DashboardLogoutButton:pressed {
            background: #E4E4E7;
        }

        #DashboardContent {
            background-color: #FAFAFA;
        }
        #DashboardCardStatus, #DashboardCardScans, #DashboardCardCameras, #DashboardCardCalibration {
            background: #FFFFFF;
            border: 1px solid #E4E4E7;
            border-radius: 10px;
        }
        #DashboardActionsCard {
            background: #FFFFFF;
            border: 1px solid #E4E4E7;
            border-radius: 10px;
        }
        #DashboardInfoCard {
            background: #FFFFFF;
            border: 1px solid #E4E4E7;
            border-radius: 10px;
        }

        /* Dark theme overrides */
        #DashboardRoot[theme="dark"] {
            background-color: #09090B;
        }
        #DashboardRoot[theme="dark"] #DashboardHeader {
            background: #18181B;
            border-bottom: 1px solid #27272A;
        }
        #DashboardRoot[theme="dark"] #DashboardTitle {
            color: #FFFFFF;
        }
        #DashboardRoot[theme="dark"] #DashboardSubtitle {
            color: #9F9FA9;
        }
        #DashboardRoot[theme="dark"] #DashboardLogoutButton {
            background: transparent;
            border: 1px solid #27272A;
            color: #9F9FA9;
        }
        #DashboardRoot[theme="dark"] #DashboardLogoutButton:hover {
            background: #27272A;
        }
        #DashboardRoot[theme="dark"] #DashboardContent {
            background-color: #09090B;
        }
        #DashboardRoot[theme="dark"] #DashboardCardStatus,
        #DashboardRoot[theme="dark"] #DashboardCardScans,
        #DashboardRoot[theme="dark"] #DashboardCardCameras,
        #DashboardRoot[theme="dark"] #DashboardCardCalibration,
        #DashboardRoot[theme="dark"] #DashboardActionsCard {
            background: #18181B;
            border: 1px solid #27272A;
        }
        #DashboardRoot[theme="dark"] #DashboardInfoCard {
            background: #18181B;
            border: 1px solid #27272A;
        }
    )");

    const bool is_dark = dark_mode_;
    const QString accent = "#00BC7D";
    const QString card_label_color = is_dark ? "#9F9FA9" : "#71717B";
    const QString actions_title_color = is_dark ? "#FFFFFF" : "#18181B";
    const QString action_desc_color = "#71717B";
    const QString info_title_color = is_dark ? "#FFFFFF" : "#18181B";
    const QString info_label_color = "#71717B";
    const QString info_value_color = is_dark ? "#FFFFFF" : "#18181B";

    auto setLabelStyle = [](QLabel* label, const QString& style) {
        if (label) {
            label->setStyleSheet(style);
        }
    };

    if (btn_logout_) {
        QPixmap logoutPix = loadSvgPixmap(":/assets/dashboard/logout.svg", 20, 20,
                                          is_dark ? "#9F9FA9" : "#71717B");
        if (!logoutPix.isNull()) {
            btn_logout_->setIcon(QIcon(logoutPix));
            btn_logout_->setIconSize(QSize(20, 20));
        }
    }

    auto setCardLabelStyle = [&](const QString& objectName) {
        if (auto* label = findChild<QLabel*>(objectName)) {
            setLabelStyle(label, QString("font-size: 14px; line-height: 20px; color: %1;")
                                     .arg(card_label_color));
        }
    };

    setCardLabelStyle("DashboardCardStatusLabel");
    setCardLabelStyle("DashboardCardScansLabel");
    setCardLabelStyle("DashboardCardCamerasLabel");
    setCardLabelStyle("DashboardCardCalibrationLabel");

    const QString value_style =
        "font-family: 'Arimo'; font-weight: 700; font-size: 24px; line-height: 32px; color: %1;";
    setLabelStyle(lbl_status_value_, QString(value_style).arg(accent));
    setLabelStyle(lbl_scans_value_, QString(value_style).arg(is_dark ? "#FFFFFF" : "#18181B"));
    setLabelStyle(lbl_battery_card_value_, QString(value_style).arg(is_dark ? "#FFFFFF" : "#18181B"));
    setLabelStyle(lbl_calibration_value_, QString(value_style).arg(is_dark ? "#FFFFFF" : "#18181B"));

    if (auto* actionsTitle = findChild<QLabel*>("DashboardActionsTitle")) {
        setLabelStyle(actionsTitle,
                      QString("font-family: 'Arimo'; font-weight: 700; font-size: 24px; "
                              "line-height: 32px; color: %1;")
                          .arg(actions_title_color));
    }

    auto setActionTitleStyle = [&](const QString& objectName, const QString& color) {
        if (auto* label = findChild<QLabel*>(objectName)) {
            setLabelStyle(label,
                          QString("font-family: 'Arimo'; font-weight: 700; font-size: 18px; "
                                  "line-height: 28px; color: %1;")
                              .arg(color));
        }
    };
    auto setActionDescStyle = [&](const QString& objectName) {
        if (auto* label = findChild<QLabel*>(objectName)) {
            setLabelStyle(label,
                          QString("font-family: 'Arimo'; font-size: 14px; line-height: 20px; "
                                  "color: %1;")
                              .arg(action_desc_color));
        }
    };

    setActionTitleStyle("DashboardBtnStartScanTitle", is_dark ? "#FFFFFF" : "#18181B");
    setActionTitleStyle("DashboardBtnDiagnosticsTitle", is_dark ? "#FFFFFF" : "#18181B");
    setActionTitleStyle("DashboardBtnRecordingsTitle", is_dark ? "#FFFFFF" : "#18181B");
    setActionTitleStyle("DashboardBtnCalibrateTiltTitle", is_dark ? "#FFFFFF" : "#18181B");
    setActionDescStyle("DashboardBtnStartScanDesc");
    setActionDescStyle("DashboardBtnDiagnosticsDesc");
    setActionDescStyle("DashboardBtnRecordingsDesc");
    setActionDescStyle("DashboardBtnCalibrateTiltDesc");

    auto* infoTitle = findChild<QLabel*>("DashboardInfoTitle");
    if (infoTitle) {
        setLabelStyle(infoTitle,
                      QString("font-family: 'Arimo'; font-weight: 700; font-size: 18px; "
                              "line-height: 27px; color: %1;")
                          .arg(info_title_color));
    }

    const QString info_value_style =
        "font-family: 'Arimo'; font-size: 14px; line-height: 20px; color: %1;";
    setLabelStyle(lbl_robot_id_value_, QString(info_value_style).arg(info_value_color));
    setLabelStyle(lbl_firmware_value_, QString(info_value_style).arg(info_value_color));
    setLabelStyle(lbl_calibration_value_info_, QString(info_value_style).arg(info_value_color));
    setLabelStyle(lbl_battery_value_, QString(info_value_style).arg(info_value_color));
    setLabelStyle(lbl_uptime_value_, QString(info_value_style).arg(info_value_color));

    if (auto* infoCard = findChild<QWidget*>("DashboardInfoCard")) {
        const auto labels = infoCard->findChildren<QLabel*>();
        for (auto* label : labels) {
            if (!label || label == infoTitle ||
                label == lbl_robot_id_value_ || label == lbl_firmware_value_ ||
                label == lbl_calibration_value_info_ || label == lbl_battery_value_ ||
                label == lbl_uptime_value_) {
                continue;
            }
            setLabelStyle(label,
                          QString("font-family: 'Arimo'; font-weight: 700; font-size: 14px; "
                                  "line-height: 20px; color: %1;")
                              .arg(info_label_color));
        }
    }

    const QString icon_bg = "rgba(0, 188, 125, 0.1)";
    auto updateCardIcon = [&](const QString& cardName, const QString& resourcePath,
                              const QString& bgColor, const QString& strokeColor) {
        if (auto* icon = findChild<QLabel*>(cardName + "Icon")) {
            icon->setStyleSheet(QString("background: %1; border-radius: 10px;").arg(bgColor));
            QPixmap iconPix = loadSvgPixmap(resourcePath, 32, 32, strokeColor);
            if (!iconPix.isNull()) {
                icon->setPixmap(iconPix);
            }
        }
    };

    updateCardIcon("DashboardCardStatus", ":/assets/dashboard/heartbeat.svg",
                   icon_bg, accent);
    updateCardIcon("DashboardCardScans", ":/assets/dashboard/location_pin.svg",
                   icon_bg, accent);
    updateCardIcon("DashboardCardCameras", ":/assets/dashboard/battery.svg",
                   icon_bg, accent);
    updateCardIcon("DashboardCardCalibration", ":/assets/dashboard/settings.svg",
                   icon_bg, accent);

    const QString action_bg = is_dark ? "#27272A" : "#FFFFFF";
    const QString action_border = is_dark ? "#27272A" : "#E4E4E7";
    const QString action_hover = is_dark ? "#2f2f33" : "#F4F4F5";
    const int action_border_width = 1;

    auto updateActionButtonStyle = [&](QPushButton* button) {
        if (!button) {
            return;
        }
        const QString selector = QString("#%1").arg(button->objectName());
        button->setStyleSheet(QString(
            "%1 {"
            "  background: %2;"
            "  border: %3px solid %4;"
            "  border-radius: 10px;"
            "  text-align: left;"
            "}"
            "%1:hover:enabled { background: %5; }"
            "%1:focus { outline: none; }")
            .arg(selector, action_bg, QString::number(action_border_width),
                 action_border, action_hover));
    };

    updateActionButtonStyle(btn_start_scan_);
    updateActionButtonStyle(btn_run_diagnostics_);
    updateActionButtonStyle(btn_view_recordings_);
    updateActionButtonStyle(btn_calibrate_tilt_);

    auto updateActionIcon = [&](const QString& buttonName, const QString& resourcePath,
                                const QString& strokeColor) {
        if (auto* icon = findChild<QLabel*>(buttonName + "Icon")) {
            QPixmap iconPix = loadSvgPixmap(resourcePath, 40, 40, strokeColor);
            if (!iconPix.isNull()) {
                icon->setPixmap(iconPix);
            }
        }
    };

    updateActionIcon("DashboardBtnStartScan", ":/assets/dashboard/location_pin.svg", accent);
    updateActionIcon("DashboardBtnDiagnostics", ":/assets/dashboard/heartbeat.svg", accent);
    updateActionIcon("DashboardBtnRecordings", ":/assets/dashboard/camera.svg", accent);
    updateActionIcon("DashboardBtnCalibrateTilt", ":/assets/dashboard/settings.svg", "#E17100");
}

}  // namespace f2c_cpp
