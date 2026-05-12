#include "app_shell.hpp"

#include <algorithm>
#include <functional>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <cstdlib>
#include <QApplication>
#include <QAbstractButton>
#include <QAbstractSocket>
#include <QCloseEvent>
#include <QDate>
#include <QGraphicsBlurEffect>
#include <QHBoxLayout>
#include <QFile>
#include <QMessageBox>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QNetworkInterface>
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStringList>
#include <QStackedWidget>
#include <QToolButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QStyle>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QSvgRenderer>
#else
#include <QtSvg/QSvgRenderer>
#endif

#include "cloud_upload_manager.hpp"
#include "components/bdr_message_box.hpp"
#include "components/mission_metadata_dialog.hpp"
#include "components/offline_finalize_dialog.hpp"
#include "components/rollback_banner.hpp"
#include "components/update_banner.hpp"
#include "components/update_modal.hpp"
#include "dashboard_screen.hpp"
#include "exploration_screen.hpp"
#include "planner_screen.hpp"
#include "robot_registry.hpp"
#include "setup_screen.hpp"
#include "settings_constants.hpp"
#include "startup_screen.hpp"
#include "transfer_manager.hpp"
#include "units_system.hpp"
#include "update/update_checker.hpp"
#include "update/update_lockfile.hpp"
#include "update/update_state.hpp"
#include "update/update_log.hpp"
#include "update/update_types.hpp"

namespace f2c_cpp {

namespace {

constexpr int kLocalNavGridBytes = 288;
constexpr qint64 kLocalNavGridStaleMs = 3000;
constexpr int kThermalThumbBytes = 384;
constexpr qint64 kThermalThumbStaleMs = 1500;
constexpr qint64 kControllerStatusStaleMs = 1500;
constexpr int kSaveRawMapMaxAttempts = 2;
constexpr int kCorrectedMapRetryCount = 1;
constexpr int kCorrectedMapRetryDelayMs = 750;
constexpr int kOdriveAxisStateIdle = 1;
constexpr int kOdriveAxisStateClosedLoopControl = 8;

QString sshUserHostSpec(const ResolvedRobotSshTarget& t) {
    const QString user =
        t.ssh_user.trimmed().isEmpty() ? QStringLiteral("roofus") : t.ssh_user.trimmed();
    return QStringLiteral("%1@%2").arg(user, t.host);
}

QPixmap loadSvgPixmap(const QString& resource_path, int width, int height, const QString& color = QString()) {
    QFile file(resource_path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QPixmap();
    }

    QString svg = QString::fromUtf8(file.readAll());
    file.close();
    if (!color.isEmpty()) {
        svg.replace(QStringLiteral("currentColor"), color);
    }

    static const QRegularExpression kFigmaVarColorPattern(
        QStringLiteral(R"(var\(--(?:fill|stroke)-\d+,\s*(#[0-9A-Fa-f]{3,8})\s*\))"));
    QString resolved_svg;
    resolved_svg.reserve(svg.size());
    int cursor = 0;
    QRegularExpressionMatchIterator it = kFigmaVarColorPattern.globalMatch(svg);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const int start = match.capturedStart(0);
        const int end = match.capturedEnd(0);
        if (start < 0 || end < start) {
            continue;
        }
        resolved_svg += svg.mid(cursor, start - cursor);
        resolved_svg += color.isEmpty() ? match.captured(1) : color;
        cursor = end;
    }
    if (cursor > 0) {
        resolved_svg += svg.mid(cursor);
        svg = resolved_svg;
    }

    QSvgRenderer renderer(svg.toUtf8());
    if (!renderer.isValid()) {
        return QPixmap();
    }

    QPixmap pixmap(width, height);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    renderer.render(&painter);
    return pixmap;
}

enum class SharedTopStatusTone {
    Good,
    Warning,
    Muted,
    Error
};

struct SharedTopStatusState {
    QString signal_text = QStringLiteral("Signal unavailable");
    SharedTopStatusTone signal_tone = SharedTopStatusTone::Muted;
    QString lock_text = QStringLiteral("Not Ready");
    SharedTopStatusTone lock_tone = SharedTopStatusTone::Error;
};

ExplorationScreen::ValueTone toExplorationTone(SharedTopStatusTone tone) {
    switch (tone) {
        case SharedTopStatusTone::Good:
            return ExplorationScreen::ValueTone::Good;
        case SharedTopStatusTone::Warning:
            return ExplorationScreen::ValueTone::Warning;
        case SharedTopStatusTone::Muted:
            return ExplorationScreen::ValueTone::Muted;
        case SharedTopStatusTone::Error:
        default:
            return ExplorationScreen::ValueTone::Error;
    }
}

PlannerScreen::ValueTone toPlannerTone(SharedTopStatusTone tone) {
    switch (tone) {
        case SharedTopStatusTone::Good:
            return PlannerScreen::ValueTone::Good;
        case SharedTopStatusTone::Warning:
            return PlannerScreen::ValueTone::Warning;
        case SharedTopStatusTone::Muted:
            return PlannerScreen::ValueTone::Muted;
        case SharedTopStatusTone::Error:
        default:
            return PlannerScreen::ValueTone::Error;
    }
}

SharedTopStatusState computeSharedTopStatus(bool launch_ready,
                                            bool launch_in_progress,
                                            bool launch_failed,
                                            bool laptop_launch_confirmed,
                                            bool robot_launch_confirmed,
                                            bool laptop_launch_started,
                                            bool robot_launch_started,
                                            bool local_zenoh_ready,
                                            bool rf_metric_fresh,
                                            int rf_rssi_dbm) {
    SharedTopStatusState status;

    if (launch_ready) {
        status.lock_text = QStringLiteral("Locked");
        status.lock_tone = SharedTopStatusTone::Good;
    } else if (launch_in_progress) {
        status.lock_text = (!laptop_launch_confirmed || !robot_launch_confirmed)
                               ? QStringLiteral("Unlocking...")
                               : QStringLiteral("Starting");
        status.lock_tone = SharedTopStatusTone::Warning;
    } else if (launch_failed) {
        status.lock_text = QStringLiteral("Not Ready");
        status.lock_tone = SharedTopStatusTone::Error;
    } else if (laptop_launch_started || robot_launch_started || local_zenoh_ready) {
        status.lock_text = QStringLiteral("Standby");
        status.lock_tone = SharedTopStatusTone::Muted;
    }

    if (!rf_metric_fresh) {
        return status;
    }

    status.signal_text = QStringLiteral("%1 dBm").arg(rf_rssi_dbm);
    if (rf_rssi_dbm >= -65) {
        status.signal_tone = SharedTopStatusTone::Good;
    } else if (rf_rssi_dbm >= -80) {
        status.signal_tone = SharedTopStatusTone::Warning;
    } else {
        status.signal_tone = SharedTopStatusTone::Error;
    }
    return status;
}

double computeMedian(std::vector<double> values) {
    if (values.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    std::sort(values.begin(), values.end());
    const size_t n = values.size();
    if ((n % 2U) == 1U) {
        return values[n / 2U];
    }
    return 0.5 * (values[(n / 2U) - 1U] + values[n / 2U]);
}

double computeMad(const std::vector<double>& values, double median) {
    if (values.empty() || !std::isfinite(median)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    std::vector<double> abs_dev;
    abs_dev.reserve(values.size());
    for (double value : values) {
        abs_dev.push_back(std::abs(value - median));
    }
    return computeMedian(std::move(abs_dev));
}

bool parseTrailingInteger(const QString& text, int* value_out) {
    if (!value_out) {
        return false;
    }
    const QRegularExpression rx(R"((-?\d+)\s*"?\s*$)");
    const QRegularExpressionMatch match = rx.match(text.trimmed());
    if (!match.hasMatch()) {
        return false;
    }
    bool ok = false;
    const int parsed = match.captured(1).toInt(&ok);
    if (!ok) {
        return false;
    }
    *value_out = parsed;
    return true;
}

bool parseKeyValueDouble(const QString& text, const QString& key, double* value_out) {
    if (!value_out || key.trimmed().isEmpty()) {
        return false;
    }
    const QString pattern =
        QString(R"((?:^|,)\s*%1\s*=\s*(-?\d+(?:\.\d+)?))")
            .arg(QRegularExpression::escape(key.trimmed()));
    const QRegularExpression rx(pattern);
    const QRegularExpressionMatch match = rx.match(text.trimmed());
    if (!match.hasMatch()) {
        return false;
    }
    bool ok = false;
    const double value = match.captured(1).toDouble(&ok);
    if (!ok || !std::isfinite(value)) {
        return false;
    }
    *value_out = value;
    return true;
}

double yawFromQuaternion(const geometry_msgs::msg::Quaternion& quaternion) {
    const double siny_cosp =
        2.0 * ((quaternion.w * quaternion.z) + (quaternion.x * quaternion.y));
    const double cosy_cosp =
        1.0 - (2.0 * ((quaternion.y * quaternion.y) + (quaternion.z * quaternion.z)));
    return std::atan2(siny_cosp, cosy_cosp);
}

QString shellSingleQuote(const QString& text) {
    QString escaped = text;
    escaped.replace('\'', QStringLiteral("'\"'\"'"));
    return QStringLiteral("'") + escaped + QStringLiteral("'");
}

}  // namespace

AppShellWindow::AppShellWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle("BDR Coverage Planning Suite");
    setMinimumSize(1100, 700);
    // Match the refreshed mission planner frame size on startup.
    resize(1426, 919);
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint | Qt::Window);

    QSettings settings(f2c_cpp::kSettingsOrgName, f2c_cpp::kSettingsAppName);
    dark_mode_ = settings.value("ui/dark_mode", false).toBool();

    // Single source of truth for "is the robot reachable?".  See
    // link_health_monitor.hpp.  Armed at exploration launch start and
    // disarmed at teardown; consumed by Stages 4/5 BOT pill, banner,
    // and button lockout via PlannerScreen::setLinkConnectionState +
    // ExplorationScreen::setLinkConnectionState.
    link_monitor_ = new LinkHealthMonitor(this);
    connect(link_monitor_, &LinkHealthMonitor::linkStateChanged,
            this, &AppShellWindow::onLinkStateChanged);

    // Layered connectivity model.  The reachability probe answers
    // "is the host on the network at all?" via ICMP -> TCP-22.  Its
    // verdict is fed into LinkHealthMonitor::setReachability, which
    // resolves RECONNECTING (topics stale, probe ok) vs DISCONNECTED
    // (topics stale, probe fails).  Both probe and monitor arm/disarm
    // together — see startRobotCompleteLaunch / teardown paths.
    reachability_probe_ = new RobotReachabilityProbe(this);
    connect(reachability_probe_, &RobotReachabilityProbe::reachabilityChanged,
            this, [this](RobotReachabilityProbe::State /*old*/,
                          RobotReachabilityProbe::State next) {
                if (!link_monitor_) {
                    return;
                }
                const bool reachable =
                    (next == RobotReachabilityProbe::State::Reachable);
                link_monitor_->setReachability(reachable, /*known=*/true);
            });

    central_root_ = new QWidget(this);
    auto* root_layout = new QVBoxLayout(central_root_);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    // OTA banner sits above the stage stack so it persists across stage
    // transitions. Hidden by default; UpdateChecker drives visibility.
    update_banner_ = new UpdateBanner(central_root_);
    update_banner_->setDarkMode(dark_mode_);
    update_banner_->hide();
    {
        // Wrap the banner in a thin margin row so it doesn't hug the window
        // edge — keeps it readable at the top of the central root.
        auto* banner_host = new QWidget(central_root_);
        auto* banner_lay = new QHBoxLayout(banner_host);
        banner_lay->setContentsMargins(12, 8, 12, 0);
        banner_lay->setSpacing(0);
        banner_lay->addWidget(update_banner_);
        root_layout->addWidget(banner_host);
    }

    stack_ = new QStackedWidget(central_root_);
    root_layout->addWidget(stack_);
    setCentralWidget(central_root_);

    // OTA checker — polls GitHub Releases for new builds (locked spec Q2=A:
    // start at app startup, before Stage 1 even completes login).
    update_checker_ = new update::UpdateChecker(this);
    connect(update_checker_, &update::UpdateChecker::updateAvailable,
            this, [this](const update::VersionInfo& info) {
                update::log::info("appshell",
                                  QStringLiteral("update available: tag=%1 sha=%2")
                                      .arg(info.tag).arg(info.commitSha));
                update_banner_->setVersionInfo(info);
                update_banner_->show();
            });
    connect(update_checker_, &update::UpdateChecker::noUpdateAvailable,
            this, [this]() { update_banner_->hide(); });
    connect(update_checker_, &update::UpdateChecker::checkFailed,
            this, [](const QString& reason) {
                update::log::warn("appshell",
                                  QStringLiteral("update check failed: %1")
                                      .arg(reason));
            });
    connect(update_banner_, &UpdateBanner::viewDetailsRequested,
            this, [this](const update::VersionInfo& info) {
                update::log::info("appshell",
                                  QStringLiteral("view details: tag=%1 sha=%2")
                                      .arg(info.tag).arg(info.commitSha));
                showUpdateModal(info);
            });
    update_checker_->start();

    stage1_ = new SetupScreen(this);
    stack_->addWidget(stage1_);
    stack_->setCurrentWidget(stage1_);

    connect(stage1_, &SetupScreen::loginSubmitted, this, &AppShellWindow::onLoginSubmitted);

    laptop_launch_proc_ = new QProcess(this);
    laptop_launch_proc_->setProcessChannelMode(QProcess::MergedChannels);
    connect(laptop_launch_proc_,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this](int exit_code, QProcess::ExitStatus exit_status) {
                Q_UNUSED(exit_status);
                if (exploration_launch_in_progress_ && !exploration_launch_ready_) {
                    setExplorationLaunchFailed(
                        QString("Laptop launch exited early (code=%1)").arg(exit_code));
                }
            });
    connect(laptop_launch_proc_,
            &QProcess::errorOccurred,
            this,
            [this](QProcess::ProcessError) {
                if (exploration_launch_in_progress_ && !exploration_launch_ready_) {
                    setExplorationLaunchFailed(
                        QString("Laptop launch error: %1").arg(laptop_launch_proc_->errorString()));
                }
            });
    connect(laptop_launch_proc_,
            &QProcess::readyReadStandardOutput,
            this,
            [this]() {
                const QString output =
                    QString::fromUtf8(laptop_launch_proc_->readAllStandardOutput());
                if (output.isEmpty()) {
                    return;
                }

                laptop_launch_last_output_.append(output);
                constexpr int kMaxLaunchLogChars = 4000;
                if (laptop_launch_last_output_.size() > kMaxLaunchLogChars) {
                    laptop_launch_last_output_ =
                        laptop_launch_last_output_.right(kMaxLaunchLogChars);
                }

                if (!exploration_launch_in_progress_ || exploration_launch_ready_) {
                    return;
                }
                const QString lower = output.toLower();
                if (lower.contains("package 'pilot_control' not found")) {
                    setExplorationLaunchFailed("Laptop launch failed: package 'pilot_control' not found");
                    return;
                }
                if (lower.contains("command not found") || lower.contains("ros2: not found")) {
                    setExplorationLaunchFailed("Laptop launch failed: ROS environment not sourced");
                    return;
                }
                if (lower.contains("xterm: command not found")) {
                    setExplorationLaunchFailed("Laptop launch failed: xterm is missing on laptop");
                    return;
                }
            });

    robot_launch_proc_ = new QProcess(this);
    robot_launch_proc_->setProcessChannelMode(QProcess::MergedChannels);
    connect(robot_launch_proc_,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this](int exit_code, QProcess::ExitStatus exit_status) {
                Q_UNUSED(exit_status);
                if (exploration_launch_in_progress_ && !exploration_launch_ready_) {
                    setExplorationLaunchFailed(
                        QString("Robot launch exited early (code=%1)").arg(exit_code));
                }
            });
    connect(robot_launch_proc_,
            &QProcess::errorOccurred,
            this,
            [this](QProcess::ProcessError) {
                if (exploration_launch_in_progress_ && !exploration_launch_ready_) {
                    setExplorationLaunchFailed(
                        QString("Robot launch error: %1").arg(robot_launch_proc_->errorString()));
                }
            });
    connect(robot_launch_proc_,
            &QProcess::readyReadStandardOutput,
            this,
            [this]() {
                const QString output =
                    QString::fromUtf8(robot_launch_proc_->readAllStandardOutput());
                if (output.isEmpty()) {
                    return;
                }

                robot_launch_last_output_.append(output);
                constexpr int kMaxLaunchLogChars = 4000;
                if (robot_launch_last_output_.size() > kMaxLaunchLogChars) {
                    robot_launch_last_output_ = robot_launch_last_output_.right(kMaxLaunchLogChars);
                }

                if (!exploration_launch_in_progress_ || exploration_launch_ready_) {
                    return;
                }
                const QString lower = output.toLower();
                if (lower.contains("package 'pilot_control' not found")) {
                    setExplorationLaunchFailed("Robot launch failed: package 'pilot_control' not found");
                    return;
                }
                if (lower.contains("command not found") || lower.contains("ros2: not found")) {
                    setExplorationLaunchFailed("Robot launch failed: ROS environment not sourced");
                    return;
                }
                if ((lower.contains("sudo") && lower.contains("password")) ||
                    lower.contains("password for")) {
                    setExplorationLaunchFailed(
                        "Robot launch blocked: passwordless sudo is required on robot");
                    return;
                }
            });

    saved_map_download_proc_ = new QProcess(this);
    saved_map_download_proc_->setProcessChannelMode(QProcess::MergedChannels);
    connect(saved_map_download_proc_,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this](int exit_code, QProcess::ExitStatus exit_status) {
                const QString output =
                    QString::fromUtf8(saved_map_download_proc_->readAllStandardOutput());
                const QString remote_map_path = pending_saved_map_remote_path_;
                const QString local_map_path = pending_saved_map_local_path_;
                const int retries_remaining = pending_saved_map_retry_remaining_;
                if (!map_save_or_download_in_progress_ && remote_map_path.isEmpty() &&
                    local_map_path.isEmpty()) {
                    return;
                }

                const QFileInfo local_info(local_map_path);
                const bool download_ok =
                    (exit_status == QProcess::NormalExit && exit_code == 0 &&
                     local_info.exists() && local_info.size() > 0 &&
                     local_info.fileName().startsWith(QStringLiteral("corrected_map_")));

                if (download_ok) {
                    latest_saved_map_local_path_ = local_map_path;
                    map_save_or_download_in_progress_ = false;
                    clearPendingSavedMapState();
                    if (stage4_) {
                        stage4_->showMapReady();
                        stage4_->setPlanningEnabled(true);
                        stage4_->setLaunchProgress(100,
                                                   "Tilt-corrected map ready. Start Planning is enabled.");
                        stage4_->setLaunchDiagnostics(
                            buildExplorationDiagnostics(
                                QString("Selected corrected map: %1\nDownloaded to: %2")
                                    .arg(remote_map_path, local_map_path)));
                    }
                    return;
                }

                QString reason =
                    QString("Corrected map download failed (exit=%1): %2")
                        .arg(exit_code)
                        .arg(output.trimmed().left(220));
                if (reason.endsWith(':')) {
                    reason.chop(1);
                }

                if (retries_remaining > 0) {
                    clearPendingSavedMapState();
                    if (stage4_) {
                        stage4_->showMapDownloadInProgress();
                        stage4_->setPlanningEnabled(false);
                        stage4_->setLaunchProgress(100, "Retrying corrected map download...");
                        stage4_->setLaunchDiagnostics(
                            buildExplorationDiagnostics(
                                QString("%1 Selected corrected map: %2. Retrying newest "
                                        "corrected-map lookup under /R_DATA.")
                                    .arg(reason, remote_map_path)));
                    }
                    QTimer::singleShot(
                        kCorrectedMapRetryDelayMs,
                        this,
                        [this, retries_remaining]() {
                            beginCorrectedMapResolveAndDownload(retries_remaining - 1);
                        });
                    return;
                }

                failSavedMapWorkflow(
                    "Corrected map download failed.",
                    QString("%1 Selected corrected map: %2").arg(reason, remote_map_path));
            });
    connect(saved_map_download_proc_,
            &QProcess::errorOccurred,
            this,
            [this](QProcess::ProcessError) {
                const QString remote_map_path = pending_saved_map_remote_path_;
                const int retries_remaining = pending_saved_map_retry_remaining_;
                if (!map_save_or_download_in_progress_ && remote_map_path.isEmpty() &&
                    pending_saved_map_local_path_.isEmpty()) {
                    return;
                }

                const QString reason =
                    QString("Corrected map download error: %1")
                        .arg(saved_map_download_proc_->errorString());
                if (retries_remaining > 0) {
                    clearPendingSavedMapState();
                    if (stage4_) {
                        stage4_->showMapDownloadInProgress();
                        stage4_->setPlanningEnabled(false);
                        stage4_->setLaunchProgress(100, "Retrying corrected map download...");
                        stage4_->setLaunchDiagnostics(
                            buildExplorationDiagnostics(
                                QString("%1 Selected corrected map: %2. Retrying newest "
                                        "corrected-map lookup under /R_DATA.")
                                    .arg(reason, remote_map_path)));
                    }
                    QTimer::singleShot(
                        kCorrectedMapRetryDelayMs,
                        this,
                        [this, retries_remaining]() {
                            beginCorrectedMapResolveAndDownload(retries_remaining - 1);
                        });
                    return;
                }

                failSavedMapWorkflow(
                    "Corrected map download process error.",
                    QString("%1 Selected corrected map: %2").arg(reason, remote_map_path));
            });

    exploration_launch_poll_timer_ = new QTimer(this);
    exploration_launch_poll_timer_->setInterval(500);
    connect(exploration_launch_poll_timer_,
            &QTimer::timeout,
            this,
            &AppShellWindow::onExplorationLaunchPoll);

    exploration_live_fast_timer_ = new QTimer(this);
    exploration_live_fast_timer_->setInterval(50);  // 20 Hz live speed/nav updates
    connect(exploration_live_fast_timer_,
            &QTimer::timeout,
            this,
            &AppShellWindow::onExplorationLiveFastTick);
    exploration_live_fast_timer_->start();

    exploration_live_slow_timer_ = new QTimer(this);
    exploration_live_slow_timer_->setInterval(1000);  // 1 Hz card/status updates
    connect(exploration_live_slow_timer_,
            &QTimer::timeout,
            this,
            &AppShellWindow::onExplorationLiveSlowTick);
    exploration_live_slow_timer_->start();

    exploration_rf_probe_proc_ = new QProcess(this);
    exploration_rf_probe_proc_->setProcessChannelMode(QProcess::MergedChannels);
    connect(exploration_rf_probe_proc_,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            &AppShellWindow::handleExplorationRfProbeFinished);
    connect(exploration_rf_probe_proc_,
            &QProcess::errorOccurred,
            this,
            [this](QProcess::ProcessError) { exploration_rf_probe_in_flight_ = false; });

    exploration_storage_probe_proc_ = new QProcess(this);
    exploration_storage_probe_proc_->setProcessChannelMode(QProcess::MergedChannels);
    connect(exploration_storage_probe_proc_,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            &AppShellWindow::handleExplorationStorageProbeFinished);

    setupWindowControls();
    setDarkMode(dark_mode_);
    const QString planner_autotest_map = qEnvironmentVariable("BDR_PLANNER_AUTOTEST_MAP").trimmed();
    if (!planner_autotest_map.isEmpty()) {
        std::cout << "PLANNER_AUTOTEST phase=app_shell_boot map=\""
                  << planner_autotest_map.toStdString() << "\"" << std::endl;
        latest_saved_map_local_path_ = planner_autotest_map;
        const QString planner_autotest_robot =
            qEnvironmentVariable("BDR_PLANNER_AUTOTEST_ROBOT").trimmed();
        if (!planner_autotest_robot.isEmpty()) {
            robot_id_ = planner_autotest_robot;
        } else if (robot_id_.isEmpty()) {
            robot_id_ = settings.value("robot_id",
                                       settings.value("setup/robot_id", QStringLiteral("AutotestRobot")))
                           .toString();
        }
        QTimer::singleShot(0, this, [this]() { goToStage5(); });
    }
    // BDR_REWIRE: dev-only screenshot hook. When BDR_DEV_START_AT_SCAN=1, jump
    // straight to PlannerScreen Stage-4 (Scan) on boot so the UI can be
    // verified without clicking through Stages 1-3 + the planner sub-stages.
    // No real map / waypoints are loaded — the Scan page renders in an empty
    // state, which is exactly what we want for visual diff against Figma.
    if (qEnvironmentVariable("BDR_DEV_START_AT_SCAN").trimmed() == QStringLiteral("1")) {
        if (robot_id_.isEmpty()) {
            robot_id_ = QStringLiteral("DevScreenshotBot");
        }
        QTimer::singleShot(0, this, [this]() {
            goToStage5();
            if (stage5_) {
                stage5_->devForceJumpToScanStep();
            }
        });
    }
    qApp->installEventFilter(this);

    // Phase 9: emit bootHealthy on the next event-loop tick. Using
    // QTimer::singleShot(0, ...) instead of an immediate emit means the
    // signal only fires after the event loop has at least started
    // dispatching events. That makes "ctor returned but app is wedged"
    // (e.g. main thread spinning, no events processed) classify as
    // unhealthy → watchdog times out → rollback. Cost: ~1 ms.
    QTimer::singleShot(0, this, [this]() {
        update::log::info("appshell",
                          QStringLiteral("bootHealthy: emitted"));
        emit bootHealthy();
    });
}

void AppShellWindow::showRolledBackBanner(const QString& message) {
    if (!central_root_) {
        update::log::warn(
            "appshell",
            QStringLiteral("showRolledBackBanner: central_root_ null, ignoring"));
        return;
    }
    if (!rollback_banner_) {
        rollback_banner_ = new RollbackBanner(central_root_);
        rollback_banner_->setDarkMode(dark_mode_);

        // Insert as the FIRST child of the central root layout so it
        // sits above both the OTA "update available" banner (when both
        // are visible — rare but possible) and the stage stack.
        auto* root_layout =
            qobject_cast<QVBoxLayout*>(central_root_->layout());
        if (root_layout) {
            auto* host = new QWidget(central_root_);
            auto* host_lay = new QHBoxLayout(host);
            host_lay->setContentsMargins(12, 8, 12, 0);
            host_lay->setSpacing(0);
            host_lay->addWidget(rollback_banner_);
            root_layout->insertWidget(0, host);
        }

        connect(rollback_banner_, &RollbackBanner::dismissRequested,
                this, [this]() {
                    update::log::info(
                        "appshell",
                        QStringLiteral("rollback banner: dismissed"));
                    update::clearUpdateState();
                    if (rollback_banner_ && rollback_banner_->parentWidget()) {
                        // Hide the host wrapper so it doesn't keep
                        // taking layout space after dismiss.
                        rollback_banner_->parentWidget()->hide();
                    }
                });
    }
    rollback_banner_->setMessage(message);
    rollback_banner_->setDarkMode(dark_mode_);
    if (rollback_banner_->parentWidget()) {
        rollback_banner_->parentWidget()->show();
    }
    rollback_banner_->show();
    update::log::info("appshell",
                      QStringLiteral("rollback banner: shown"));
}

AppShellWindow::~AppShellWindow() {
    if (exploration_launch_poll_timer_) {
        exploration_launch_poll_timer_->stop();
    }
    if (stage4_) {
        stage4_->forceTeleopStop();
        stage4_->stopFpvStream();
    }
    if (laptop_launch_proc_ && laptop_launch_proc_->state() != QProcess::NotRunning) {
        laptop_launch_proc_->terminate();
        if (!laptop_launch_proc_->waitForFinished(2000)) {
            laptop_launch_proc_->kill();
            laptop_launch_proc_->waitForFinished(1000);
        }
    }
    if (robot_launch_proc_ && robot_launch_proc_->state() != QProcess::NotRunning) {
        robot_launch_proc_->terminate();
        if (!robot_launch_proc_->waitForFinished(2000)) {
            robot_launch_proc_->kill();
            robot_launch_proc_->waitForFinished(1000);
        }
    }
    if (saved_map_download_proc_ &&
        saved_map_download_proc_->state() != QProcess::NotRunning) {
        saved_map_download_proc_->terminate();
        if (!saved_map_download_proc_->waitForFinished(1500)) {
            saved_map_download_proc_->kill();
            saved_map_download_proc_->waitForFinished(500);
        }
    }
    if (exploration_rf_probe_proc_ &&
        exploration_rf_probe_proc_->state() != QProcess::NotRunning) {
        exploration_rf_probe_proc_->terminate();
        if (!exploration_rf_probe_proc_->waitForFinished(800)) {
            exploration_rf_probe_proc_->kill();
            exploration_rf_probe_proc_->waitForFinished(300);
        }
    }
    if (exploration_storage_probe_proc_ &&
        exploration_storage_probe_proc_->state() != QProcess::NotRunning) {
        exploration_storage_probe_proc_->terminate();
        if (!exploration_storage_probe_proc_->waitForFinished(1200)) {
            exploration_storage_probe_proc_->kill();
            exploration_storage_probe_proc_->waitForFinished(300);
        }
    }
}

void AppShellWindow::onLoginSubmitted(const QString& robotId, const QString& accessCode) {
    robot_id_ = robotId;
    access_code_ = accessCode;
    ensureStage2();
    if (stage2_) {
        stage2_->resetState();
        stage2_->setRobotId(robot_id_);
    }
    goToStage2();
}

void AppShellWindow::goToStage1() {
    stack_->setCurrentWidget(stage1_);
}

void AppShellWindow::goToStage2() {
    ensureStage2();
    if (!stage2_) {
        return;
    }
    stage2_->setRobotId(robot_id_);
    stack_->setCurrentWidget(stage2_);
}

void AppShellWindow::goToStage3() {
    ensureStage3();
    if (!stage3_) {
        return;
    }
    if (stage4_) {
        stage4_->forceTeleopStop();
        stage4_->stopFpvStream();
    }
    stage3_->setRobotId(robot_id_);
    // Forward the latest Stage 2 preflight rollup so the Stage 3 System
    // Status card can fold it into its READY / WARNING / NOT READY
    // calculation. Empty string when no report has been parsed yet
    // (DashboardScreen treats this as neutral, not a degrade).
    if (stage2_) {
        stage3_->setPreflightResult(stage2_->preflightResult());
    }
    stack_->setCurrentWidget(stage3_);
}

void AppShellWindow::goToStage4() {
    ensureStage4();
    if (!stage4_) {
        return;
    }
    if (!exploration_rf_config_ready_) {
        loadExplorationRfConfigForActiveRobot();
    }
    stack_->setCurrentWidget(stage4_);
    if (stream_status_ready_ && !fpv_started_) {
        stage4_->startFpvStream(5600);
        fpv_started_ = true;
    }
    pushExplorationTopMotorsChipState();
    // Seed the top-bar battery pill with whatever sample we already
    // have from the dashboard's MQTT subscriber — otherwise the pill
    // would stay at "—%" until the dashboard's NEXT publish, which
    // could be ≥1 s away depending on when the operator transitioned.
    stage4_->setTopBatteryState(last_battery_pct_, last_battery_stale_);
}

void AppShellWindow::goToStage5() {
    ensureStage5();
    if (!stage5_) {
        return;
    }
    if (qEnvironmentVariableIsSet("BDR_PLANNER_AUTOTEST_MAP")) {
        std::cout << "PLANNER_AUTOTEST phase=go_to_stage5 map=\""
                  << latest_saved_map_local_path_.toStdString() << "\""
                  << " robot=\"" << robot_id_.toStdString() << "\"" << std::endl;
    }
    if (stage4_) {
        stage4_->forceTeleopStop();
        stage4_->stopFpvStream();
        fpv_started_ = false;
    }
    stage5_->setRobotId(robot_id_);
    stage5_->setMapPath(latest_saved_map_local_path_);
    planner_estop_active_ = false;
    pushPlannerTelemetrySnapshot();
    // Seed the Stage 5 top-bar battery pill — same reasoning as Stage
    // 4 (avoid a "—%" gap during the transition; the dashboard's
    // last sample is still authoritative).
    stage5_->setTopBatteryState(last_battery_pct_, last_battery_stale_);
    stack_->setCurrentWidget(stage5_);
    pushExplorationTelemetryToUiSlow();
    // No SetParameters client warmup — sendControllerMaxLinearVelocity now
    // creates a fresh client per call (see comment there for why).
}

void AppShellWindow::onThemeToggleChanged(bool dark_mode) {
    setDarkMode(dark_mode);
    QSettings settings(f2c_cpp::kSettingsOrgName, f2c_cpp::kSettingsAppName);
    settings.setValue("ui/dark_mode", dark_mode_);
}


void AppShellWindow::setDarkMode(bool dark_mode) {
    dark_mode_ = dark_mode;
    if (update_banner_) {
        update_banner_->setDarkMode(dark_mode_);
    }
    if (rollback_banner_) {
        rollback_banner_->setDarkMode(dark_mode_);
    }
    if (stage1_) {
        stage1_->setDarkMode(dark_mode_);
    }
    if (stage2_) {
        stage2_->setDarkMode(dark_mode_);
    }
    if (window_theme_toggle_) {
        window_theme_toggle_->setChecked(dark_mode_);
        updateWindowControlsToggleUi();
    }
    updateWindowControlsTheme();
    if (stage3_) {
        stage3_->setDarkMode(dark_mode_);
    }
    if (stage4_) {
        stage4_->setDarkMode(dark_mode_);
    }
    if (stage5_) {
        stage5_->setDarkMode(dark_mode_);
    }
}

void AppShellWindow::showUpdateModal(const update::VersionInfo& info) {
    UpdateModal::GateState gate;
    // Active mission proxy: the operator is on Stage 5 (PlannerScreen).
    // Phase 6 has no granular `isMissionActive()` accessor on PlannerScreen
    // yet — using stage equality is intentionally conservative: the modal
    // refuses Install Now whenever the planner is foregrounded, which is
    // exactly what the locked spec ("update will only pop up when there is
    // internet — if any other process is happening the update wont start")
    // demands. Phase 7 can tighten this once PlannerScreen exposes a real
    // mission-state predicate.
    gate.has_active_mission = (stack_ && stage5_ &&
                               stack_->currentWidget() == stage5_);
    gate.has_active_transfer = TransferManager::instance().hasActiveTransfer();
    gate.has_active_upload = CloudUploadManager::instance().hasActiveUpload();
    gate.battery_pct = UpdateModal::readBatteryPercent();

    auto* modal = new UpdateModal(info, gate, dark_mode_, this);
    modal->setAttribute(Qt::WA_DeleteOnClose);

    connect(modal, &UpdateModal::remindMeLaterRequested, this,
            [this](const update::VersionInfo& vi) {
                Q_UNUSED(vi);
                if (!update_checker_) return;
                const qint64 until_ms =
                    QDateTime::currentMSecsSinceEpoch() +
                    update::kSnoozeDurationMs;
                update_checker_->setSnoozedUntil(until_ms);
                update::log::info(
                    "appshell",
                    QStringLiteral("snooze 4h: until_ms=%1")
                        .arg(until_ms));
                if (update_banner_) {
                    update_banner_->hide();
                }
            });

    // Phase 7: spawn the external bdr-update-runner, wait for it to
    // acquire its lockfile (proves the runner window is up — Q3=B), then
    // quit the OCU. Locked Q1=A: CLI args; Q2=B: shared bdr_update_core
    // lib backs both processes; concern #2: runner takes flock on startup;
    // concern #3: OCU polls for the lockfile being held before quitting.
    connect(modal, &UpdateModal::installRequested, this,
            [this, modal](const update::VersionInfo& vi) {
                update::log::info(
                    "appshell",
                    QStringLiteral("install requested: tag=%1 sha=%2 "
                                   "size=%3")
                        .arg(vi.tag).arg(vi.commitSha).arg(vi.sizeBytes));
                handoffToUpdateRunner(vi, modal);
            });

    modal->show();
    modal->raise();
    modal->activateWindow();
}

void AppShellWindow::handoffToUpdateRunner(const update::VersionInfo& info,
                                           QWidget* modal_window) {
    // Resolve runner binary path. In a deployed install the runner lives
    // next to the OCU at /usr/bin/bdr-update-runner (.deb staging in Phase
    // 7 places it there). For dev builds we accept it sitting in the same
    // directory as the OCU's argv[0]; main.cpp captures applicationDirPath
    // via QCoreApplication. Fall back to PATH lookup if neither exists.
    const QString ocu_binary = QCoreApplication::applicationFilePath();
    const QString ocu_dir = QFileInfo(ocu_binary).absolutePath();

    QString runner_path = ocu_dir + QStringLiteral("/bdr-update-runner");
    if (!QFileInfo::exists(runner_path)) {
        // Fall back to the .deb-installed location.
        runner_path = QStringLiteral("/usr/bin/bdr-update-runner");
    }
    if (!QFileInfo::exists(runner_path)) {
        update::log::error(
            "appshell",
            QStringLiteral("bdr-update-runner not found at %1 or /usr/bin")
                .arg(ocu_dir));
        BdrMessageBox::warning(
            modal_window ? modal_window : this,
            QStringLiteral("Update unavailable"),
            QStringLiteral(
                "The update installer (bdr-update-runner) is missing from "
                "this build. Please rebuild and try again."));
        return;
    }

    // Build CLI args (locked Q1=A).
    QStringList runner_args = {
        QStringLiteral("--deb-url"),     info.downloadUrl,
        QStringLiteral("--sha256-url"),  info.sha256Url,
        QStringLiteral("--asset-name"),  info.assetName,
        QStringLiteral("--tag"),         info.tag,
        QStringLiteral("--commit-sha"),  info.commitSha,
        QStringLiteral("--size-bytes"),  QString::number(info.sizeBytes),
        QStringLiteral("--ocu-binary"),  ocu_binary,
    };
    if (dark_mode_) runner_args << QStringLiteral("--dark");

    update::log::info(
        "appshell",
        QStringLiteral("spawning runner: %1 (theme=%2)")
            .arg(runner_path).arg(dark_mode_ ? "dark" : "light"));

    qint64 runner_pid = 0;
    const bool spawned =
        QProcess::startDetached(runner_path, runner_args,
                                ocu_dir, &runner_pid);
    if (!spawned) {
        update::log::error(
            "appshell",
            QStringLiteral("QProcess::startDetached failed for %1")
                .arg(runner_path));
        BdrMessageBox::warning(
            modal_window ? modal_window : this,
            QStringLiteral("Update could not start"),
            QStringLiteral(
                "Failed to launch the update installer. Check that "
                "bdr-update-runner is installed and try again."));
        return;
    }
    update::log::info(
        "appshell",
        QStringLiteral("runner spawned, pid=%1; waiting for lockfile")
            .arg(runner_pid));

    // Lockfile poll loop (Q3=B, concern #3). 50 ms cadence, 2 s ceiling.
    // We use a single-shot timer chain rather than a busy loop so the OCU
    // event loop keeps draining (banner styling, modal close, etc).
    auto* poll = new QTimer(this);
    poll->setInterval(50);
    auto* deadline = new QTimer(this);
    deadline->setSingleShot(true);
    deadline->setInterval(2000);

    auto cleanup = [poll, deadline]() {
        poll->stop();
        deadline->stop();
        poll->deleteLater();
        deadline->deleteLater();
    };

    connect(poll, &QTimer::timeout, this,
            [this, cleanup, runner_pid]() {
                if (!update::isRunnerLockfileHeld()) return;
                update::log::info(
                    "appshell",
                    QStringLiteral(
                        "runner lockfile held by pid=%1 — quitting OCU")
                        .arg(runner_pid));
                cleanup();
                qApp->quit();
            });
    connect(deadline, &QTimer::timeout, this,
            [this, cleanup, modal_window]() {
                cleanup();
                update::log::error(
                    "appshell",
                    QStringLiteral(
                        "runner did not acquire lockfile within 2s; "
                        "aborting handoff"));
                if (modal_window) {
                    BdrMessageBox::warning(
                        modal_window,
                        QStringLiteral("Updater failed to start"),
                        QStringLiteral(
                            "The update installer launched but did not "
                            "respond. Please try again later."));
                }
            });

    poll->start();
    deadline->start();
}

void AppShellWindow::ensureStage2() {
    if (stage2_) {
        return;
    }
    stage2_ = new StartupScreen(this);
    stack_->addWidget(stage2_);
    connect(stage2_, &StartupScreen::backRequested, this, &AppShellWindow::goToStage1);
    connect(stage2_, &StartupScreen::continueRequested, this, &AppShellWindow::goToStage3);
    stage2_->setDarkMode(dark_mode_);
}

void AppShellWindow::ensureStage3() {
    if (stage3_) {
        return;
    }
    stage3_ = new DashboardScreen(this);
    stack_->addWidget(stage3_);
    connect(stage3_, &DashboardScreen::logoutRequested, this, &AppShellWindow::goToStage1);
    connect(stage3_, &DashboardScreen::runDiagnosticsRequested, this, &AppShellWindow::goToStage2);
    connect(stage3_, &DashboardScreen::startNewScanRequested, this, &AppShellWindow::onStartNewScan);
    // Mirror the dashboard's MQTT battery state onto the Stage 4 /
    // Stage 5 top-bar pills.  Dashboard owns the subprocess + JSON
    // parsing; AppShell only caches the latest sample and pushes it
    // to whichever stage is currently visible.  See
    // DashboardScreen::batterySocChanged for the contract.
    connect(stage3_, &DashboardScreen::batterySocChanged, this,
            &AppShellWindow::onDashboardBatteryStateChanged);
    stage3_->setDarkMode(dark_mode_);
    if (!robot_id_.isEmpty()) {
        stage3_->setRobotId(robot_id_);
    }
}

void AppShellWindow::ensureStage4() {
    if (stage4_) {
        return;
    }
    stage4_ = new ExplorationScreen(this);
    stack_->addWidget(stage4_);
    connect(stage4_, &ExplorationScreen::backRequested, this, &AppShellWindow::goToStage3);
    connect(stage4_, &ExplorationScreen::startScanRequested, this, &AppShellWindow::onExplorationStartScanRequested);
    connect(stage4_, &ExplorationScreen::finishSaveMapRequested, this, &AppShellWindow::onExplorationFinishSaveMapRequested);
    connect(stage4_, &ExplorationScreen::startPlanningRequested, this, &AppShellWindow::onExplorationStartPlanningRequested);
    connect(stage4_, &ExplorationScreen::stopPipelineRequested, this, &AppShellWindow::onExplorationStopPipelineRequested);
    connect(stage4_, &ExplorationScreen::teleopTwistRequested, this, &AppShellWindow::onExplorationTeleopTwistRequested);
    connect(stage4_, &ExplorationScreen::teleopArmRequested, this, &AppShellWindow::onExplorationTeleopArmRequested);
    connect(stage4_, &ExplorationScreen::teleopDisarmRequested, this, &AppShellWindow::onExplorationTeleopDisarmRequested);
    connect(stage4_, &ExplorationScreen::teleopGprPowerOffRequested, this, &AppShellWindow::onExplorationTeleopGprPowerOffRequested);
    connect(stage4_, &ExplorationScreen::cameraSelectRequested,
            this, &AppShellWindow::onExplorationCameraSelectRequested);
    stage4_->setDarkMode(dark_mode_);
    // Seed the pill from QSettings so the operator's last choice
    // shows as active before /stream_camera_status confirms (or in
    // the no-launch state where we have no confirmation).
    {
        QSettings settings(kSettingsOrgName, kSettingsAppName);
        const QString persisted =
            settings.value(kSettingsStreamingCameraKey,
                           QStringLiteral("left")).toString();
        stage4_->setStreamingCamera(persisted);
    }
}

void AppShellWindow::ensureStage5() {
    if (stage5_) {
        return;
    }

    stage5_ = new PlannerScreen(this);
    connect(stage5_, &PlannerScreen::backRequested, this, &AppShellWindow::goToStage4);
    connect(stage5_,
            &PlannerScreen::publishScanSegmentsRequested,
            this,
            &AppShellWindow::onPlannerPublishScanSegmentsRequested);
    connect(stage5_,
            &PlannerScreen::startScanSegmentsRequested,
            this,
            &AppShellWindow::onPlannerStartScanSegmentsRequested);
    connect(stage5_, &PlannerScreen::scanStartRequested, this, &AppShellWindow::onPlannerScanStartRequested);
    connect(stage5_, &PlannerScreen::scanPauseRequested, this, &AppShellWindow::onPlannerScanPauseRequested);
    connect(stage5_, &PlannerScreen::scanResumeRequested, this, &AppShellWindow::onPlannerScanResumeRequested);
    connect(stage5_, &PlannerScreen::wakeGprRequested, this, &AppShellWindow::onPlannerWakeGprRequested);
    connect(stage5_, &PlannerScreen::emergencyStopRequested, this, &AppShellWindow::onPlannerEmergencyStopRequested);
    connect(stage5_, &PlannerScreen::scanTeleopTwistRequested, this, &AppShellWindow::onExplorationTeleopTwistRequested);
    connect(stage5_, &PlannerScreen::scanTeleopArmRequested, this, &AppShellWindow::onExplorationTeleopArmRequested);
    connect(stage5_, &PlannerScreen::scanTeleopDisarmRequested, this, &AppShellWindow::onExplorationTeleopDisarmRequested);
    connect(stage5_, &PlannerScreen::scanTeleopGprPowerOffRequested, this, &AppShellWindow::onExplorationTeleopGprPowerOffRequested);
    connect(stage5_, &PlannerScreen::completeMissionRequested, this, &AppShellWindow::onPlannerCompleteMissionRequested);
    connect(stage5_, &PlannerScreen::cancelScanRequested, this, &AppShellWindow::onPlannerCancelScanRequested);
    connect(stage5_, &PlannerScreen::discardScanRequested, this, &AppShellWindow::onPlannerDiscardScanRequested);
    stage5_->setDarkMode(dark_mode_);
    stage5_->setRobotId(robot_id_);
    stage5_->setMapPath(latest_saved_map_local_path_);
    pushPlannerTelemetrySnapshot();
    stack_->addWidget(stage5_);
}

// ---------------------------------------------------------------------------
// Scan workflow routing
// ---------------------------------------------------------------------------

void AppShellWindow::onStartNewScan() {
    // Intercept the Dashboard "Start New Scan" CTA with the New Scan
    // Information modal (Building / Operator / Units). On Cancel or X
    // close, the operator stays on the Dashboard with no transition and
    // no robot contact. On Proceed, MissionMetadataDialog has already
    // persisted the values to QSettings and updated UnitsProvider — we
    // just advance to Stage 4. The OCU never calls /dc/start itself —
    // the autonomous controller fires it after Stage 5 Planner Start
    // Scan publishes autonomy_enable=true. The metadata is pushed to
    // /data_collection_coordinator/set_parameters by
    // sendDataCollectorSessionMetadata in onPlannerScanStartRequested,
    // gated so autonomy_enable cannot fire if the push fails.
    //
    // Backdrop blur on the Dashboard mirrors the
    // PlannerScreen::showScanPreflightDialog pattern in
    // planner_screen.cpp:5202 so the operator's eye is drawn to the
    // modal.
    QWidget* backdrop = nullptr;
    if (stage3_) {
        backdrop = stage3_;
    } else if (stack_) {
        backdrop = stack_->currentWidget();
    }

    QGraphicsBlurEffect* blur = nullptr;
    if (backdrop) {
        blur = new QGraphicsBlurEffect(backdrop);
        blur->setBlurRadius(14.0);
        backdrop->setGraphicsEffect(blur);
    }

    auto* dialog = new MissionMetadataDialog(this);
    QPointer<MissionMetadataDialog> guard(dialog);
    QTimer::singleShot(0, dialog, [this, guard]() {
        if (!guard) return;
        const QPoint top_left = mapToGlobal(QPoint(
            (width() - guard->width()) / 2,
            (height() - guard->height()) / 2));
        guard->move(top_left);
    });

    const int rc = dialog->exec();

    if (backdrop) {
        backdrop->setGraphicsEffect(nullptr);
    }
    dialog->deleteLater();

    if (rc != QDialog::Accepted) {
        return;
    }

    goToStage4();
}

void AppShellWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    updateWindowControlsPosition();
    if (command_toast_ && command_toast_->isVisible()) {
        if (QWidget* host = command_toast_->parentWidget()) {
            const int x = host->width() - command_toast_->width() - 24;
            command_toast_->move(std::max(24, x), command_toast_->y());
        }
    }
}

void AppShellWindow::closeEvent(QCloseEvent* event) {
    // Are we mid-launch?  Only gate close if the launch has been started
    // (Stage 4 entered and Start Mapping clicked, OR Stage 5 entered with
    // active scan).  Stage 1-3 close paths skip the dialog: there is
    // nothing on the robot to tear down.  Per operator: "after Start
    // Mapping the close must be gated".
    const bool launch_active =
        exploration_launch_in_progress_ ||
        exploration_launch_ready_ ||
        laptop_launch_started_ ||
        robot_launch_started_;
    if (!launch_active) {
        QMainWindow::closeEvent(event);
        return;
    }

    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("Close BDR Coverage Planner?"));
    box.setIcon(QMessageBox::Warning);
    box.setText(QStringLiteral("A scan/launch is currently active on the robot."));
    box.setInformativeText(QStringLiteral(
        "Closing the OCU will tear down the robot launch (data collector, "
        "controllers, mapping). Any unfinished scan section will be lost.\n\n"
        "Continue?"));
    auto* close_btn = box.addButton(QStringLiteral("Close OCU"), QMessageBox::AcceptRole);
    auto* cancel_btn = box.addButton(QStringLiteral("Cancel"), QMessageBox::RejectRole);
    box.setDefaultButton(cancel_btn);
    box.exec();

    if (box.clickedButton() != close_btn) {
        event->ignore();
        return;
    }

    // SYNCHRONOUS teardown — same path as Complete Mission.  The new
    // teardown script polls for unified_data_collector PID exit before
    // returning (commit 283ac26), so by the time this returns the
    // robot's UDC has had a chance to call seekcamera_manager_destroy()
    // and we won't wedge the Seek SDK across OCU restarts.
    qInfo("[AppShell] closeEvent accepted: running synchronous launch teardown before quit");
    performExplorationPipelineTeardown();

    QMainWindow::closeEvent(event);
}

bool AppShellWindow::eventFilter(QObject* obj, QEvent* event) {
    auto* widget = qobject_cast<QWidget*>(obj);
    if (!widget || widget->window() != this) {
        return QMainWindow::eventFilter(obj, event);
    }

    if (event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::MouseMove ||
        event->type() == QEvent::MouseButtonRelease) {
        if (qobject_cast<QAbstractButton*>(widget) || qobject_cast<QLineEdit*>(widget)) {
            return QMainWindow::eventFilter(obj, event);
        }

        auto* mouse = static_cast<QMouseEvent*>(event);
        const QPoint global_pos = mouse->globalPos();
        const QPoint local_pos = mapFromGlobal(global_pos);
        const bool in_drag_area = (local_pos.y() <= drag_height_);
        const bool over_controls =
            window_controls_ && window_controls_->geometry().contains(local_pos);

        if (event->type() == QEvent::MouseButtonPress) {
            if (mouse->button() == Qt::LeftButton && in_drag_area && !over_controls && !isMaximized()) {
                dragging_ = true;
                drag_offset_ = global_pos - frameGeometry().topLeft();
            }
        } else if (event->type() == QEvent::MouseMove) {
            if (dragging_ && (mouse->buttons() & Qt::LeftButton)) {
                move(global_pos - drag_offset_);
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            dragging_ = false;
        }
    }

    return QMainWindow::eventFilter(obj, event);
}

void AppShellWindow::setupWindowControls() {
    if (!central_root_) {
        return;
    }

    window_controls_ = new QWidget(central_root_);
    window_controls_->setObjectName("WindowControls");
    auto* controls_layout = new QHBoxLayout(window_controls_);
    controls_layout->setContentsMargins(0, 0, 0, 0);
    controls_layout->setSpacing(4);

    window_theme_host_ = new QWidget(window_controls_);
    window_theme_host_->setObjectName("WindowThemeToggleHost");
    window_theme_host_->setFixedSize(48, 24);

    window_theme_toggle_ = new QToolButton(window_theme_host_);
    window_theme_toggle_->setObjectName("WindowThemeToggle");
    window_theme_toggle_->setCheckable(true);
    window_theme_toggle_->setChecked(dark_mode_);
    window_theme_toggle_->setAutoRaise(true);
    window_theme_toggle_->setCursor(Qt::PointingHandCursor);
    window_theme_toggle_->setFixedSize(window_theme_host_->size());
    window_theme_toggle_->move(0, 0);

    window_theme_left_icon_ = new QLabel(window_theme_toggle_);
    window_theme_left_icon_->setFixedSize(12, 12);
    window_theme_left_icon_->move(6, 6);
    window_theme_left_icon_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    window_theme_left_icon_->setStyleSheet(QStringLiteral("background: transparent;"));
    window_theme_left_icon_->setPixmap(
        loadSvgPixmap(QStringLiteral(":/assets/missionplanner/window_theme_left.svg"), 12, 12));
    window_theme_left_icon_->raise();

    window_theme_right_icon_ = new QLabel(window_theme_toggle_);
    window_theme_right_icon_->setFixedSize(12, 12);
    window_theme_right_icon_->move(30, 6);
    window_theme_right_icon_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    window_theme_right_icon_->setStyleSheet(QStringLiteral("background: transparent;"));
    window_theme_right_icon_->setPixmap(
        loadSvgPixmap(QStringLiteral(":/assets/missionplanner/window_theme_right.svg"), 12, 12));
    window_theme_right_icon_->raise();

    window_theme_knob_ = new QWidget(window_theme_host_);
    window_theme_knob_->setObjectName("WindowThemeToggleKnob");
    window_theme_knob_->setFixedSize(16, 16);
    window_theme_knob_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    updateWindowControlsToggleUi();

    connect(window_theme_toggle_, &QToolButton::clicked, this, &AppShellWindow::onWindowThemeToggleClicked);

    controls_layout->addWidget(window_theme_host_);
    controls_layout->addSpacing(32);

    window_minimize_ = new QToolButton(window_controls_);
    window_minimize_->setObjectName("WindowMinimize");
    window_minimize_->setText(QString());
    window_minimize_->setAutoRaise(true);
    window_minimize_->setIcon(QIcon(loadSvgPixmap(
        QStringLiteral(":/assets/missionplanner/window_minimize.svg"), 16, 16)));
    window_minimize_->setIconSize(QSize(16, 16));
    window_minimize_->setFixedSize(32, 32);
    window_minimize_->setCursor(Qt::PointingHandCursor);
    connect(window_minimize_, &QToolButton::clicked, this, &AppShellWindow::onWindowMinimizeClicked);
    controls_layout->addWidget(window_minimize_);

    window_maximize_ = new QToolButton(window_controls_);
    window_maximize_->setObjectName("WindowMaximize");
    window_maximize_->setText(QString());
    window_maximize_->setAutoRaise(true);
    window_maximize_->setIcon(QIcon(loadSvgPixmap(
        QStringLiteral(":/assets/missionplanner/window_maximize.svg"), 14, 14)));
    window_maximize_->setIconSize(QSize(14, 14));
    window_maximize_->setFixedSize(32, 32);
    window_maximize_->setCursor(Qt::PointingHandCursor);
    connect(window_maximize_, &QToolButton::clicked, this, &AppShellWindow::onWindowMaximizeClicked);
    controls_layout->addWidget(window_maximize_);

    window_close_ = new QToolButton(window_controls_);
    window_close_->setObjectName("WindowClose");
    window_close_->setText(QString());
    window_close_->setAutoRaise(true);
    window_close_->setIcon(QIcon(loadSvgPixmap(
        QStringLiteral(":/assets/missionplanner/window_close.svg"), 16, 16)));
    window_close_->setIconSize(QSize(16, 16));
    window_close_->setFixedSize(32, 32);
    window_close_->setCursor(Qt::PointingHandCursor);
    connect(window_close_, &QToolButton::clicked, this, &AppShellWindow::onWindowCloseClicked);
    controls_layout->addWidget(window_close_);

    window_controls_->adjustSize();
    window_controls_->raise();

    updateWindowControlsTheme();
    updateWindowControlsPosition();
}

void AppShellWindow::updateWindowControlsPosition() {
    if (!window_controls_ || !central_root_) {
        return;
    }

    const int margin = 24;
    window_controls_->adjustSize();
    const int x = std::max(0, central_root_->width() - window_controls_->width() - margin);
    const int y = std::max(0, (53 - window_controls_->height()) / 2);
    window_controls_->move(x, y);
    window_controls_->raise();

}

void AppShellWindow::updateWindowControlsTheme() {
    if (!window_controls_) {
        return;
    }

    const QString toggle_bg = dark_mode_ ? "#3F3F46" : "#D4D4D8";
    const QString knob = dark_mode_ ? "#18181B" : "#FFFFFF";
    const QString hover = dark_mode_ ? "#27272A" : "#E4E4E7";
    const QString close_hover = dark_mode_ ? "#7F1D1D" : "#FECACA";

    window_controls_->setStyleSheet(QString(R"(
        #WindowControls {
            background: transparent;
        }
        #WindowThemeToggle {
            background: %1;
            border: none;
            border-radius: 12px;
        }
        #WindowThemeToggle:checked {
            background: %1;
        }
        #WindowThemeToggleKnob {
            background: %2;
            border-radius: 8px;
        }
        #WindowMinimize, #WindowMaximize, #WindowClose {
            background: transparent;
            border: none;
            border-radius: 4px;
            outline: none;
            padding: 0;
        }
        #WindowMinimize:hover, #WindowMaximize:hover {
            background: %3;
        }
        #WindowClose:hover {
            background: %4;
        }
    )")
                                     .arg(toggle_bg, knob, hover, close_hover));
}

void AppShellWindow::updateWindowControlsToggleUi() {
    if (!window_theme_toggle_ || !window_theme_knob_) {
        return;
    }
    const int pad = 4;
    const int knob_w = window_theme_knob_->width();
    const int knob_h = window_theme_knob_->height();
    const int y = (window_theme_toggle_->height() - knob_h) / 2;
    const int x = window_theme_toggle_->isChecked()
                      ? (window_theme_toggle_->width() - knob_w - pad)
                      : pad;
    window_theme_knob_->move(x, y);
    window_theme_knob_->raise();
}

void AppShellWindow::onWindowThemeToggleClicked() {
    if (!window_theme_toggle_) {
        return;
    }
    onThemeToggleChanged(window_theme_toggle_->isChecked());
    updateWindowControlsToggleUi();
}

void AppShellWindow::onWindowMinimizeClicked() {
    showMinimized();
}

void AppShellWindow::onWindowMaximizeClicked() {
    if (isMaximized()) {
        showNormal();
    } else {
        showMaximized();
    }
}

void AppShellWindow::onWindowCloseClicked() {
    close();
}

void AppShellWindow::onExplorationStartScanRequested() {
    ensureStage4();
    if (!stage4_) {
        return;
    }
    if (exploration_launch_in_progress_) {
        return;
    }
    if (exploration_rf_probe_proc_ &&
        exploration_rf_probe_proc_->state() != QProcess::NotRunning) {
        exploration_rf_probe_proc_->terminate();
        if (!exploration_rf_probe_proc_->waitForFinished(400)) {
            exploration_rf_probe_proc_->kill();
            exploration_rf_probe_proc_->waitForFinished(200);
        }
    }

    stage4_->forceTeleopStop();
    stage4_->stopFpvStream();
    stage4_->resetMappingWorkflowUi();
    stage4_->setPlanningEnabled(false);
    stage4_->beginMappingRunLock(60);
    stage4_->setLaunchReady(false);
    stage4_->setLaunchInProgress(true);
    stage4_->setLaunchProgress(0, "Initializing launch sequence...");
    stage4_->setTopSignalState("Signal unavailable", ExplorationScreen::ValueTone::Muted);
    stage4_->setThermalHidden();
    stage4_->resetNavigationMap();

    exploration_launch_in_progress_ = true;
    exploration_launch_ready_ = false;
    exploration_launch_failed_ = false;
    laptop_launch_started_ = false;
    robot_launch_started_ = false;
    laptop_launch_confirmed_ = false;
    robot_launch_confirmed_ = false;
    local_zenoh_ready_ = false;
    video_service_ready_ = false;
    odom_ready_ = false;
    stream_status_ready_ = false;
    stream_target_published_ = false;
    fpv_started_ = false;
    // Arm link monitor: from now until teardown, every ROS callback
    // stamps freshness; staleness > kStaleMaxMs (10 s) combined with
    // the reachability probe verdict resolves to RECONNECTING (probe
    // ok, topics stale) vs DISCONNECTED (probe + topics both gone).
    // See onLinkStateChanged.  The reachability probe is armed below,
    // after ssh_target is resolved (we need its host to know what to
    // probe).
    if (link_monitor_) {
        link_monitor_->arm();
    }
    link_disconnect_started_at_ms_ = 0;
    link_recovery_resync_pending_ = false;
    exploration_rf_link_latched_ = false;
    exploration_rf_metric_ready_ = false;
    exploration_rf_probe_in_flight_ = false;
    exploration_rf_rssi_dbm_ = 0;
    exploration_last_rf_probe_at_ms_ = 0;
    exploration_last_rf_success_at_ms_ = 0;
    last_launch_probe_at_ms_ = 0;
    last_stream_target_publish_at_ms_ = 0;
    stream_status_text_.clear();
    exploration_last_stream_status_at_ms_ = 0;
    exploration_last_thermal_thumb_at_ms_ = 0;
    exploration_last_thermal_summary_at_ms_ = 0;
    exploration_odom_samples_.clear();
    exploration_latest_x_m_ = 0.0;
    exploration_latest_y_m_ = 0.0;
    exploration_latest_z_m_ = 0.0;
    exploration_latest_vx_mps_ = 0.0;
    exploration_latest_vy_mps_ = 0.0;
    exploration_latest_yaw_rad_ = 0.0;
    exploration_thermal_thumb_ready_ = false;
    exploration_thermal_thumb_bytes_.clear();
    exploration_thermal_summary_ready_ = false;
    exploration_thermal_max_c_ = 0.0;
    exploration_thermal_avg_c_ = 0.0;
    exploration_thermal_min_c_ = 0.0;
    exploration_last_odom_at_ms_ = 0;
    exploration_last_local_nav_grid_at_ms_ = 0;
    exploration_local_nav_grid_ready_ = false;
    exploration_local_nav_grid_bytes_.clear();
    exploration_scan_started_at_ms_ = 0;
    exploration_left_iq_measured_ = 0.0;
    exploration_right_iq_measured_ = 0.0;
    exploration_left_iq_at_ms_ = 0;
    exploration_right_iq_at_ms_ = 0;
    exploration_motor_current_scan_samples_.clear();
    exploration_motor_spike_streak_ = 0;
    exploration_motor_warning_active_ = false;
    latest_map_save_cutoff_at_ms_ = 0;
    {
        QSettings settings(f2c_cpp::kSettingsOrgName, f2c_cpp::kSettingsAppName);
        settings.remove("planner/driven_path_cutoff_ms");
    }
    latest_saved_map_local_path_.clear();
    clearPendingSavedMapState();
    map_save_or_download_in_progress_ = false;
    exploration_storage_text_ = QStringLiteral("N/A");
    exploration_storage_ready_ = false;
    exploration_storage_probe_in_flight_ = false;
    exploration_last_storage_probe_at_ms_ = 0;
    loadExplorationRfConfigForActiveRobot();
    active_robot_host_.clear();
    active_robot_ssh_user_.clear();
    laptop_launch_last_output_.clear();
    robot_launch_last_output_.clear();
    exploration_launch_started_at_ms_ = QDateTime::currentMSecsSinceEpoch();
    loadPreviousScanMotorBaseline();

    ensureExplorationRosInterfaces();

    ResolvedRobotSshTarget ssh_target;
    QString resolve_err;
    if (!resolveRobotSshTargetFromSettings(&ssh_target, &resolve_err)) {
        setExplorationLaunchFailed(
            resolve_err.isEmpty()
                ? QStringLiteral(
                      "Could not resolve robot SSH target (complete setup login or set robot_ip).")
                : resolve_err);
        return;
    }
    active_robot_host_ = ssh_target.host;
    active_robot_ssh_user_ = ssh_target.ssh_user;
    // Arm the reachability probe on the same host the launch is
    // targeting — keeps probe + monitor in lock-step.  Layered
    // model: ICMP every 1 s with TCP-22 fallback so a Zenoh
    // peer-rediscovery hiccup (topics stale, host still up)
    // resolves to RECONNECTING instead of DISCONNECTED.
    if (reachability_probe_ && !ssh_target.host.isEmpty()) {
        reachability_probe_->arm(ssh_target.host);
    }
    startLaptopTeleopLaunch(ssh_target.host);
    startRobotCompleteLaunch(ssh_target);

    if (exploration_launch_poll_timer_ && !exploration_launch_poll_timer_->isActive()) {
        exploration_launch_poll_timer_->start();
    }
    updateExplorationProgressUi();
    stage4_->setLaunchDiagnostics(buildExplorationDiagnostics("Launch sequence started"));
}

void AppShellWindow::onExplorationFinishSaveMapRequested() {
    ensureStage4();
    if (!stage4_) {
        return;
    }
    if (!exploration_launch_ready_) {
        stage4_->setLaunchProgress(100, "Pipeline is not ready yet.");
        stage4_->setLaunchDiagnostics(
            buildExplorationDiagnostics("Finish blocked: pipeline not ready"));
        return;
    }
    if (map_save_or_download_in_progress_) {
        return;
    }
    if (!exploration_ros_node_) {
        ensureExplorationRosInterfaces();
    }
    if (!exploration_save_map_client_) {
        stage4_->setLaunchProgress(100, "Map save service unavailable.");
        stage4_->setPrimaryActionReadyToFinish();
        stage4_->setPlanningEnabled(false);
        stage4_->setLaunchDiagnostics(
            buildExplorationDiagnostics("Finish failed: /save_raw_map client missing"));
        return;
    }

    map_save_or_download_in_progress_ = true;
    latest_saved_map_local_path_.clear();
    clearPendingSavedMapState();
    stage4_->setPlanningEnabled(false);
    stage4_->showMapSaveInProgress();
    stage4_->setLaunchProgress(100, "Saving tilt-corrected map...");
    stage4_->setLaunchDiagnostics(
        buildExplorationDiagnostics("Calling /save_raw_map before selecting newest corrected map"));

    QString save_error;
    if (!requestSavedMapPathWithRetry(nullptr, &save_error)) {
        failSavedMapWorkflow("Tilt-corrected map save failed.", save_error);
        return;
    }

    persistCurrentScanMotorBaseline();

    // Save-time cutoff is captured from local receive time so later planner snapshotting
    // can ignore odom points collected after this map was finalized.
    latest_map_save_cutoff_at_ms_ = QDateTime::currentMSecsSinceEpoch();
    {
        QSettings settings(f2c_cpp::kSettingsOrgName, f2c_cpp::kSettingsAppName);
        settings.setValue("planner/driven_path_cutoff_ms", latest_map_save_cutoff_at_ms_);
    }

    beginCorrectedMapResolveAndDownload(kCorrectedMapRetryCount);
}

void AppShellWindow::onExplorationStartPlanningRequested() {
    if (latest_saved_map_local_path_.isEmpty()) {
        if (stage4_) {
            // TEMP(planner-preview): allow entering Mission Planner without a
            // downloaded tilt-corrected map so the planner UI can be reviewed.
            // Restore the blocking return after planner validation is complete.
            stage4_->setLaunchProgress(100, "Opening Mission Planner preview without a saved map.");
            stage4_->setLaunchDiagnostics(
                buildExplorationDiagnostics(
                    "Planner preview override active: proceeding without a downloaded "
                    "tilt-corrected map"));
        }
    }
    goToStage5();
}

void AppShellWindow::onExplorationTeleopTwistRequested(double linear_x, double angular_z) {
    publishExplorationTeleopTwist(linear_x, angular_z);
}

void AppShellWindow::onExplorationTeleopArmRequested() {
    sendExplorationAxisStateRequest(kOdriveAxisStateClosedLoopControl);
}

void AppShellWindow::onExplorationTeleopDisarmRequested() {
    sendExplorationAxisStateRequest(kOdriveAxisStateIdle);
}

void AppShellWindow::onExplorationTeleopGprPowerOffRequested() {
    sendExplorationGprPowerOffRequest();
}

void AppShellWindow::onPlannerPublishScanSegmentsRequested(const std::vector<double>& xy_pairs) {
    if (!exploration_ros_node_) {
        ensureExplorationRosInterfaces();
    }
    if (!planner_f2c_waypoints_pub_) {
        return;
    }
    if (xy_pairs.empty()) {
        return;
    }
    const bool pair_payload = (xy_pairs.size() % 2) == 0;
    const bool triple_payload = (xy_pairs.size() % 3) == 0;
    if (!pair_payload && !triple_payload) {
        return;
    }
    std_msgs::msg::Float64MultiArray msg;
    msg.data.assign(xy_pairs.begin(), xy_pairs.end());
    planner_f2c_waypoints_pub_->publish(msg);
}

void AppShellWindow::onPlannerStartScanSegmentsRequested(const QString& /*progression_mode*/) {
    if (!exploration_ros_node_) {
        ensureExplorationRosInterfaces();
    }
    if (!planner_f2c_waypoints_pub_) {
        return;
    }
    // The MPC controller interprets a single-element [0.0] payload as a
    // "start navigation" command on /f2c_waypoints.
    std_msgs::msg::Float64MultiArray msg;
    msg.data.push_back(0.0);
    planner_f2c_waypoints_pub_->publish(msg);
}

void AppShellWindow::publishPlannerAutonomyEnable(bool enabled) {
    if (!exploration_ros_node_) {
        ensureExplorationRosInterfaces();
    }
    if (!planner_mpc_autonomy_pub_) {
        return;
    }
    std_msgs::msg::Bool msg;
    msg.data = enabled;
    planner_mpc_autonomy_pub_->publish(msg);
}

void AppShellWindow::callPlannerTriggerService(
    const rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr& client) {
    if (!exploration_ros_node_) {
        ensureExplorationRosInterfaces();
    }
    if (!client || !client->service_is_ready()) {
        return;
    }
    auto request = std::make_shared<std_srvs::srv::Trigger::Request>();
    (void)client->async_send_request(request);
}

void AppShellWindow::onPlannerScanStartRequested(double speed_mps) {
    // GATE 0: refuse to arm a scan if the unified_data_collector is
    // dead or wedged.  isUdcDeadOrWedged() returns true ONLY for
    // DEAD_MAX_RESTARTS (supervisor gave up after 3 crashes in 60s)
    // or ERROR_THERMAL_WEDGED (Seek SDK never connected).  All other
    // states — STARTING / RECORDING / PAUSED / STOPPED / UNKNOWN —
    // are allowed (no false positives: per operator "very expensive").
    // If we don't gate here the operator gets a scan that records GPR
    // + rosbag + odom but ZERO visual frames, and won't notice until
    // post-mission inspection.
    if (isUdcDeadOrWedged()) {
        QMessageBox box(this);
        box.setWindowTitle(QStringLiteral("Cannot start scan"));
        box.setIcon(QMessageBox::Critical);
        if (udc_health_state_ == QStringLiteral("DEAD_MAX_RESTARTS")) {
            box.setText(QStringLiteral("Visual data collector failed to start."));
            box.setInformativeText(QStringLiteral(
                "unified_data_collector crashed 3 times in 60s on the robot. "
                "The supervisor has stopped trying to restart it.\n\n"
                "Required action: close the OCU, restart the robot launch, "
                "and reopen the OCU. If it crashes again, check the "
                "unified_data_collector logs on the robot."));
        } else {
            box.setText(QStringLiteral("Thermal camera SDK is wedged."));
            box.setInformativeText(QStringLiteral(
                "The Seek thermal SDK never connected after the last UDC "
                "start. Visual frames will be empty.\n\n"
                "Required action: physically unplug+replug the Seek "
                "thermal USB cable on the robot, then reopen the OCU."));
        }
        box.addButton(QStringLiteral("OK"), QMessageBox::AcceptRole);
        box.exec();
        qWarning("[AppShell] Start Scan blocked by /udc/health gate (state=%s)",
                 udc_health_state_.toUtf8().constData());
        return;
    }

    // Order of operations on planner Start Scan:
    //   1. Push New-Scan-Information metadata (building / operator /
    //      units) to /data_collection_coordinator via SetParameters,
    //      and HARD-BLOCK on its success. The controller's /dc/start
    //      fires shortly after publishPlannerAutonomyEnable(true)
    //      below; the coordinator re-reads these params on every
    //      ensure_mission_session(), so they MUST be live before
    //      autonomy is armed or the section folder lands under the
    //      BDR_test fallback (polluting the per-building AWS layout).
    //   2. On metadata success: push the operator-selected cruise
    //      speed (mutates the controller's max_linear_velocity
    //      synchronously inside its on_parameter_change), clear E-Stop
    //      if engaged, then publish autonomy_enable=true.
    //   3. On metadata failure: BdrMessageBox::warning was already
    //      shown by the helper; do NOT arm autonomy. Operator can
    //      retry by re-pressing Start Scan once the coordinator is
    //      reachable.
    QSettings settings(kSettingsOrgName, kSettingsAppName);
    const QString building_name =
        settings.value(kSettingsBuildingNameKey).toString();
    const QString operator_name =
        settings.value(kSettingsOperatorNameKey).toString();
    const QString units_preference =
        units::toString(UnitsProvider::instance()->units());

    sendDataCollectorSessionMetadata(
        building_name, operator_name, units_preference,
        [this, speed_mps](bool ok) {
            if (!ok) {
                return;
            }
            sendControllerMaxLinearVelocity(speed_mps);
            if (planner_estop_active_) {
                callPlannerTriggerService(planner_dc_resume_client_);
                sendExplorationAxisStateRequest(kOdriveAxisStateClosedLoopControl);
                planner_estop_active_ = false;
            }
            publishPlannerAutonomyEnable(true);
        });
}

void AppShellWindow::onPlannerScanPauseRequested() {
    if (isRobotLinkOffline()) {
        showCommandDroppedToast(
            QStringLiteral("Pause not delivered — robot offline."));
        return;
    }
    publishPlannerAutonomyEnable(false);
    callPlannerTriggerService(planner_dc_pause_client_);
}

void AppShellWindow::onPlannerScanResumeRequested() {
    if (isRobotLinkOffline()) {
        showCommandDroppedToast(
            QStringLiteral("Resume not delivered — robot offline."));
        return;
    }
    if (planner_estop_active_) {
        // Clear E-Stop in required order: resume data collection first, then arm.
        callPlannerTriggerService(planner_dc_resume_client_);
        sendExplorationAxisStateRequest(kOdriveAxisStateClosedLoopControl);
        planner_estop_active_ = false;
    } else {
        callPlannerTriggerService(planner_dc_resume_client_);
    }
    publishPlannerAutonomyEnable(true);
}

void AppShellWindow::onPlannerEmergencyStopRequested() {
    // Stage 2 disconnect-resilience guard: reject E-Stop press when we
    // know the link is TRULY offline (probe failed AND ROS topics
    // stale).  Otherwise the operator's press would silently flip
    // planner_estop_active_ even though the robot never received the
    // disarm — diverging the OCU's model from the robot's real state.
    //
    // E-STOP BYPASS: We deliberately use isRobotLinkUnreachable()
    // (strict) rather than isRobotLinkOffline() — E-Stop is the one
    // command where dropping it is unacceptable, so we let it fire in
    // SYNCING (camera/probe say bot is alive).  Even if the RPC lands
    // 5-10 s late after Zenoh re-handshakes, that's better than
    // refusing to send it at all.  See onLinkStateChanged.
    if (isRobotLinkUnreachable()) {
        showCommandDroppedToast(
            QStringLiteral("E-Stop not delivered — robot offline. State unchanged."));
        return;
    }
    if (!planner_estop_active_) {
        // Engage E-Stop: disarm first, then pause mission.
        sendExplorationAxisStateRequest(kOdriveAxisStateIdle);
        callPlannerTriggerService(planner_dc_pause_client_);
        publishPlannerAutonomyEnable(false);
        planner_estop_active_ = true;
        return;
    }

    // Clear E-Stop: resume first, then arm.
    callPlannerTriggerService(planner_dc_resume_client_);
    sendExplorationAxisStateRequest(kOdriveAxisStateClosedLoopControl);
    publishPlannerAutonomyEnable(true);
    planner_estop_active_ = false;
}

void AppShellWindow::onPlannerCompleteMissionRequested() {
    // Data-first branch (Stage 4 of disconnect-resilience).  When the
    // link is truly unreachable, the existing motor-disarm +
    // /dc/finalize_mission RPC chain will burn ~25 s of UI time on
    // dead RPCs and still leave the robot's mission_config.json with
    // mission_finalized_at: null.  The data IS on disk — the operator
    // just needs a way to land the finalize metadata.  Show the
    // OfflineFinalizeDialog and route accordingly.
    //
    // Note: gated on isRobotLinkUnreachable() (strict), NOT
    // isRobotLinkOffline() — RECONNECTING shouldn't pop the dialog
    // because the link might recover in seconds and the normal path
    // would have worked.  The Complete Mission button is itself
    // disabled during RECONNECTING by the screen-side gating, so the
    // operator can't actually reach this slot in that state anyway;
    // the strict check is a defence-in-depth measure.
    if (isRobotLinkUnreachable()) {
        qInfo("[AppShell] Complete Mission while link offline — showing OfflineFinalizeDialog");
        OfflineFinalizeDialog dialog(link_monitor_, this);
        if (central_root_) {
            dialog.move(central_root_->mapToGlobal(central_root_->rect().center())
                            - dialog.rect().center());
        }
        const int rc = dialog.exec();
        switch (dialog.chosen()) {
            case OfflineFinalizeDialog::Choice::Cancelled:
                qInfo("[AppShell] Complete Mission cancelled by operator (link still offline)");
                return;
            case OfflineFinalizeDialog::Choice::WaitReconnected:
                qInfo("[AppShell] Complete Mission: link recovered while waiting — running normal path");
                executeCompleteMissionNormalPath();
                return;
            case OfflineFinalizeDialog::Choice::FinalizeOverSsh:
                qInfo("[AppShell] Complete Mission: finalize via SSH (rc=%d)", rc);
                executeCompleteMissionSshFallback();
                return;
        }
        return;
    }
    executeCompleteMissionNormalPath();
}

void AppShellWindow::executeCompleteMissionNormalPath() {
    qInfo("[AppShell] Complete Mission (normal path) \u2014 disarming motors before pipeline teardown");

    beginExplorationMotorsIdleWait([this](bool timed_out) {
        if (timed_out) {
            qWarning("[AppShell] Complete Mission: motor disarm not confirmed within 2s \u2014 proceeding with finalize anyway");
        } else {
            qInfo("[AppShell] Complete Mission: motors confirmed IDLE \u2014 finalizing mission GNSS before teardown");
        }
        finalizeMissionDataCollection([this]() {
            performExplorationPipelineTeardown();
            planner_estop_active_ = false;
            goToStage3();
        });
    });
}

void AppShellWindow::executeCompleteMissionSshFallback() {
    // SSH-offline finalize.  Two phases:
    //   1. Run finalize_mission_local.py on the robot to land
    //      mission_finalized_at + finalized_via=ssh_offline into the
    //      open mission's mission_config.json.  This is independent
    //      of ROS / Zenoh; works as long as we can SSH in.
    //   2. Run the existing pipeline teardown.  It SSHes again to
    //      kill the launch tree (which sends ODrive IDLE on shutdown
    //      via the controller's exit handlers, so motors disarm
    //      naturally) and tears down the local laptop processes.
    //
    // Both phases are best-effort: if the SSH script fails (e.g. host
    // unreachable), we still proceed with teardown so the OCU returns
    // to Stage 3 and the operator isn't trapped.  The robot-side
    // 10-min auto-finalize watchdog is the safety net for that case.
    const ResolvedRobotSshTarget ssh_target = resolveRobotSshForRemoteOps();
    const QString robot_host = ssh_target.host;

    if (!robot_host.isEmpty()) {
        QProcess proc;
        QStringList args;
        // IMPORTANT: invoke the installed Python script directly via
        // python3 — NOT via `ros2 run pilot_control ...` — because
        // non-interactive SSH sessions don't source
        // /opt/ros/humble/setup.bash + the workspace overlay, so
        // `ros2` is not on PATH and the previous form failed with
        // rc=127 ("ros2: command not found").  finalize_mission_local
        // is pure file I/O (no rclpy import), so we don't need ROS
        // sourcing at all.  See the script's docstring.
        const QString script_path =
            QStringLiteral("/home/%1/pilot_ws/install/pilot_control/lib/pilot_control/finalize_mission_local.py")
                .arg(ssh_target.ssh_user);
        args << "-o" << "ConnectTimeout=4"
             << "-o" << "StrictHostKeyChecking=no"
             << "-o" << "UserKnownHostsFile=/dev/null"
             << "-o" << "BatchMode=yes"
             << sshUserHostSpec(ssh_target)
             << QStringLiteral("python3 %1").arg(script_path);
        qInfo("[AppShell] Complete Mission SSH: invoking finalize_mission_local.py on %s (path=%s)",
              robot_host.toUtf8().constData(),
              script_path.toUtf8().constData());
        proc.start("ssh", args);
        if (!proc.waitForFinished(8000)) {
            proc.kill();
            proc.waitForFinished(500);
            qWarning("[AppShell] Complete Mission SSH: finalize_mission_local.py timed out (>8s)");
        } else {
            const QByteArray out = proc.readAllStandardOutput().trimmed();
            const QByteArray err = proc.readAllStandardError().trimmed();
            const int rc = proc.exitCode();
            // Promote non-zero exit to a warning so failures are loud
            // in the log and easier to grep post-flight.  The previous
            // path silently logged rc=127 at info-level and the
            // operator never noticed the fallback didn't actually
            // finalize.
            if (rc == 0) {
                qInfo("[AppShell] Complete Mission SSH: rc=0 stdout=%s stderr=%s",
                      out.constData(),
                      err.constData());
            } else {
                qWarning("[AppShell] Complete Mission SSH: NON-ZERO rc=%d stdout=%s stderr=%s "
                         "\u2014 falling back to robot-side auto-finalize watchdog",
                         rc,
                         out.constData(),
                         err.constData());
            }
        }
    } else {
        qWarning("[AppShell] Complete Mission SSH: no robot_host resolved \u2014 skipping remote finalize");
    }

    // Skip the motor-idle wait + /dc/finalize_mission RPC chain (both
    // would just time out on a dead link).  Go straight to teardown.
    performExplorationPipelineTeardown();
    planner_estop_active_ = false;
    goToStage3();
}

void AppShellWindow::beginExplorationMotorsIdleWait(std::function<void(bool timed_out)> continuation) {
    // IDLE on all axes (left + right + GPR). Async on rclcpp; poll left/right
    // feedback until IDLE so the launch tree stays alive long enough for the
    // request to reach odrive_can.
    sendExplorationAxisStateRequest(kOdriveAxisStateIdle);

    if (exploration_motors_idle_wait_timer_ && exploration_motors_idle_wait_timer_->isActive()) {
        exploration_motors_idle_wait_timer_->stop();
    }
    exploration_motors_idle_wait_ticks_ = 0;

    auto finish = [this, continuation](bool timed_out) {
        if (exploration_motors_idle_wait_timer_) {
            exploration_motors_idle_wait_timer_->stop();
        }
        if (continuation) {
            continuation(timed_out);
        }
    };

    if (exploration_left_axis_state_ == kOdriveAxisStateIdle &&
        exploration_right_axis_state_ == kOdriveAxisStateIdle) {
        finish(/*timed_out=*/false);
        return;
    }

    if (!exploration_motors_idle_wait_timer_) {
        exploration_motors_idle_wait_timer_ = new QTimer(this);
        exploration_motors_idle_wait_timer_->setInterval(50);
        exploration_motors_idle_wait_timer_->setSingleShot(false);
    }

    disconnect(exploration_motors_idle_wait_timer_, &QTimer::timeout, nullptr, nullptr);
    connect(exploration_motors_idle_wait_timer_, &QTimer::timeout, this, [this, finish]() {
        ++exploration_motors_idle_wait_ticks_;
        if (exploration_left_axis_state_ == kOdriveAxisStateIdle &&
            exploration_right_axis_state_ == kOdriveAxisStateIdle) {
            finish(/*timed_out=*/false);
            return;
        }
        // 50 ms × 40 = 2000 ms hard ceiling.
        if (exploration_motors_idle_wait_ticks_ >= 40) {
            finish(/*timed_out=*/true);
        }
    });
    exploration_motors_idle_wait_timer_->start();
}

void AppShellWindow::publishExplorationTeleopTwist(double linear_x, double angular_z) {
    if (!exploration_ros_node_) {
        ensureExplorationRosInterfaces();
    }
    if (!exploration_cmd_vel_pub_) {
        return;
    }
    geometry_msgs::msg::Twist msg;
    msg.linear.x = linear_x;
    msg.angular.z = angular_z;
    exploration_cmd_vel_pub_->publish(msg);
}

void AppShellWindow::sendExplorationAxisStateRequest(int requested_state) {
    if (!exploration_ros_node_) {
        ensureExplorationRosInterfaces();
    }
    if (!exploration_ros_node_) {
        return;
    }

    // host_teleop.cpp does wait_for_service(1s) at startup, which forces
    // zenoh-ros2dds discovery to fully propagate for all three clients.
    // Our orchestrator never did that, so /right and /gpr discovery often
    // never resolved from this node's view — async_send_request would
    // silently no-op for those clients (rclcpp has no known service
    // server to route to). A brief per-call wait_for_service settles
    // discovery on first use; once cached, subsequent calls return
    // immediately so the UI stall is one-time and tiny.
    auto send_request = [this, requested_state](
                            const rclcpp::Client<odrive_can::srv::AxisState>::SharedPtr& client,
                            const char* axis_name) {
        if (!client) {
            return;
        }
        const QString axis_label = QString::fromLatin1(axis_name);
        const int target_state = requested_state;
        if (!client->wait_for_service(std::chrono::milliseconds(150))) {
            qWarning("[AppShell] %s axis: service not available (discovery did not settle in 150ms) — request dropped",
                     axis_label.toUtf8().constData());
            return;
        }
        auto request = std::make_shared<odrive_can::srv::AxisState::Request>();
        request->axis_requested_state = requested_state;
        (void)client->async_send_request(
            request,
            [axis_label, target_state](
                rclcpp::Client<odrive_can::srv::AxisState>::SharedFuture future) {
                try {
                    auto result = future.get();
                    qInfo("[AppShell] %s axis: state=%d errors=%d (requested %d)",
                          axis_label.toUtf8().constData(),
                          result->axis_state,
                          result->active_errors,
                          target_state);
                } catch (const std::exception& e) {
                    qWarning("[AppShell] %s axis request failed: %s",
                             axis_label.toUtf8().constData(),
                             e.what());
                }
            });
    };

    send_request(exploration_left_axis_client_, "left");
    send_request(exploration_right_axis_client_, "right");
    send_request(exploration_gpr_axis_client_, "gpr");
}

void AppShellWindow::sendExplorationGprPowerOffRequest() {
    if (!exploration_ros_node_) {
        ensureExplorationRosInterfaces();
    }
    if (!exploration_gpr_power_off_client_) {
        return;
    }
    if (!exploration_gpr_power_off_client_->service_is_ready()) {
        return;
    }
    auto request = std::make_shared<std_srvs::srv::Trigger::Request>();
    (void)exploration_gpr_power_off_client_->async_send_request(request);
}

void AppShellWindow::sendGprLineStopRequest() {
    if (!exploration_ros_node_) {
        ensureExplorationRosInterfaces();
    }
    if (!exploration_gpr_line_stop_client_) {
        return;
    }
    if (!exploration_gpr_line_stop_client_->service_is_ready()) {
        RCLCPP_WARN(rclcpp::get_logger("AppShellWindow"),
                    "/gpr_line_stop service not ready; skipping wake request.");
        return;
    }
    auto request = std::make_shared<std_srvs::srv::Trigger::Request>();
    (void)exploration_gpr_line_stop_client_->async_send_request(request);
}

void AppShellWindow::onPlannerWakeGprRequested() {
    sendGprLineStopRequest();
}

void AppShellWindow::finalizeMissionDataCollection(std::function<void()> done) {
    auto run_done = [this, done]() {
        planner_finalize_mission_in_flight_ = false;
        if (planner_finalize_mission_wait_timer_) {
            planner_finalize_mission_wait_timer_->stop();
        }
        if (done) {
            done();
        }
    };

    if (!exploration_ros_node_) {
        ensureExplorationRosInterfaces();
    }
    if (!planner_dc_finalize_mission_client_) {
        qWarning("[AppShell] /dc/finalize_mission client unavailable — skipping mission finalize");
        run_done();
        return;
    }
    if (planner_finalize_mission_in_flight_) {
        qInfo("[AppShell] /dc/finalize_mission already in flight — coalescing to single call");
        return;
    }

    // Discovery may not have settled if the operator hits Complete Mission
    // immediately after launch; give it a brief one-time wait. If the service
    // never appears (e.g. coordinator crashed), don't block teardown.
    if (!planner_dc_finalize_mission_client_->wait_for_service(std::chrono::milliseconds(250))) {
        qWarning("[AppShell] /dc/finalize_mission service not available — skipping mission finalize");
        run_done();
        return;
    }

    planner_finalize_mission_in_flight_ = true;
    planner_finalize_mission_wait_ticks_ = 0;
    qInfo("[AppShell] Sending /dc/finalize_mission to stop continuous GNSS and finalize mission_config.json");

    auto request = std::make_shared<std_srvs::srv::Trigger::Request>();
    (void)planner_dc_finalize_mission_client_->async_send_request(
        request,
        [this, run_done](rclcpp::Client<std_srvs::srv::Trigger>::SharedFuture future) {
            QMetaObject::invokeMethod(this, [this, run_done, future]() mutable {
                if (!planner_finalize_mission_in_flight_) {
                    return;  // 6 s ceiling already fired; nothing to do.
                }
                try {
                    auto result = future.get();
                    if (result && result->success) {
                        qInfo("[AppShell] /dc/finalize_mission OK: %s",
                              result->message.c_str());
                    } else {
                        qWarning("[AppShell] /dc/finalize_mission reported failure: %s",
                                 result ? result->message.c_str() : "(null)");
                    }
                } catch (const std::exception& e) {
                    qWarning("[AppShell] /dc/finalize_mission exception: %s", e.what());
                }
                run_done();
            }, Qt::QueuedConnection);
        });

    if (!planner_finalize_mission_wait_timer_) {
        planner_finalize_mission_wait_timer_ = new QTimer(this);
        planner_finalize_mission_wait_timer_->setInterval(50);
        planner_finalize_mission_wait_timer_->setSingleShot(false);
    }
    disconnect(planner_finalize_mission_wait_timer_, &QTimer::timeout, nullptr, nullptr);
    connect(planner_finalize_mission_wait_timer_, &QTimer::timeout, this, [this, run_done]() {
        ++planner_finalize_mission_wait_ticks_;
        // 50 ms × 120 = 6000 ms hard ceiling.
        if (planner_finalize_mission_wait_ticks_ >= 120) {
            qWarning("[AppShell] /dc/finalize_mission did not respond within 6s — proceeding with teardown anyway");
            run_done();
        }
    });
    planner_finalize_mission_wait_timer_->start();
}

void AppShellWindow::onPlannerCancelScanRequested() {
    qInfo("[AppShell] PlannerScreen::cancelScanRequested — aborting scan, deleting all mission data");

    // Phase 1: command IDLE on all axes so the bot doesn't keep moving
    // while the controller rips data off disk. The pipeline stays alive,
    // so the operator can re-arm later by simply starting a new mission.
    sendExplorationAxisStateRequest(kOdriveAxisStateIdle);
    publishPlannerAutonomyEnable(false);

    cancelActiveScanDataCollection([this](bool success) {
        // Always clear our local "estop latched" state so the planner can
        // exit the latched UI; PlannerScreen::notifyScanCancelled does the
        // segment-list / planned-path / stage navigation reset.
        planner_estop_active_ = false;
        if (stage5_) {
            stage5_->notifyScanCancelled(success);
        }
    });
}

void AppShellWindow::cancelActiveScanDataCollection(std::function<void(bool)> done) {
    auto run_done = [this, done](bool success) {
        planner_cancel_scan_in_flight_ = false;
        if (planner_cancel_scan_wait_timer_) {
            planner_cancel_scan_wait_timer_->stop();
        }
        if (done) {
            done(success);
        }
    };

    if (!exploration_ros_node_) {
        ensureExplorationRosInterfaces();
    }
    if (!planner_dc_cancel_scan_client_) {
        qWarning("[AppShell] /dc/cancel_scan client unavailable — proceeding with UI reset only");
        run_done(false);
        return;
    }
    if (planner_cancel_scan_in_flight_) {
        qInfo("[AppShell] /dc/cancel_scan already in flight — coalescing to single call");
        return;
    }

    // Brief discovery wait. The planner reaches this code only after a scan
    // has actually started, so the coordinator service should already be
    // discovered, but we keep parity with finalize for safety.
    if (!planner_dc_cancel_scan_client_->wait_for_service(std::chrono::milliseconds(250))) {
        qWarning("[AppShell] /dc/cancel_scan service not available — proceeding with UI reset only");
        run_done(false);
        return;
    }

    planner_cancel_scan_in_flight_ = true;
    planner_cancel_scan_wait_ticks_ = 0;
    qInfo("[AppShell] Sending /dc/cancel_scan to delete all mission data");

    auto request = std::make_shared<std_srvs::srv::Trigger::Request>();
    (void)planner_dc_cancel_scan_client_->async_send_request(
        request,
        [this, run_done](rclcpp::Client<std_srvs::srv::Trigger>::SharedFuture future) {
            QMetaObject::invokeMethod(this, [this, run_done, future]() mutable {
                if (!planner_cancel_scan_in_flight_) {
                    return;  // 8 s ceiling already fired; nothing to do.
                }
                bool success = false;
                try {
                    auto result = future.get();
                    if (result && result->success) {
                        success = true;
                        qInfo("[AppShell] /dc/cancel_scan OK: %s",
                              result->message.c_str());
                    } else {
                        qWarning("[AppShell] /dc/cancel_scan reported failure: %s",
                                 result ? result->message.c_str() : "(null)");
                    }
                } catch (const std::exception& e) {
                    qWarning("[AppShell] /dc/cancel_scan exception: %s", e.what());
                }
                run_done(success);
            }, Qt::QueuedConnection);
        });

    if (!planner_cancel_scan_wait_timer_) {
        planner_cancel_scan_wait_timer_ = new QTimer(this);
        planner_cancel_scan_wait_timer_->setInterval(50);
        planner_cancel_scan_wait_timer_->setSingleShot(false);
    }
    disconnect(planner_cancel_scan_wait_timer_, &QTimer::timeout, nullptr, nullptr);
    connect(planner_cancel_scan_wait_timer_, &QTimer::timeout, this, [this, run_done]() {
        ++planner_cancel_scan_wait_ticks_;
        // 50 ms × 160 = 8000 ms hard ceiling. Cancel needs more headroom
        // than finalize because the controller may rmtree a multi-section
        // mission folder (lots of small files).
        if (planner_cancel_scan_wait_ticks_ >= 160) {
            qWarning("[AppShell] /dc/cancel_scan did not respond within 8s — proceeding with UI reset anyway");
            run_done(false);
        }
    });
    planner_cancel_scan_wait_timer_->start();
}

void AppShellWindow::onPlannerDiscardScanRequested() {
    qInfo("[AppShell] PlannerScreen::discardScanRequested — post-Completed delete with retry-once");

    // Phase 1: motors should already be IDLE because the mission completed,
    // but defensively command IDLE again + drop autonomy. Cheap, idempotent.
    sendExplorationAxisStateRequest(kOdriveAxisStateIdle);
    publishPlannerAutonomyEnable(false);

    discardCompletedScanDataCollection([this](bool success) {
        // Discard never tears down the pipeline — operator advances by
        // pressing Complete Mission. Just hand the result back to the
        // PlannerScreen which latches its terminal "Discarded" UI state.
        if (stage5_) {
            stage5_->notifyScanDiscarded(success);
        }
    });
}

void AppShellWindow::discardCompletedScanDataCollection(std::function<void(bool)> done) {
    // First attempt. On success → done(true). On failure → second attempt.
    // On second failure → done(false). Total worst-case wait ≈ 16s (two
    // 8s ceilings back-to-back).
    cancelActiveScanDataCollection([this, done](bool first_success) {
        if (first_success) {
            qInfo("[AppShell] Discard: /dc/cancel_scan succeeded on first attempt");
            if (done) {
                done(true);
            }
            return;
        }

        qWarning("[AppShell] Discard: /dc/cancel_scan failed on first attempt — retrying once");
        cancelActiveScanDataCollection([done](bool retry_success) {
            if (retry_success) {
                qInfo("[AppShell] Discard: /dc/cancel_scan succeeded on retry");
            } else {
                qCritical("[AppShell] Discard: /dc/cancel_scan FAILED on retry — UI will latch "
                          "Discarded state but the controller may still hold mission data on disk");
            }
            if (done) {
                done(retry_success);
            }
        });
    });
}

void AppShellWindow::performExplorationPipelineTeardownPreamble() {
    const ResolvedRobotSshTarget t = resolveRobotSshForRemoteOps();
    if (!t.host.isEmpty()) {
        active_robot_host_ = t.host;
        active_robot_ssh_user_ = t.ssh_user;
    }

    if (exploration_launch_poll_timer_) {
        exploration_launch_poll_timer_->stop();
    }
    if (stage4_) {
        stage4_->forceTeleopStop();
        stage4_->stopFpvStream();
    }
}

void AppShellWindow::explorationStopPipelineTeardownKillProcessesAndResetUi() {
    const ResolvedRobotSshTarget ssh_target = resolveRobotSshForRemoteOps();
    const QString robot_host = ssh_target.host;

    auto stopProcess = [](QProcess* proc, int terminate_wait_ms, int kill_wait_ms) {
        if (!proc || proc->state() == QProcess::NotRunning) {
            return;
        }
        proc->terminate();
        if (!proc->waitForFinished(terminate_wait_ms)) {
            proc->kill();
            proc->waitForFinished(kill_wait_ms);
        }
    };

    map_save_or_download_in_progress_ = false;
    clearPendingSavedMapState();
    stopProcess(saved_map_download_proc_, 1200, 400);
    stopProcess(exploration_rf_probe_proc_, 500, 200);
    stopProcess(exploration_storage_probe_proc_, 1200, 300);
    stopProcess(laptop_launch_proc_, 1800, 600);
    stopProcess(robot_launch_proc_, 1800, 600);

    // Best-effort cleanup for local processes that may outlive launch wrappers.
    {
        QProcess cleanup_proc;
        const QString cleanup_cmd = QString(
            "pkill -f \"[r]os2 launch pilot_control laptop_teleop.launch.py\" >/dev/null 2>&1 || true; "
            "pkill -f \"[z]enohd -c /tmp/zenohd_laptop_%1.json5\" >/dev/null 2>&1 || true; "
            "pkill -f \"[/]pilot_control/host_teleop\" >/dev/null 2>&1 || true")
                                        .arg(robot_host);
        cleanup_proc.start("bash", QStringList() << "-lc" << cleanup_cmd);
        if (!cleanup_proc.waitForFinished(5000)) {
            cleanup_proc.kill();
            cleanup_proc.waitForFinished(500);
        }
    }

    // Best-effort cleanup on robot side for the full exploration stack.
    if (!robot_host.isEmpty()) {
        QProcess remote_cleanup_proc;
        // Send SIGTERM (default) first to the whole stack, then BLOCK
        // up to 5s for unified_data_collector to actually exit, then
        // escalate to SIGKILL on anything still alive. The wait is
        // critical: UDC's destructor calls seekcamera_manager_destroy()
        // which releases the Seek thermal SDK's libusb claim. If we
        // race past the SIGTERM here and the OCU starts a new launch
        // before the prior UDC has finished its destructor, the new
        // UDC opens the USB device against a half-released SDK and
        // never receives SEEKCAMERA_MANAGER_EVENT_CONNECT — visual
        // frames silently stop being recorded for the rest of the OCU
        // session until the operator physically replugs the camera.
        // See ~UnifiedDataCollector and the connect-watchdog WARN in
        // unified_data_collector.cpp for the matching robot-side guards.
        QString remote_script =
            "set +e; "
            "pkill -f '[r]os2 launch pilot_control robot_complete.launch.py' >/dev/null 2>&1 || true; "
            "pkill -f '[/]pilot_control/udc_supervisor' >/dev/null 2>&1 || true; "
            "pkill -f '[/]pilot_control/unified_data_collector' >/dev/null 2>&1 || true; "
            "pkill -f '[/]pilot_control/raw_map_saver' >/dev/null 2>&1 || true; "
            "pkill -f '[/]pilot_control/local_nav_grid_publisher' >/dev/null 2>&1 || true; "
            "pkill -f '[/]pilot_control/odom_tilt_corrector' >/dev/null 2>&1 || true; "
            "pkill -f '[m]pc_accel_autonomous_controller' >/dev/null 2>&1 || true; "
            "pkill -f '[f]astlio_mapping' >/dev/null 2>&1 || true; "
            "pkill -f '[d]iff_drive_controller' >/dev/null 2>&1 || true; "
            "pkill -f '[z]enohd -c /tmp/zenohd_robot.json5' >/dev/null 2>&1 || true; "
            "for i in $(seq 1 50); do "
            "  pgrep -f '[/]pilot_control/unified_data_collector' >/dev/null 2>&1 || break; "
            "  sleep 0.1; "
            "done; "
            "pkill -9 -f '[/]pilot_control/unified_data_collector' >/dev/null 2>&1 || true; "
            "pkill -9 -f '[/]pilot_control/udc_supervisor' >/dev/null 2>&1 || true; "
            "pkill -9 -f '[r]os2 launch pilot_control robot_complete.launch.py' >/dev/null 2>&1 || true";
        const QString remote_cmd = QString("bash -lc \"%1\"").arg(remote_script.replace("\"", "\\\""));

        QStringList args;
        args << "-o"
             << "ConnectTimeout=8"
             << "-o"
             << "StrictHostKeyChecking=no"
             << "-o"
             << "UserKnownHostsFile=/dev/null"
             << "-o"
             << "BatchMode=yes"
             << sshUserHostSpec(ssh_target)
             << remote_cmd;
        remote_cleanup_proc.start("ssh", args);
        // 15s = 8s ConnectTimeout headroom + 5s graceful UDC exit poll
        // + 2s slack for SIGKILL escalation and SSH framing overhead.
        if (!remote_cleanup_proc.waitForFinished(15000)) {
            remote_cleanup_proc.kill();
            remote_cleanup_proc.waitForFinished(500);
        }
    }

    exploration_launch_in_progress_ = false;
    exploration_launch_ready_ = false;
    exploration_launch_failed_ = false;
    map_save_or_download_in_progress_ = false;
    laptop_launch_started_ = false;
    robot_launch_started_ = false;
    laptop_launch_confirmed_ = false;
    robot_launch_confirmed_ = false;
    local_zenoh_ready_ = false;
    video_service_ready_ = false;
    odom_ready_ = false;
    stream_status_ready_ = false;
    // Disarm link monitor + reachability probe: scan over, no more
    // "robot offline" UI, no more background ICMP/TCP traffic.
    // PlannerScreen / ExplorationScreen revert to BOT IDLE pill.
    if (link_monitor_) {
        link_monitor_->disarm();
    }
    if (reachability_probe_) {
        reachability_probe_->disarm();
    }
    link_disconnect_started_at_ms_ = 0;
    link_recovery_resync_pending_ = false;
    if (stage4_) {
        stage4_->setBotLinkPillState(QStringLiteral("BOT ..."), ExplorationScreen::ValueTone::Muted);
        stage4_->setLinkConnectionState(/*connected=*/true, /*reachable=*/true, 0);
    }
    if (stage5_) {
        stage5_->setBotLinkPillState(QStringLiteral("BOT ..."), PlannerScreen::ValueTone::Muted);
        stage5_->setLinkConnectionState(/*connected=*/true, /*reachable=*/true, 0);
    }
    stream_target_published_ = false;
    fpv_started_ = false;
    exploration_rf_link_latched_ = false;
    exploration_rf_metric_ready_ = false;
    exploration_rf_probe_in_flight_ = false;
    exploration_rf_rssi_dbm_ = 0;
    exploration_last_rf_probe_at_ms_ = 0;
    exploration_last_rf_success_at_ms_ = 0;
    exploration_last_thermal_thumb_at_ms_ = 0;
    exploration_last_thermal_summary_at_ms_ = 0;
    exploration_thermal_thumb_ready_ = false;
    exploration_thermal_thumb_bytes_.clear();
    exploration_thermal_summary_ready_ = false;
    exploration_thermal_max_c_ = 0.0;
    exploration_thermal_avg_c_ = 0.0;
    exploration_thermal_min_c_ = 0.0;
    exploration_scan_started_at_ms_ = 0;
    last_launch_probe_at_ms_ = 0;
    last_stream_target_publish_at_ms_ = 0;
    stream_status_text_.clear();
    exploration_last_stream_status_at_ms_ = 0;
    exploration_odom_samples_.clear();
    exploration_latest_x_m_ = 0.0;
    exploration_latest_y_m_ = 0.0;
    exploration_latest_z_m_ = 0.0;
    exploration_latest_vx_mps_ = 0.0;
    exploration_latest_vy_mps_ = 0.0;
    exploration_latest_yaw_rad_ = 0.0;
    exploration_last_odom_at_ms_ = 0;
    exploration_last_local_nav_grid_at_ms_ = 0;
    exploration_local_nav_grid_ready_ = false;
    exploration_local_nav_grid_bytes_.clear();
    exploration_left_iq_measured_ = 0.0;
    exploration_right_iq_measured_ = 0.0;
    exploration_left_iq_at_ms_ = 0;
    exploration_right_iq_at_ms_ = 0;
    exploration_motor_current_scan_samples_.clear();
    exploration_motor_spike_streak_ = 0;
    exploration_motor_warning_active_ = false;
    latest_map_save_cutoff_at_ms_ = 0;
    latest_saved_map_local_path_.clear();
    clearPendingSavedMapState();
    exploration_storage_text_ = QStringLiteral("N/A");
    exploration_storage_ready_ = false;
    exploration_storage_probe_in_flight_ = false;
    exploration_last_storage_probe_at_ms_ = 0;
    exploration_rf_config_ready_ = false;
    exploration_rf_radio_ip_.clear();
    exploration_rf_snmp_ro_community_.clear();
    exploration_rf_rssi_oid_.clear();
    exploration_rf_snr_oid_.clear();
    {
        QSettings settings(f2c_cpp::kSettingsOrgName, f2c_cpp::kSettingsAppName);
        settings.remove("planner/driven_path_cutoff_ms");
    }

    if (stage4_) {
        stage4_->resetMappingWorkflowUi();
        stage4_->setPlanningEnabled(false);
        stage4_->setLaunchInProgress(false);
        stage4_->setLaunchReady(false);
        stage4_->setTopSignalState("Signal unavailable", ExplorationScreen::ValueTone::Muted);
        stage4_->setThermalHidden();
        stage4_->resetNavigationMap();
        stage4_->setLaunchProgress(0, "Pipeline stopped (test control)");
        stage4_->setLaunchDiagnostics(
            buildExplorationDiagnostics("Pipeline stopped by user (test control)"));
    }
}

void AppShellWindow::performExplorationPipelineTeardown() {
    performExplorationPipelineTeardownPreamble();
    explorationStopPipelineTeardownKillProcessesAndResetUi();
}

void AppShellWindow::onExplorationStopPipelineRequested() {
    performExplorationPipelineTeardownPreamble();
    beginExplorationMotorsIdleWait([this](bool timed_out) {
        if (timed_out) {
            qWarning("[AppShell] Stop Pipeline: motor disarm not confirmed within 2s — proceeding with teardown anyway");
        } else {
            qInfo("[AppShell] Stop Pipeline: motors confirmed IDLE — tearing down launch processes");
        }
        explorationStopPipelineTeardownKillProcessesAndResetUi();
    });
}

void AppShellWindow::onExplorationLaunchPoll() {
    if (!exploration_launch_in_progress_ && !exploration_launch_ready_) {
        return;
    }

    if (exploration_ros_node_ && rclcpp::ok()) {
        rclcpp::spin_some(exploration_ros_node_);
    }

    local_zenoh_ready_ = localZenohLaptopBridgeRunning();
    if (exploration_video_record_client_) {
        video_service_ready_ = exploration_video_record_client_->service_is_ready();
    }

    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    if (last_launch_probe_at_ms_ == 0 || (now_ms - last_launch_probe_at_ms_) >= 3000) {
        laptop_launch_confirmed_ =
            laptop_launch_started_ && laptop_launch_proc_ &&
            laptop_launch_proc_->state() == QProcess::Running;
        robot_launch_confirmed_ =
            robot_launch_started_ && robot_launch_proc_ &&
            robot_launch_proc_->state() == QProcess::Running;
        last_launch_probe_at_ms_ = now_ms;
    }

    const bool should_publish_stream_target =
        local_zenoh_ready_ && exploration_stream_target_pub_ &&
        (!stream_target_published_ ||
         (!stream_status_ready_ &&
          (last_stream_target_publish_at_ms_ == 0 ||
           (now_ms - last_stream_target_publish_at_ms_) >= 3000)));
    if (should_publish_stream_target) {
        publishExplorationStreamTarget();
    }

    if (stream_status_ready_ && !fpv_started_ && stage4_ && stack_->currentWidget() == stage4_) {
        stage4_->startFpvStream(5600);
        fpv_started_ = true;
    }

    const qint64 elapsed_ms = now_ms - exploration_launch_started_at_ms_;
    if (exploration_launch_in_progress_ && elapsed_ms > 120000) {
        QStringList missing;
        if (!laptop_launch_confirmed_) {
            missing << "laptop_teleop.launch.py";
        }
        if (!robot_launch_confirmed_) {
            missing << "robot_complete.launch.py";
        }
        if (!local_zenoh_ready_) missing << "local zenoh";
        if (!stream_target_published_) missing << "stream target publish";
        if (!video_service_ready_) missing << "/video_record_set service";
        if (!odom_ready_) missing << "/Odometry_tilt_corrected_diff";
        if (!stream_status_ready_) missing << "/stream_status";
        setExplorationLaunchFailed(
            QString("Startup timeout after %1s (missing: %2)")
                .arg(elapsed_ms / 1000)
                .arg(missing.join(", ")));
        return;
    }

    updateExplorationProgressUi();
}

void AppShellWindow::ensureExplorationRosInterfaces() {
    if (exploration_ros_node_ || !rclcpp::ok()) {
        return;
    }

    exploration_ros_node_ = rclcpp::Node::make_shared("bdr_exploration_orchestrator");
    exploration_stream_target_pub_ =
        exploration_ros_node_->create_publisher<std_msgs::msg::String>("/stream_target_ip", 10);
    // Streaming-camera publisher: routes to UDC's /stream_camera_select
    // subscriber (`unified_data_collector.cpp` ~L495).  Reliable QoS
    // because we don't want a brief Zenoh hiccup to drop the operator's
    // selection — UDC will rebuild the GStreamer pipeline on receipt.
    stream_camera_select_pub_ = exploration_ros_node_->create_publisher<std_msgs::msg::String>(
        "/stream_camera_select",
        rclcpp::QoS(1).reliable());
    // Streaming-camera status subscriber: UDC publishes the actual
    // active camera here after each pipeline rebuild settles.  We
    // forward into ExplorationScreen so the pill's active button
    // reflects the robot's TRUTH, not the operator's last click — and
    // so the click-debounce drops as soon as confirmation arrives.
    stream_camera_status_sub_ = exploration_ros_node_->create_subscription<std_msgs::msg::String>(
        "/stream_camera_status",
        rclcpp::QoS(1).reliable(),
        std::bind(&AppShellWindow::onStreamCameraStatus, this, std::placeholders::_1));
    exploration_cmd_vel_pub_ =
        exploration_ros_node_->create_publisher<geometry_msgs::msg::Twist>("/cmd_vel", 10);
    planner_mpc_autonomy_pub_ =
        exploration_ros_node_->create_publisher<std_msgs::msg::Bool>("/mpc_autonomy_enable", 10);
    exploration_stream_status_sub_ = exploration_ros_node_->create_subscription<std_msgs::msg::String>(
        "/stream_status",
        10,
        std::bind(&AppShellWindow::onExplorationStreamStatus, this, std::placeholders::_1));
    exploration_thermal_summary_sub_ = exploration_ros_node_->create_subscription<std_msgs::msg::String>(
        "/thermal/summary",
        rclcpp::QoS(rclcpp::KeepLast(10)).reliable(),
        std::bind(&AppShellWindow::onExplorationThermalSummary, this, std::placeholders::_1));
    planner_scan_status_sub_ = exploration_ros_node_->create_subscription<std_msgs::msg::String>(
        "/scan_segment_status",
        rclcpp::QoS(rclcpp::KeepLast(20)).reliable(),
        std::bind(&AppShellWindow::onPlannerScanExecutionStatus, this, std::placeholders::_1));
    // /udc/health: TRANSIENT_LOCAL so we catch the latched
    // DEAD_MAX_RESTARTS message even if we subscribe AFTER the
    // supervisor publishes it (e.g., operator restarts the OCU after
    // a UDC crash storm without restarting the launch).
    udc_health_sub_ = exploration_ros_node_->create_subscription<std_msgs::msg::String>(
        "/udc/health",
        rclcpp::QoS(1).transient_local(),
        std::bind(&AppShellWindow::onUdcHealthMessage, this, std::placeholders::_1));
    rclcpp::QoS thermal_thumb_qos(rclcpp::KeepLast(1));
    thermal_thumb_qos.best_effort();
    exploration_thermal_thumb_sub_ =
        exploration_ros_node_->create_subscription<std_msgs::msg::UInt8MultiArray>(
            "/thermal/thumb",
            thermal_thumb_qos,
            std::bind(&AppShellWindow::onExplorationThermalThumb, this, std::placeholders::_1));
    rclcpp::QoS local_nav_qos(rclcpp::KeepLast(1));
    local_nav_qos.best_effort();
    exploration_local_nav_grid_sub_ =
        exploration_ros_node_->create_subscription<std_msgs::msg::UInt8MultiArray>(
            "/local_nav_grid",
            local_nav_qos,
            std::bind(&AppShellWindow::onExplorationLocalNavGrid, this, std::placeholders::_1));
    exploration_odom_sub_ = exploration_ros_node_->create_subscription<nav_msgs::msg::Odometry>(
        "/Odometry_tilt_corrected_diff",
        rclcpp::QoS(rclcpp::KeepLast(20)).reliable(),
        std::bind(&AppShellWindow::onExplorationOdom, this, std::placeholders::_1));
    exploration_left_status_sub_ =
        exploration_ros_node_->create_subscription<odrive_can::msg::ControllerStatus>(
            "/left/controller_status",
            rclcpp::QoS(rclcpp::KeepLast(50)).reliable(),
            std::bind(&AppShellWindow::onExplorationLeftControllerStatus, this, std::placeholders::_1));
    exploration_right_status_sub_ =
        exploration_ros_node_->create_subscription<odrive_can::msg::ControllerStatus>(
            "/right/controller_status",
            rclcpp::QoS(rclcpp::KeepLast(50)).reliable(),
            std::bind(&AppShellWindow::onExplorationRightControllerStatus, this, std::placeholders::_1));
    exploration_video_record_client_ =
        exploration_ros_node_->create_client<std_srvs::srv::SetBool>("/video_record_set");
    exploration_save_map_client_ =
        exploration_ros_node_->create_client<std_srvs::srv::Trigger>("/save_raw_map");
    planner_dc_pause_client_ =
        exploration_ros_node_->create_client<std_srvs::srv::Trigger>("/dc/pause");
    planner_dc_resume_client_ =
        exploration_ros_node_->create_client<std_srvs::srv::Trigger>("/dc/resume");
    planner_dc_finalize_mission_client_ =
        exploration_ros_node_->create_client<std_srvs::srv::Trigger>("/dc/finalize_mission");
    planner_dc_cancel_scan_client_ =
        exploration_ros_node_->create_client<std_srvs::srv::Trigger>("/dc/cancel_scan");
    exploration_left_axis_client_ =
        exploration_ros_node_->create_client<odrive_can::srv::AxisState>("/left/request_axis_state");
    exploration_right_axis_client_ =
        exploration_ros_node_->create_client<odrive_can::srv::AxisState>("/right/request_axis_state");
    exploration_gpr_axis_client_ =
        exploration_ros_node_->create_client<odrive_can::srv::AxisState>("/gpr/request_axis_state");
    exploration_gpr_power_off_client_ =
        exploration_ros_node_->create_client<std_srvs::srv::Trigger>("/gpr_power_off");
    exploration_gpr_line_stop_client_ =
        exploration_ros_node_->create_client<std_srvs::srv::Trigger>("/gpr_line_stop");
    planner_f2c_waypoints_pub_ =
        exploration_ros_node_->create_publisher<std_msgs::msg::Float64MultiArray>(
            "/f2c_waypoints", rclcpp::QoS(rclcpp::KeepLast(10)).reliable().transient_local());
}

void AppShellWindow::startLaptopTeleopLaunch(const QString& robot_host) {
    if (!laptop_launch_proc_) {
        return;
    }
    if (laptop_launch_proc_->state() != QProcess::NotRunning) {
        laptop_launch_proc_->terminate();
        if (!laptop_launch_proc_->waitForFinished(1500)) {
            laptop_launch_proc_->kill();
            laptop_launch_proc_->waitForFinished(500);
        }
    }

    // Always start from a clean local teleop state to avoid duplicate Zenoh/DDS conflicts.
    QProcess cleanup_proc;
    const QString cleanup_cmd = QString(
        "pkill -f \"[r]os2 launch pilot_control laptop_teleop.launch.py\" >/dev/null 2>&1 || true; "
        "pkill -f \"[z]enohd -c /tmp/zenohd_laptop_%1.json5\" >/dev/null 2>&1 || true; "
        "pkill -f \"[/]pilot_control/host_teleop\" >/dev/null 2>&1 || true; "
        "sleep 1")
                                    .arg(robot_host);
    cleanup_proc.start("bash", QStringList() << "-lc" << cleanup_cmd);
    cleanup_proc.waitForFinished(5000);

    QString local_cmd =
        "set -e; "
        "if [ -f \"$HOME/.bashrc\" ]; then source \"$HOME/.bashrc\"; fi; "
        "if [ -f /opt/ros/humble/setup.bash ]; then source /opt/ros/humble/setup.bash; fi; "
        "if [ -f \"$HOME/pilot_ws/install/setup.bash\" ]; then source \"$HOME/pilot_ws/install/setup.bash\"; fi; "
        "case \"${CYCLONEDDS_URI:-}\" in *rf_cyclonedds.xml*) unset CYCLONEDDS_URI ;; esac; "
        "if [ -z \"${CYCLONEDDS_URI:-}\" ] && [ -f \"$HOME/cyclone_loopback.xml\" ]; then "
        "export CYCLONEDDS_URI=\"file://$HOME/cyclone_loopback.xml\"; "
        "fi; "
        "export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp; "
        "export ROS_DOMAIN_ID=0; "
        "echo \"[BDR app laptop launch env] CYCLONEDDS_URI=${CYCLONEDDS_URI:-<unset>} "
        "RMW_IMPLEMENTATION=${RMW_IMPLEMENTATION:-<unset>} ROS_DOMAIN_ID=${ROS_DOMAIN_ID:-<unset>}\"; "
        "ros2 launch pilot_control laptop_teleop.launch.py "
        "robot_ip:=%1 use_xterm:=false interactive_sdl:=false cmd_vel_enabled:=false";
    local_cmd = local_cmd.arg(robot_host);

    laptop_launch_proc_->start("bash", QStringList() << "-lc" << local_cmd);
    if (!laptop_launch_proc_->waitForStarted(3000)) {
        setExplorationLaunchFailed(
            QString("Failed to start laptop launch: %1").arg(laptop_launch_proc_->errorString()));
        return;
    }
    laptop_launch_started_ = true;
}

void AppShellWindow::startRobotCompleteLaunch(const ResolvedRobotSshTarget& ssh_target) {
    if (!robot_launch_proc_) {
        return;
    }
    const QString robot_host = ssh_target.host;
    if (robot_host.trimmed().isEmpty()) {
        setExplorationLaunchFailed(QStringLiteral("Robot host is empty; cannot start robot launch."));
        return;
    }
    if (robot_launch_proc_->state() != QProcess::NotRunning) {
        robot_launch_started_ = true;
        return;
    }

    QString remote_script =
        "set -e; "
        "if [ -f \"$HOME/.bashrc\" ]; then source \"$HOME/.bashrc\"; fi; "
        "if [ -f /opt/ros/humble/setup.bash ]; then source /opt/ros/humble/setup.bash; "
        "elif [ -f /opt/ros/foxy/setup.bash ]; then source /opt/ros/foxy/setup.bash; "
        "fi; "
        "if [ -f \"$HOME/pilot_ws/install/setup.bash\" ]; then source \"$HOME/pilot_ws/install/setup.bash\"; fi; "
        "case \"${CYCLONEDDS_URI:-}\" in *rf_cyclonedds.xml*) unset CYCLONEDDS_URI ;; esac; "
        "if [ -z \"${CYCLONEDDS_URI:-}\" ] && [ -f \"$HOME/cyclone_loopback.xml\" ]; then "
        "export CYCLONEDDS_URI=\"file://$HOME/cyclone_loopback.xml\"; "
        "fi; "
        "export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp; "
        "export ROS_DOMAIN_ID=0; "
        "echo \"[BDR app robot launch env] CYCLONEDDS_URI=${CYCLONEDDS_URI:-<unset>} "
        "RMW_IMPLEMENTATION=${RMW_IMPLEMENTATION:-<unset>} ROS_DOMAIN_ID=${ROS_DOMAIN_ID:-<unset>}\"; "
        "ros2 launch pilot_control robot_complete.launch.py";
    const QString remote_cmd = QString("bash -lc \"%1\"").arg(remote_script.replace("\"", "\\\""));

    QStringList args;
    args << "-tt"
         << "-o"
         << "ConnectTimeout=10"
         << "-o"
         << "StrictHostKeyChecking=no"
         << "-o"
         << "UserKnownHostsFile=/dev/null"
         << "-o"
         << "BatchMode=yes"
         << sshUserHostSpec(ssh_target)
         << remote_cmd;

    robot_launch_proc_->start("ssh", args);
    if (!robot_launch_proc_->waitForStarted(3000)) {
        setExplorationLaunchFailed(
            QString("Failed to start robot launch: %1").arg(robot_launch_proc_->errorString()));
        return;
    }
    robot_launch_started_ = true;
}

void AppShellWindow::setExplorationLaunchFailed(const QString& reason) {
    exploration_launch_in_progress_ = false;
    exploration_launch_ready_ = false;
    exploration_launch_failed_ = true;
    exploration_rf_link_latched_ = false;
    exploration_rf_metric_ready_ = false;
    exploration_rf_probe_in_flight_ = false;
    exploration_rf_rssi_dbm_ = 0;
    exploration_last_rf_probe_at_ms_ = 0;
    exploration_last_rf_success_at_ms_ = 0;
    exploration_last_thermal_thumb_at_ms_ = 0;
    exploration_last_thermal_summary_at_ms_ = 0;
    exploration_thermal_thumb_ready_ = false;
    exploration_thermal_thumb_bytes_.clear();
    exploration_thermal_summary_ready_ = false;
    exploration_thermal_max_c_ = 0.0;
    exploration_thermal_avg_c_ = 0.0;
    exploration_thermal_min_c_ = 0.0;
    exploration_last_local_nav_grid_at_ms_ = 0;
    exploration_local_nav_grid_ready_ = false;
    exploration_local_nav_grid_bytes_.clear();
    exploration_latest_yaw_rad_ = 0.0;
    exploration_scan_started_at_ms_ = 0;
    map_save_or_download_in_progress_ = false;
    if (exploration_rf_probe_proc_ &&
        exploration_rf_probe_proc_->state() != QProcess::NotRunning) {
        exploration_rf_probe_proc_->terminate();
        if (!exploration_rf_probe_proc_->waitForFinished(300)) {
            exploration_rf_probe_proc_->kill();
            exploration_rf_probe_proc_->waitForFinished(150);
        }
    }
    if (exploration_launch_poll_timer_) {
        exploration_launch_poll_timer_->stop();
    }
    if (stage4_) {
        stage4_->forceTeleopStop();
        stage4_->resetMappingWorkflowUi();
        stage4_->setPlanningEnabled(false);
        stage4_->setLaunchInProgress(false);
        stage4_->setLaunchReady(false);
        stage4_->setTopSignalState("Signal unavailable", ExplorationScreen::ValueTone::Muted);
        stage4_->setThermalHidden();
        stage4_->resetNavigationMap();
        stage4_->setLaunchProgress(0, reason);
        stage4_->setLaunchDiagnostics(buildExplorationDiagnostics(reason));
    }
}

void AppShellWindow::publishExplorationStreamTarget() {
    if (!exploration_stream_target_pub_) {
        return;
    }
    const QString local_ip = detectLocalIP();
    if (local_ip.isEmpty()) {
        setExplorationLaunchFailed("Cannot detect local IP for stream target");
        return;
    }
    std_msgs::msg::String msg;
    msg.data = local_ip.toStdString();
    exploration_stream_target_pub_->publish(msg);
    stream_target_published_ = true;
    last_stream_target_publish_at_ms_ = QDateTime::currentMSecsSinceEpoch();
}

// ---- Streaming-camera selection (Stage 4 FPV pill) ---------------------

void AppShellWindow::onExplorationCameraSelectRequested(const QString& cam) {
    if (cam != QStringLiteral("left") && cam != QStringLiteral("right")) {
        return;
    }
    // Persist the request immediately so it survives an OCU restart
    // even before UDC confirms it via /stream_camera_status.  The
    // confirmation just locks in the visual.
    QSettings settings(kSettingsOrgName, kSettingsAppName);
    settings.setValue(kSettingsStreamingCameraKey, cam);
    publishStreamCameraSelection(cam);
}

void AppShellWindow::publishStreamCameraSelection(const QString& cam) {
    if (!stream_camera_select_pub_) {
        return;
    }
    std_msgs::msg::String msg;
    msg.data = cam.toStdString();
    stream_camera_select_pub_->publish(msg);
}

void AppShellWindow::onStreamCameraStatus(const std_msgs::msg::String::SharedPtr msg) {
    if (!msg) {
        return;
    }
    const QString cam = QString::fromStdString(msg->data).trimmed().toLower();
    if (cam != QStringLiteral("left") && cam != QStringLiteral("right")) {
        return;
    }
    if (stage4_) {
        // Marshal to the GUI thread — ROS callbacks land on the
        // exploration spinner thread.  QMetaObject::invokeMethod with
        // Qt::QueuedConnection is the standard pattern used elsewhere
        // in this file (see onExplorationThermalSummary etc.) but
        // since stage4_ exposes a thread-safe setter, a direct call
        // via Qt::QueuedConnection is cheaper.
        QMetaObject::invokeMethod(stage4_,
            [stage = stage4_, cam]() {
                stage->setStreamingCamera(cam);
            },
            Qt::QueuedConnection);
    }
}

bool AppShellWindow::requestSavedMapPathWithRetry(QString* saved_remote_path,
                                                  QString* error_message) {
    if (saved_remote_path) {
        saved_remote_path->clear();
    }

    QString last_error = QStringLiteral("Unknown /save_raw_map failure");
    for (int attempt = 1; attempt <= kSaveRawMapMaxAttempts; ++attempt) {
        const bool retrying = attempt > 1;
        if (stage4_) {
            stage4_->showMapSaveInProgress();
            stage4_->setPlanningEnabled(false);
            stage4_->setLaunchProgress(100,
                                       retrying ? "Retrying tilt-corrected map save..."
                                                : "Saving tilt-corrected map...");
            stage4_->setLaunchDiagnostics(buildExplorationDiagnostics(
                retrying ? QStringLiteral("Retrying /save_raw_map for tilt-corrected map")
                         : QStringLiteral("Calling /save_raw_map for tilt-corrected map")));
        }

        if (!exploration_save_map_client_->wait_for_service(std::chrono::seconds(3))) {
            last_error = QStringLiteral("/save_raw_map service not ready");
            continue;
        }

        auto request = std::make_shared<std_srvs::srv::Trigger::Request>();
        auto future = exploration_save_map_client_->async_send_request(request);
        const auto save_rc = rclcpp::spin_until_future_complete(
            exploration_ros_node_, future, std::chrono::seconds(20));
        if (save_rc != rclcpp::FutureReturnCode::SUCCESS) {
            last_error = QStringLiteral("/save_raw_map timed out");
            continue;
        }

        std_srvs::srv::Trigger::Response::SharedPtr response;
        try {
            response = future.get();
        } catch (const std::exception& ex) {
            last_error = QString("/save_raw_map exception: %1").arg(ex.what());
            continue;
        }

        const QString save_message =
            response ? QString::fromStdString(response->message) : QStringLiteral("(empty response)");
        if (!response || !response->success) {
            last_error = QString("/save_raw_map failed: %1").arg(save_message.left(220));
            continue;
        }

        const QString remote_path = extractSavedMapPath(save_message);
        if (remote_path.isEmpty()) {
            last_error =
                QString("Cannot parse saved map path from /save_raw_map response: %1")
                    .arg(save_message.left(220));
            continue;
        }

        if (saved_remote_path) {
            *saved_remote_path = remote_path;
        }
        return true;
    }

    if (error_message) {
        *error_message = last_error;
    }
    return false;
}

QString AppShellWindow::resolveLatestCorrectedMapUnderDataRoot(QString* error_message) const {
    const ResolvedRobotSshTarget ssh_target = resolveRobotSshForRemoteOps();
    const QString cleaned_root = QStringLiteral("/R_DATA");
    if (ssh_target.host.isEmpty()) {
        if (error_message) {
            *error_message = QStringLiteral("Robot host unavailable for corrected-map lookup");
        }
        return QString();
    }
    if (cleaned_root.isEmpty()) {
        if (error_message) {
            *error_message = QStringLiteral("Corrected-map search root is empty");
        }
        return QString();
    }

    const QString python_code = QStringLiteral(
        "import os, sys\n"
        "root = sys.argv[1]\n"
        "if not os.path.isdir(root):\n"
        "    sys.stderr.write('Search root not found: ' + root)\n"
        "    sys.exit(12)\n"
        "best = None\n"
        "for dirpath, _, filenames in os.walk(root):\n"
        "    for filename in filenames:\n"
        "        if not filename.startswith('corrected_map_') or not filename.endswith('.pcd'):\n"
        "            continue\n"
        "        path = os.path.join(dirpath, filename)\n"
        "        try:\n"
        "            modified_at = os.path.getmtime(path)\n"
        "        except OSError:\n"
        "            continue\n"
        "        if best is None or modified_at > best[0]:\n"
        "            best = (modified_at, path)\n"
        "if best is None:\n"
        "    sys.exit(3)\n"
        "print(best[1])\n");
    const QString remote_cmd = QString("python3 -c %1 %2")
                                   .arg(shellSingleQuote(python_code),
                                        shellSingleQuote(cleaned_root));

    QProcess proc;
    QStringList args;
    args << "-o"
         << "ConnectTimeout=6"
         << "-o"
         << "StrictHostKeyChecking=no"
         << "-o"
         << "UserKnownHostsFile=/dev/null"
         << "-o"
         << "BatchMode=yes"
         << sshUserHostSpec(ssh_target)
         << remote_cmd;
    proc.start("ssh", args);
    if (!proc.waitForFinished(15000)) {
        proc.kill();
        proc.waitForFinished(500);
        if (error_message) {
            *error_message =
                QString("Timed out while resolving newest corrected map under %1").arg(cleaned_root);
        }
        return QString();
    }

    const QString resolved_path = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
    const QString stderr_output = QString::fromUtf8(proc.readAllStandardError()).trimmed();
    if (proc.exitCode() == 3 || resolved_path.isEmpty()) {
        if (error_message) {
            *error_message = QString("No tilt-corrected map found under %1").arg(cleaned_root);
        }
        return QString();
    }
    if (proc.exitCode() != 0) {
        if (error_message) {
            *error_message = stderr_output.isEmpty()
                                 ? QString("Corrected-map lookup failed under %1").arg(cleaned_root)
                                 : stderr_output.left(220);
        }
        return QString();
    }
    if (!resolved_path.startsWith(cleaned_root + QStringLiteral("/")) &&
        resolved_path != cleaned_root) {
        if (error_message) {
            *error_message = QString("Resolved corrected map is outside %1: %2")
                                 .arg(cleaned_root, resolved_path.left(220));
        }
        return QString();
    }
    if (!QFileInfo(resolved_path).fileName().startsWith(QStringLiteral("corrected_map_"))) {
        if (error_message) {
            *error_message =
                QString("Resolved file is not tilt-corrected: %1")
                    .arg(QFileInfo(resolved_path).fileName());
        }
        return QString();
    }
    return resolved_path;
}

void AppShellWindow::beginCorrectedMapResolveAndDownload(int retries_remaining) {
    if (stage4_) {
        stage4_->showMapDownloadInProgress();
        stage4_->setPlanningEnabled(false);
        stage4_->setLaunchProgress(100,
                                   retries_remaining < kCorrectedMapRetryCount
                                       ? "Retrying corrected map lookup..."
                                       : "Resolving newest tilt-corrected map under /R_DATA...");
        stage4_->setLaunchDiagnostics(buildExplorationDiagnostics(
            QString("Resolving newest corrected_map_*.pcd under /R_DATA")));
    }

    QString resolve_error;
    const QString corrected_remote_path =
        resolveLatestCorrectedMapUnderDataRoot(&resolve_error);
    if (corrected_remote_path.isEmpty()) {
        if (retries_remaining > 0) {
            if (stage4_) {
                stage4_->showMapDownloadInProgress();
                stage4_->setPlanningEnabled(false);
                stage4_->setLaunchProgress(100, "Retrying corrected map lookup...");
                stage4_->setLaunchDiagnostics(buildExplorationDiagnostics(
                    resolve_error +
                    QStringLiteral(" Retrying newest corrected-map lookup under /R_DATA.")));
            }
            QTimer::singleShot(
                kCorrectedMapRetryDelayMs,
                this,
                [this, retries_remaining]() {
                    beginCorrectedMapResolveAndDownload(retries_remaining - 1);
                });
            return;
        }

        failSavedMapWorkflow("Corrected map not found.", resolve_error);
        return;
    }

    if (stage4_) {
        stage4_->showMapDownloadInProgress();
        stage4_->setPlanningEnabled(false);
        stage4_->setLaunchProgress(
            100,
            QString("Downloading %1...").arg(QFileInfo(corrected_remote_path).fileName()));
        stage4_->setLaunchDiagnostics(buildExplorationDiagnostics(
            QString("Selected corrected map: %1").arg(corrected_remote_path)));
    }

    startSavedMapDownload(corrected_remote_path, retries_remaining);
}

QString AppShellWindow::extractSavedMapPath(const QString& save_message) const {
    const QString trimmed = save_message.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }

    // Expected format from raw_map_saver: "... to /path/to/file.pcd"
    const int to_idx = trimmed.lastIndexOf(" to ");
    if (to_idx >= 0) {
        QString candidate = trimmed.mid(to_idx + 4).trimmed();
        if (candidate.startsWith('"') && candidate.endsWith('"') && candidate.size() >= 2) {
            candidate = candidate.mid(1, candidate.size() - 2);
        }
        if (candidate.startsWith('\'') && candidate.endsWith('\'') && candidate.size() >= 2) {
            candidate = candidate.mid(1, candidate.size() - 2);
        }
        if (candidate.startsWith('/')) {
            return candidate;
        }
    }

    // Fallback: first absolute path token in message.
    const QRegularExpression path_rx(R"((/[^ \n\r\t]+))");
    const QRegularExpressionMatch match = path_rx.match(trimmed);
    if (match.hasMatch()) {
        return match.captured(1);
    }
    return QString();
}

void AppShellWindow::clearPendingSavedMapState() {
    pending_saved_map_remote_path_.clear();
    pending_saved_map_local_path_.clear();
    pending_saved_map_retry_remaining_ = 0;
}

void AppShellWindow::failSavedMapWorkflow(const QString& progress_message,
                                          const QString& diagnostic_message) {
    map_save_or_download_in_progress_ = false;
    latest_saved_map_local_path_.clear();
    clearPendingSavedMapState();
    if (!stage4_) {
        return;
    }

    stage4_->setPrimaryActionReadyToFinish();
    stage4_->setPlanningEnabled(false);
    stage4_->setLaunchProgress(100, progress_message);
    stage4_->setLaunchDiagnostics(buildExplorationDiagnostics(diagnostic_message));
}

QString AppShellWindow::localMapDownloadPathForRemote(const QString& remote_map_path) const {
    const QString cleaned_path = QDir::cleanPath(remote_map_path.trimmed());
    const QFileInfo remote_info(cleaned_path);
    const QString file_name = remote_info.fileName();
    if (file_name.isEmpty()) {
        return QString();
    }

    QString relative_dir = QDir::cleanPath(remote_info.path());
    const QString remote_root = QStringLiteral("/R_DATA");
    if (relative_dir == remote_root) {
        relative_dir.clear();
    } else if (relative_dir.startsWith(remote_root + QStringLiteral("/"))) {
        relative_dir.remove(0, remote_root.size() + 1);
    } else if (relative_dir.startsWith('/')) {
        relative_dir.remove(0, 1);
    }

    QString local_dir = QDir::homePath() + "/Roofus_maps";
    if (!relative_dir.isEmpty()) {
        local_dir += "/" + relative_dir;
    }
    if (!QDir().mkpath(local_dir)) {
        return QString();
    }
    return local_dir + "/" + file_name;
}

void AppShellWindow::startSavedMapDownload(const QString& remote_map_path,
                                           int retries_remaining) {
    if (!saved_map_download_proc_) {
        failSavedMapWorkflow("Corrected map download unavailable.",
                             "Download failed: process not initialized");
        return;
    }

    const QString cleaned_remote_path = QDir::cleanPath(remote_map_path.trimmed());
    if (!QFileInfo(cleaned_remote_path).fileName().startsWith(QStringLiteral("corrected_map_"))) {
        failSavedMapWorkflow("Corrected map selection failed.",
                             QString("Resolved map is not tilt-corrected: %1")
                                 .arg(QFileInfo(cleaned_remote_path).fileName()));
        return;
    }

    const ResolvedRobotSshTarget ssh_target = resolveRobotSshForRemoteOps();
    active_robot_host_ = ssh_target.host;
    active_robot_ssh_user_ = ssh_target.ssh_user;
    if (ssh_target.host.isEmpty()) {
        failSavedMapWorkflow("Robot host unavailable for download.",
                             "Download failed: robot host is empty");
        return;
    }

    const QString local_map_path = localMapDownloadPathForRemote(cleaned_remote_path);
    if (local_map_path.isEmpty()) {
        failSavedMapWorkflow("Cannot create local corrected-map folder.",
                             "Download failed: cannot allocate mirrored local map path");
        return;
    }

    pending_saved_map_remote_path_ = cleaned_remote_path;
    pending_saved_map_local_path_ = local_map_path;
    pending_saved_map_retry_remaining_ = std::max(0, retries_remaining);

    if (saved_map_download_proc_->state() != QProcess::NotRunning) {
        saved_map_download_proc_->kill();
        saved_map_download_proc_->waitForFinished(500);
    }

    const QString remote_spec =
        QStringLiteral("%1:%2").arg(sshUserHostSpec(ssh_target), cleaned_remote_path);
    QStringList args;
    args << "-o"
         << "ConnectTimeout=10"
         << "-o"
         << "StrictHostKeyChecking=no"
         << "-o"
         << "UserKnownHostsFile=/dev/null"
         << "-o"
         << "BatchMode=yes"
         << remote_spec
         << local_map_path;
    saved_map_download_proc_->start("scp", args);

    if (!saved_map_download_proc_->waitForStarted(3000)) {
        const QString selected_remote_map = pending_saved_map_remote_path_;
        const int retries_left = pending_saved_map_retry_remaining_;
        const QString error_string = saved_map_download_proc_->errorString();
        clearPendingSavedMapState();
        if (retries_left > 0) {
            if (stage4_) {
                stage4_->showMapDownloadInProgress();
                stage4_->setPlanningEnabled(false);
                stage4_->setLaunchProgress(100, "Retrying corrected map download...");
                stage4_->setLaunchDiagnostics(buildExplorationDiagnostics(
                    QString("Download failed to start: %1 Selected corrected map: %2. "
                            "Retrying newest corrected-map lookup under /R_DATA.")
                        .arg(error_string, selected_remote_map)));
            }
            QTimer::singleShot(
                kCorrectedMapRetryDelayMs,
                this,
                [this, retries_left]() {
                    beginCorrectedMapResolveAndDownload(retries_left - 1);
                });
            return;
        }

        failSavedMapWorkflow("Failed to start corrected map download.",
                             QString("Download failed to start: %1 Selected corrected map: %2")
                                 .arg(error_string, selected_remote_map));
    }
}

void AppShellWindow::updateExplorationProgressUi() {
    if (!stage4_) {
        return;
    }

    auto lastNonEmptyLine = [](const QString& text) -> QString {
        const QStringList lines = text.split('\n', Qt::SkipEmptyParts);
        if (lines.isEmpty()) {
            return QString();
        }
        return lines.last().trimmed();
    };

    const bool local_launch_ok = laptop_launch_confirmed_;
    const bool robot_launch_ok = robot_launch_confirmed_;

    int progress = 0;
    if (local_launch_ok) progress += 20;
    if (robot_launch_ok) progress += 20;
    if (local_zenoh_ready_) progress += 15;
    if (stream_target_published_) progress += 10;
    if (video_service_ready_) progress += 15;
    if (odom_ready_) progress += 10;
    if (stream_status_ready_) progress += 10;

    QString status = "Starting laptop launch...";
    if (!local_launch_ok) {
        status = laptop_launch_started_ ? "Waiting for laptop launch process..." : "Starting laptop launch...";
        const QString hint = lastNonEmptyLine(laptop_launch_last_output_);
        if (!hint.isEmpty()) {
            status += QString(" (%1)").arg(hint.left(100));
        }
    } else if (!robot_launch_ok) {
        status = robot_launch_started_ ? "Waiting for robot launch process..." : "Starting robot launch...";
        const QString hint = lastNonEmptyLine(robot_launch_last_output_);
        if (!hint.isEmpty()) {
            status += QString(" (%1)").arg(hint.left(100));
        }
    } else if (!local_zenoh_ready_) {
        status = "Waiting for Zenoh bridge...";
    } else if (!stream_target_published_) {
        status = "Configuring stream target...";
    } else if (!video_service_ready_) {
        status = "Waiting for video services...";
    } else if (!odom_ready_) {
        status = "Waiting for mapping odometry...";
    } else if (!stream_status_ready_) {
        status = "Waiting for stream status...";
    } else {
        status = "All systems ready";
    }

    stage4_->setLaunchProgress(progress, status);
    stage4_->setLaunchDiagnostics(buildExplorationDiagnostics(status));

    if (progress >= 100 && !exploration_launch_ready_) {
        exploration_launch_ready_ = true;
        exploration_launch_in_progress_ = false;
        exploration_launch_failed_ = false;
        if (exploration_scan_started_at_ms_ <= 0) {
            exploration_scan_started_at_ms_ = QDateTime::currentMSecsSinceEpoch();
        }
        stage4_->setLaunchInProgress(false);
        stage4_->setLaunchReady(true);
        stage4_->setLaunchDiagnostics(buildExplorationDiagnostics("All systems ready"));
        if (exploration_launch_poll_timer_) {
            exploration_launch_poll_timer_->stop();
        }
    }
}

QString AppShellWindow::buildExplorationDiagnostics(const QString& headline) const {
    auto processStateText = [](const QProcess* proc) -> QString {
        if (!proc) {
            return "null";
        }
        switch (proc->state()) {
            case QProcess::NotRunning:
                return "not_running";
            case QProcess::Starting:
                return "starting";
            case QProcess::Running:
                return "running";
        }
        return "unknown";
    };

    auto tailLog = [](const QString& text, int max_lines, int max_chars) -> QString {
        QString cleaned = text;
        cleaned.replace('\r', '\n');
        QStringList lines = cleaned.split('\n', Qt::SkipEmptyParts);
        if (lines.size() > max_lines) {
            lines = lines.mid(lines.size() - max_lines);
        }
        QString joined = lines.join("\n").trimmed();
        if (joined.size() > max_chars) {
            joined = joined.right(max_chars);
        }
        return joined;
    };

    auto mark = [](bool ready) -> QString {
        return ready ? "[OK]" : "[..]";
    };

    const bool local_launch_ok = laptop_launch_confirmed_;
    const bool robot_launch_ok = robot_launch_confirmed_;

    QStringList lines;
    lines << "Exploration launch diagnostics";
    if (!headline.trimmed().isEmpty()) {
        lines << QString("status: %1").arg(headline.trimmed());
    }
    lines << QString("robot_host: %1")
                 .arg(active_robot_host_.isEmpty() ? QStringLiteral("(unset)") : active_robot_host_);
    lines << QString("launch: in_progress=%1 ready=%2")
                 .arg(exploration_launch_in_progress_ ? "yes" : "no")
                 .arg(exploration_launch_ready_ ? "yes" : "no");
    lines << QString("%1 laptop_launch (started=%2 confirmed=%3 proc=%4)")
                 .arg(mark(local_launch_ok))
                 .arg(laptop_launch_started_ ? "yes" : "no")
                 .arg(laptop_launch_confirmed_ ? "yes" : "no")
                 .arg(processStateText(laptop_launch_proc_));
    lines << QString("%1 robot_launch (started=%2 confirmed=%3 proc=%4)")
                 .arg(mark(robot_launch_ok))
                 .arg(robot_launch_started_ ? "yes" : "no")
                 .arg(robot_launch_confirmed_ ? "yes" : "no")
                 .arg(processStateText(robot_launch_proc_));
    lines << QString("%1 zenohd local process").arg(mark(local_zenoh_ready_));
    lines << QString("%1 /video_record_set service").arg(mark(video_service_ready_));
    lines << QString("%1 /Odometry_tilt_corrected_diff").arg(mark(odom_ready_));
    lines << QString("%1 /stream_status").arg(mark(stream_status_ready_));
    lines << QString("%1 /stream_target_ip published").arg(mark(stream_target_published_));

    if (!stream_status_text_.trimmed().isEmpty()) {
        lines << QString("stream_status payload: %1").arg(stream_status_text_.trimmed().left(180));
    }

    const QString laptop_tail = tailLog(laptop_launch_last_output_, 8, 700);
    if (!laptop_tail.isEmpty()) {
        lines << "";
        lines << "laptop launch tail:";
        lines << laptop_tail;
    }

    const QString robot_tail = tailLog(robot_launch_last_output_, 8, 700);
    if (!robot_tail.isEmpty()) {
        lines << "";
        lines << "robot launch tail:";
        lines << robot_tail;
    }

    return lines.join("\n");
}

QString AppShellWindow::detectLocalIP() const {
    QString rf_ip;
    QString wifi_ip;
    QString other_ip;

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

ResolvedRobotSshTarget AppShellWindow::resolveRobotSshForRemoteOps() const {
    ResolvedRobotSshTarget t;
    if (!active_robot_host_.trimmed().isEmpty()) {
        t.host = active_robot_host_.trimmed();
        t.ssh_user = active_robot_ssh_user_.trimmed().isEmpty() ? QStringLiteral("roofus")
                                                                 : active_robot_ssh_user_.trimmed();
        return t;
    }
    QString err;
    if (resolveRobotSshTargetFromSettings(&t, &err)) {
        return t;
    }
    return {};
}

bool AppShellWindow::isLocalProcessRunning(const QString& process_name) const {
    QProcess proc;
    proc.start("pgrep", QStringList() << "-x" << process_name);
    if (!proc.waitForFinished(1200)) {
        return false;
    }
    return proc.exitCode() == 0;
}

bool AppShellWindow::localZenohLaptopBridgeRunning() const {
    QProcess proc;
    // Must stay in sync with pilot_control/launch/laptop_teleop.launch.py temp config path prefix.
    proc.start("pgrep", QStringList() << "-f" << "zenohd_laptop_");
    if (!proc.waitForFinished(1200)) {
        return false;
    }
    return proc.exitCode() == 0;
}

bool AppShellWindow::isRobotPipelineRunning(const ResolvedRobotSshTarget& ssh_target) const {
    if (ssh_target.host.trimmed().isEmpty()) {
        return false;
    }
    QProcess proc;
    const QString remote_cmd =
        "bash -lc \"pgrep -f 'unified_data_collector|fastlio_mapping|diff_drive_controller' >/dev/null\"";
    QStringList args;
    args << "-o"
         << "ConnectTimeout=6"
         << "-o"
         << "StrictHostKeyChecking=no"
         << "-o"
         << "UserKnownHostsFile=/dev/null"
         << "-o"
         << "BatchMode=yes"
         << sshUserHostSpec(ssh_target)
         << remote_cmd;
    proc.start("ssh", args);
    if (!proc.waitForFinished(7000)) {
        proc.kill();
        proc.waitForFinished(500);
        return false;
    }
    return proc.exitCode() == 0;
}

void AppShellWindow::onExplorationStreamStatus(const std_msgs::msg::String::SharedPtr msg) {
    if (link_monitor_) {
        link_monitor_->stamp(LinkHealthMonitor::Source::StreamStatus);
    }
    if (!msg) {
        return;
    }
    const bool first_status_this_session = !stream_status_ready_;
    stream_status_ready_ = true;
    exploration_rf_link_latched_ = true;
    stream_status_text_ = QString::fromStdString(msg->data);
    exploration_last_stream_status_at_ms_ = QDateTime::currentMSecsSinceEpoch();

    // Push the operator's persisted streaming-camera preference exactly
    // ONCE per launch session, on first /stream_status arrival (UDC's
    // signal that it's up and ready to switch pipelines).  Pushing
    // earlier (e.g. at launch start) racks up against UDC's startup
    // sequence and the request gets dropped; pushing later (e.g. on
    // Start Scan) means the operator sees the wrong camera during
    // exploration before any scan starts.
    //
    // No-op when the preference matches UDC's startup default ("left")
    // — UDC's own /stream_camera_status will arrive shortly with
    // "left" and the pill will lock in.  When the preference is
    // "right" we push so the operator's last choice carries across
    // OCU restarts.
    if (first_status_this_session) {
        QSettings settings(kSettingsOrgName, kSettingsAppName);
        const QString persisted =
            settings.value(kSettingsStreamingCameraKey,
                           QStringLiteral("left")).toString();
        if (persisted == QStringLiteral("right")) {
            publishStreamCameraSelection(persisted);
        }
    }
}

void AppShellWindow::onPlannerScanExecutionStatus(const std_msgs::msg::String::SharedPtr msg) {
    if (link_monitor_) {
        link_monitor_->stamp(LinkHealthMonitor::Source::ScanStatus);
    }
    if (!msg || !stage5_) {
        return;
    }
    const QString payload = QString::fromStdString(msg->data).trimmed();
    if (payload.startsWith(QStringLiteral("segment_complete"))) {
        // Phase A: bot reached final waypoint. DC end-and-save is in flight
        // on the controller side — do not advance to the next segment yet.
        stage5_->notifyScanSegmentCompleted();
        return;
    }
    if (payload.startsWith(QStringLiteral("segment_saved"))) {
        // Phase B: controller has confirmed /dc/end_and_save returned and the
        // GP8800 actuator has retracted. Safe to advance / prompt operator.
        stage5_->notifyScanSegmentSaved();
        return;
    }
}

void AppShellWindow::onExplorationOdom(const nav_msgs::msg::Odometry::SharedPtr msg) {
    odom_ready_ = true;
    if (link_monitor_) {
        link_monitor_->stamp(LinkHealthMonitor::Source::Odom);
    }
    if (!msg) {
        return;
    }

    const double x = msg->pose.pose.position.x;
    const double y = msg->pose.pose.position.y;
    const double z = msg->pose.pose.position.z;
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    exploration_latest_x_m_ = x;
    exploration_latest_y_m_ = y;
    exploration_latest_z_m_ = z;
    exploration_latest_vx_mps_ = msg->twist.twist.linear.x;
    exploration_latest_vy_mps_ = msg->twist.twist.linear.y;
    exploration_latest_yaw_rad_ = yawFromQuaternion(msg->pose.pose.orientation);
    exploration_last_odom_at_ms_ = now_ms;

    if (exploration_odom_samples_.empty() ||
        std::hypot(exploration_odom_samples_.back().x - x,
                   exploration_odom_samples_.back().y - y) > 0.03) {
        exploration_odom_samples_.push_back({x, y, now_ms});
        if (exploration_odom_samples_.size() > exploration_odom_max_points_) {
            const size_t remove_n =
                exploration_odom_samples_.size() - exploration_odom_max_points_;
            exploration_odom_samples_.erase(
                exploration_odom_samples_.begin(),
                exploration_odom_samples_.begin() + remove_n);
        }
    }

    pushPlannerTelemetrySnapshot();
}

void AppShellWindow::pushPlannerTelemetrySnapshot() {
    if (!stage5_) {
        return;
    }

    std::optional<f2c_cpp::PathState> pose;
    if (exploration_last_odom_at_ms_ > 0) {
        pose = f2c_cpp::PathState(
            f2c_cpp::Point2D(exploration_latest_x_m_, exploration_latest_y_m_),
            exploration_latest_yaw_rad_);
    }

    std::vector<f2c_cpp::Point2D> trail;
    trail.reserve(exploration_odom_samples_.size());
    for (const auto& sample : exploration_odom_samples_) {
        trail.emplace_back(sample.x, sample.y);
    }

    stage5_->setLiveRobotTelemetry(pose, trail);
    pushPlannerMotorsChipState();
}

void AppShellWindow::onDashboardBatteryStateChanged(double pct, bool stale) {
    // Cache the latest sample so goToStage4()/goToStage5() can seed
    // the top-bar pill on transition (the next dashboard publish may
    // be ≥1 s away).  Push to whichever stage is currently visible
    // immediately so a stale↔fresh transition is reflected without
    // waiting for the next stage entry.
    last_battery_pct_ = pct;
    last_battery_stale_ = stale;

    if (stage4_ && stack_ && stack_->currentWidget() == stage4_) {
        stage4_->setTopBatteryState(pct, stale);
    }
    if (stage5_ && stack_ && stack_->currentWidget() == stage5_) {
        stage5_->setTopBatteryState(pct, stale);
    }
}

void AppShellWindow::pushExplorationTopMotorsChipState() {
    if (!stage4_) {
        return;
    }

    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    const bool fresh_left =
        exploration_left_iq_at_ms_ > 0 && (now_ms - exploration_left_iq_at_ms_) <= kControllerStatusStaleMs;
    const bool fresh_right =
        exploration_right_iq_at_ms_ > 0 && (now_ms - exploration_right_iq_at_ms_) <= kControllerStatusStaleMs;
    const bool motors_armed =
        fresh_left && fresh_right &&
        exploration_left_axis_state_ == kOdriveAxisStateClosedLoopControl &&
        exploration_right_axis_state_ == kOdriveAxisStateClosedLoopControl;

    stage4_->setTopMotorsChipState(motors_armed ? QStringLiteral("MOTORS ARMED")
                                                : QStringLiteral("DISARMED"),
                                   motors_armed ? ExplorationScreen::ValueTone::Error
                                                : ExplorationScreen::ValueTone::Muted);
}

void AppShellWindow::pushPlannerMotorsChipState() {
    if (!stage5_) {
        return;
    }

    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    const bool fresh_left =
        exploration_left_iq_at_ms_ > 0 && (now_ms - exploration_left_iq_at_ms_) <= kControllerStatusStaleMs;
    const bool fresh_right =
        exploration_right_iq_at_ms_ > 0 && (now_ms - exploration_right_iq_at_ms_) <= kControllerStatusStaleMs;
    const bool motors_armed =
        fresh_left && fresh_right &&
        exploration_left_axis_state_ == kOdriveAxisStateClosedLoopControl &&
        exploration_right_axis_state_ == kOdriveAxisStateClosedLoopControl;

    stage5_->setTopMotorsChipState(motors_armed ? QStringLiteral("MOTORS ARMED")
                                                : QStringLiteral("DISARMED"),
                                   motors_armed ? PlannerScreen::ValueTone::Error
                                                : PlannerScreen::ValueTone::Muted);
}

// ---------- /udc/health: REC pill ----------

void AppShellWindow::onUdcHealthMessage(const std_msgs::msg::String::SharedPtr msg) {
    // Naive JSON state extractor — we only consume two fields
    // ("state" + "total_rows_enqueued"), so a real JSON dependency
    // is overkill.  The robot-side schema is owned by us
    // (UnifiedDataCollector::publishHealth + udc_supervisor.py) so the
    // exact byte format is stable.
    const QString payload = QString::fromStdString(msg->data);
    QString new_state = QStringLiteral("UNKNOWN");
    const int state_idx = payload.indexOf(QStringLiteral("\"state\":\""));
    if (state_idx >= 0) {
        const int start = state_idx + 9;
        const int end = payload.indexOf('"', start);
        if (end > start) {
            new_state = payload.mid(start, end - start);
        }
    }

    quint64 new_total_rows = 0;
    const int rows_idx = payload.indexOf(QStringLiteral("\"total_rows_enqueued\":"));
    if (rows_idx >= 0) {
        const int start = rows_idx + 22;
        int end = start;
        while (end < payload.size() && (payload[end].isDigit() || payload[end] == '-')) {
            ++end;
        }
        new_total_rows = payload.mid(start, end - start).toULongLong();
    }

    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    if (new_total_rows != udc_health_last_seen_total_rows_) {
        udc_health_last_row_advance_ms_ = now_ms;
        udc_health_last_seen_total_rows_ = new_total_rows;
    } else if (udc_health_last_row_advance_ms_ == 0) {
        udc_health_last_row_advance_ms_ = now_ms;
    }
    udc_health_total_rows_ = new_total_rows;
    udc_health_state_ = new_state;
    udc_health_last_msg_ms_ = now_ms;

    if (link_monitor_) {
        link_monitor_->stamp(LinkHealthMonitor::Source::UdcHealth);
    }
    pushUdcRecPillState();
}

bool AppShellWindow::isUdcDeadOrWedged() const {
    // Used by Start Scan gate.  Be conservative: only block on the
    // two unambiguously-bad states.  STARTING/UNKNOWN must NOT block
    // (false positives are very expensive — operator's words).
    return udc_health_state_ == QStringLiteral("DEAD_MAX_RESTARTS") ||
           udc_health_state_ == QStringLiteral("ERROR_THERMAL_WEDGED");
}

bool AppShellWindow::isRobotLinkOffline() const {
    // Hard-block RPCs in BOTH true OFFLINE and the RECONNECTING soft
    // window (operator confirmed Q1=a).  An RPC fired during
    // RECONNECTING would race with the Zenoh peer-rediscovery and
    // either drop on the floor or land 5-10 s late, neither of which
    // is acceptable mid-scan.
    if (!link_monitor_ || !link_monitor_->isArmed()) {
        return false;
    }
    const auto s = link_monitor_->state();
    return s == LinkHealthMonitor::State::Disconnected ||
           s == LinkHealthMonitor::State::Reconnecting;
}

bool AppShellWindow::isRobotLinkUnreachable() const {
    // Strict variant — only true OFFLINE.  Used by the Complete
    // Mission flow so RECONNECTING (probe says host is up) doesn't
    // pop the OfflineFinalizeDialog when a few seconds of patience
    // would have cleared the issue.
    return link_monitor_ && link_monitor_->isArmed() &&
           link_monitor_->state() == LinkHealthMonitor::State::Disconnected;
}

void AppShellWindow::showCommandDroppedToast(const QString& message) {
    // Dedup: same message within 1 s window collapses to a single toast.
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    if (message == command_toast_last_message_ &&
        now_ms - command_toast_last_shown_at_ms_ < 1000) {
        return;
    }
    command_toast_last_message_ = message;
    command_toast_last_shown_at_ms_ = now_ms;

    if (!command_toast_) {
        command_toast_ = new QFrame(central_root_ ? central_root_ : this);
        command_toast_->setObjectName("AppShellCommandToast");
        command_toast_->setStyleSheet(
            QStringLiteral("QFrame#AppShellCommandToast { background: rgba(220,38,38,0.95); "
                           "border: 1px solid #FCA5A5; border-radius: 10px; }"));
        command_toast_->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto* layout = new QHBoxLayout(command_toast_);
        layout->setContentsMargins(14, 8, 14, 8);
        layout->setSpacing(0);
        command_toast_label_ = new QLabel(command_toast_);
        command_toast_label_->setStyleSheet(
            QStringLiteral("color: #FFFBEB; font-family: 'Arimo'; font-size: 13px; "
                           "font-weight: 600; background: transparent;"));
        command_toast_label_->setWordWrap(false);
        layout->addWidget(command_toast_label_);
        command_toast_->raise();
        command_toast_->hide();

        command_toast_dismiss_timer_ = new QTimer(this);
        command_toast_dismiss_timer_->setSingleShot(true);
        command_toast_dismiss_timer_->setInterval(4000);
        connect(command_toast_dismiss_timer_, &QTimer::timeout,
                this, &AppShellWindow::hideCommandDroppedToast);
    }

    command_toast_label_->setText(message);
    command_toast_->adjustSize();
    if (QWidget* host = command_toast_->parentWidget()) {
        const int x = host->width() - command_toast_->width() - 24;
        const int y = 56;  // below window controls / OTA banner
        command_toast_->move(std::max(24, x), y);
    }
    command_toast_->show();
    command_toast_->raise();
    if (command_toast_dismiss_timer_) {
        command_toast_dismiss_timer_->start();
    }
}

void AppShellWindow::hideCommandDroppedToast() {
    if (command_toast_) {
        command_toast_->hide();
    }
}

void AppShellWindow::onLinkStateChanged(LinkHealthMonitor::State old_state,
                                        LinkHealthMonitor::State new_state,
                                        qint64 since_ms) {
    using State = LinkHealthMonitor::State;

    // Push the BOT pill text + tone to both stage screens.
    QString text;
    PlannerScreen::ValueTone planner_tone = PlannerScreen::ValueTone::Muted;
    ExplorationScreen::ValueTone expl_tone = ExplorationScreen::ValueTone::Muted;
    // `connected` toggles the screens' grey-out + button gating.
    // `reachable` toggles the heavier OFFLINE visual treatment
    // (banner + map halo).  See PlannerScreen::setLinkConnectionState
    // for the truth table.
    bool connected = true;
    bool reachable = true;

    switch (new_state) {
        case State::Idle:
            text = QStringLiteral("BOT ...");
            planner_tone = PlannerScreen::ValueTone::Muted;
            expl_tone = ExplorationScreen::ValueTone::Muted;
            connected = true;
            reachable = true;
            break;
        case State::Healthy:
            text = QStringLiteral("BOT LIVE");
            planner_tone = PlannerScreen::ValueTone::Good;
            expl_tone = ExplorationScreen::ValueTone::Good;
            connected = true;
            reachable = true;
            break;
        case State::Reconnecting:
            // Topics stale but probe says host is up — and (post-FPV-
            // stamp fix) we'd only land here if RTP video is also
            // gone, which means we're in a fully soft fade rather
            // than a Zenoh-only hiccup.  Pill text reflects
            // operator-facing semantics: "BOT LIVE - SYNCING" so the
            // operator knows the bot is believed alive (probe ok)
            // and controls are catching up, rather than panicking
            // them with "RECONNECTING".
            text = QString::fromLatin1("BOT LIVE - SYNCING %1s")
                       .arg(QString::number((since_ms + 500) / 1000));
            planner_tone = PlannerScreen::ValueTone::Warning;
            expl_tone = ExplorationScreen::ValueTone::Warning;
            connected = false;  // hard-block RPCs (operator answer Q1=a)
            reachable = true;   // suppress banner/halo + allow E-Stop bypass
            break;
        case State::Disconnected:
            text = QString::fromLatin1("BOT OFFLINE %1s")
                       .arg(QString::number((since_ms + 500) / 1000));
            planner_tone = PlannerScreen::ValueTone::Error;
            expl_tone = ExplorationScreen::ValueTone::Error;
            connected = false;
            reachable = false;
            break;
    }

    if (stage4_) {
        stage4_->setBotLinkPillState(text, expl_tone);
        stage4_->setLinkConnectionState(connected, reachable, since_ms);
    }
    if (stage5_) {
        stage5_->setBotLinkPillState(text, planner_tone);
        stage5_->setLinkConnectionState(connected, reachable, since_ms);
    }

    // Track the disconnect entry timestamp.  The "soft state resync"
    // hook only fires on true Disconnected -> Healthy transitions
    // (passing through Reconnecting is irrelevant — the controller
    // owns its own pause/resume across heartbeat loss; see
    // onRobotLinkRecovered).
    if (new_state == State::Disconnected && old_state != State::Disconnected) {
        link_disconnect_started_at_ms_ = QDateTime::currentMSecsSinceEpoch();
        link_recovery_resync_pending_ = true;
        qWarning("[AppShell] LinkHealth: state OFFLINE (%lld ms since last seen)",
                 static_cast<long long>(since_ms));
    } else if (new_state == State::Reconnecting && old_state == State::Healthy) {
        // Informational only — no visual banner, no resync hook.
        // Helps post-mortem grep for "we entered the soft window".
        qInfo("[AppShell] LinkHealth: state RECONNECTING (%lld ms since last seen, probe=reachable)",
              static_cast<long long>(since_ms));
    } else if (new_state == State::Healthy && link_recovery_resync_pending_) {
        link_recovery_resync_pending_ = false;
        const qint64 down_ms = link_disconnect_started_at_ms_ > 0
            ? QDateTime::currentMSecsSinceEpoch() - link_disconnect_started_at_ms_
            : 0;
        qInfo("[AppShell] LinkHealth: state HEALTHY after %lld ms offline — re-syncing soft state",
              static_cast<long long>(down_ms));
        onRobotLinkRecovered();
    }
}

void AppShellWindow::onRobotLinkRecovered() {
    // Log-only on reconnect — the robot-side controller already handles
    // pause/resume across heartbeat loss/recovery (see
    // mpc_accel_autonomous_controller.py heartbeat_callback +
    // _check_heartbeat_safety: zero-velocity + DC pause on loss,
    // _maybe_resume_dc on recovery).  Re-publishing autonomy_enable from
    // the OCU here would race with that path and could clobber the
    // operator's intent if they pressed E-Stop while disconnected.
    //
    // The slot stays wired so post-mortems can grep the log for the
    // recovery transition; no soft state is pushed.
    qInfo("[AppShell] onRobotLinkRecovered — controller heartbeat path owns resync");
}

void AppShellWindow::pushUdcRecPillState() {
    if (!stage4_ && !stage5_) {
        return;
    }

    // Map UDC health state -> (pill_text, tone).  Tone semantics match
    // the existing top-bar Signal/Lock chips: Good=green, Warning=
    // amber, Error=red, Muted=grey.
    QString text;
    enum Tone { Muted, Warning, Error, Good } tone = Muted;

    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    const bool stale = udc_health_last_msg_ms_ > 0 &&
                       (now_ms - udc_health_last_msg_ms_) > 3500;

    if (udc_health_state_ == QStringLiteral("DEAD_MAX_RESTARTS")) {
        text = QStringLiteral("REC FAILED");
        tone = Error;
    } else if (udc_health_state_ == QStringLiteral("ERROR_THERMAL_WEDGED")) {
        text = QStringLiteral("THERMAL WEDGED");
        tone = Error;
    } else if (stale) {
        text = QStringLiteral("REC STALE");
        tone = Warning;
    } else if (udc_health_state_ == QStringLiteral("RECORDING")) {
        text = QStringLiteral("REC");
        tone = Good;
        // Sub-state: rows aren't advancing while supposedly RECORDING.
        // 2 s window matches the per-row 1Hz drop diagnostic in UDC.
        if (udc_health_last_row_advance_ms_ > 0 &&
            (now_ms - udc_health_last_row_advance_ms_) > 2500) {
            text = QStringLiteral("REC NO ROWS");
            tone = Warning;
        }
    } else if (udc_health_state_ == QStringLiteral("PAUSED")) {
        text = QStringLiteral("REC PAUSED");
        tone = Warning;
    } else if (udc_health_state_ == QStringLiteral("STOPPED")) {
        text = QStringLiteral("REC IDLE");
        tone = Muted;
    } else if (udc_health_state_ == QStringLiteral("STARTING")) {
        text = QStringLiteral("REC STARTING");
        tone = Muted;
    } else {
        text = QStringLiteral("REC ...");
        tone = Muted;
    }

    auto toExplorationTone = [](Tone t) {
        switch (t) {
            case Good: return ExplorationScreen::ValueTone::Good;
            case Warning: return ExplorationScreen::ValueTone::Warning;
            case Error: return ExplorationScreen::ValueTone::Error;
            case Muted: default: return ExplorationScreen::ValueTone::Muted;
        }
    };
    auto toPlannerTone = [](Tone t) {
        switch (t) {
            case Good: return PlannerScreen::ValueTone::Good;
            case Warning: return PlannerScreen::ValueTone::Warning;
            case Error: return PlannerScreen::ValueTone::Error;
            case Muted: default: return PlannerScreen::ValueTone::Muted;
        }
    };
    if (stage4_) {
        stage4_->setTopRecPillState(text, toExplorationTone(tone));
    }
    if (stage5_) {
        stage5_->setTopRecPillState(text, toPlannerTone(tone));
    }
}

bool AppShellWindow::sendControllerMaxLinearVelocity(double max_linear_velocity_mps) {
    // Push the operator-selected cruise speed to the MPC controller's
    // `max_linear_velocity` AND `desired_linear_speed` ROS params. The
    // controller's `_on_parameter_change` callback fans both names into
    // the same `_set_navigation_speed()` setter today; we set both so
    // the controller's cruise target and hard cap stay in sync if the
    // controller ever splits them.
    //
    // Implementation: callback-form async_send_request, fire-and-forget
    // from this slot's perspective. Surface success/failure
    // ASYNCHRONOUSLY via BdrMessageBox::warning posted to the Qt main
    // thread from the response callback.
    //
    // History (do not regress): we previously tried two synchronous
    // patterns, both produced false-positive 2-second timeouts even
    // though the controller logged the param update within ~50 ms:
    //   1) `rclcpp::spin_until_future_complete(node, future, 2s)` —
    //      temp executor in rclcpp Humble doesn't reliably wake on the
    //      response when the same node is also spun by a Qt-tick
    //      `spin_some` elsewhere. ros2/rclcpp issues 1839 / 1990.
    //   2) `future.wait_for(...)` polled inside a manual `spin_some`
    //      loop on the same node — same symptom, same root cause:
    //      response is consumed by rclcpp internals but not paired with
    //      our pending future.
    // The callback variant works because the response handler is
    // dispatched by the node's existing live_fast_timer `spin_some`
    // (same path that delivers /dc/cancel_scan replies at
    // app_shell.cpp:1690). No polling, no future-completion race.
    //
    // The local client `set_params_client` is captured by value into
    // the response lambda so it OUTLIVES this function. Without that
    // capture the client would be destroyed when the function returns
    // and the response handler would never fire.
    //
    // Returns true on dispatch (caller proceeds to arm autonomy). The
    // controller applies the new cap synchronously inside its
    // _on_parameter_change before MPC's next iteration runs, so by the
    // time autonomy_enable's Bool is observed by the controller's
    // main loop the new speed is already live.
    if (!std::isfinite(max_linear_velocity_mps) || max_linear_velocity_mps <= 0.0) {
        return false;
    }
    if (!exploration_ros_node_) {
        ensureExplorationRosInterfaces();
    }
    if (!exploration_ros_node_ || !rclcpp::ok()) {
        BdrMessageBox::warning(
            this,
            QStringLiteral("Speed update failed"),
            QStringLiteral(
                "ROS interfaces are not initialized; the robot will use "
                "its current cruise speed instead of the operator-selected "
                "value. Re-press Start Scan to retry."));
        return false;
    }

    // Service name MUST match the launched node's ROS graph name. The
    // Python file calls `super().__init__("mpc_accel_controller")`, but
    // robot_complete and mpc_accel_autonomous_scan launch files pass
    // `name="mpc_accel_autonomous_controller"`, which overrides it. Use
    // the launch name or the SetParameters service is never found.
    auto set_params_client =
        exploration_ros_node_->create_client<rcl_interfaces::srv::SetParameters>(
            "/mpc_accel_autonomous_controller/set_parameters");

    // Synchronous 1.5 s wait for service discovery. This part doesn't
    // hit the future-completion bug — wait_for_service uses graph
    // events, not service responses. Worth keeping so we can surface a
    // crisp "controller not reachable" warning before dispatching a
    // request that would never get answered.
    if (!set_params_client->wait_for_service(std::chrono::milliseconds(1500))) {
        RCLCPP_WARN(rclcpp::get_logger("AppShellWindow"),
                    "/mpc_accel_autonomous_controller/set_parameters not "
                    "available after 1.5 s; cruise speed push skipped.");
        BdrMessageBox::warning(
            this,
            QStringLiteral("Speed update failed"),
            QString::fromUtf8(
                "Could not reach the autonomous controller's parameter "
                "service within 1.5 s. The robot will use its current "
                "cruise speed (most likely the launch-time default, not "
                "the %1 you selected). Verify the robot stack is up "
                "and re-press Start Scan to retry.")
                .arg(units::formatSpeed(max_linear_velocity_mps, 2)));
        return false;
    }

    auto request = std::make_shared<rcl_interfaces::srv::SetParameters::Request>();
    request->parameters.reserve(2);
    for (const char* name : {"max_linear_velocity", "desired_linear_speed"}) {
        rcl_interfaces::msg::Parameter p;
        p.name = name;
        p.value.type = rcl_interfaces::msg::ParameterType::PARAMETER_DOUBLE;
        p.value.double_value = max_linear_velocity_mps;
        request->parameters.push_back(std::move(p));
    }

    // Dispatch with callback. `set_params_client` and `request` are
    // captured by value so they outlive this function (the client owns
    // the pending request map; if it dies before the response, the
    // callback is never invoked). The lambda runs on the rclcpp
    // executor thread (the live_fast_timer's `spin_some`), so we hop
    // back to the Qt main thread via QMetaObject::invokeMethod before
    // touching any UI.
    set_params_client->async_send_request(
        request,
        [this, set_params_client, request, max_linear_velocity_mps](
            rclcpp::Client<rcl_interfaces::srv::SetParameters>::SharedFuture future) {
            QMetaObject::invokeMethod(
                this,
                [this, future, request, max_linear_velocity_mps]() mutable {
                    rcl_interfaces::srv::SetParameters::Response::SharedPtr response;
                    try {
                        response = future.get();
                    } catch (const std::exception& ex) {
                        RCLCPP_WARN(rclcpp::get_logger("AppShellWindow"),
                                    "SetParameters future threw: %s", ex.what());
                        BdrMessageBox::warning(
                            this,
                            QStringLiteral("Speed update failed"),
                            QString::fromUtf8(
                                "The cruise speed update raised an exception: "
                                "%1. The robot will use its current cruise speed.")
                                .arg(QString::fromUtf8(ex.what())));
                        return;
                    }

                    if (!response ||
                        response->results.size() != request->parameters.size()) {
                        BdrMessageBox::warning(
                            this,
                            QStringLiteral("Speed update failed"),
                            QStringLiteral(
                                "The autonomous controller returned an unexpected "
                                "response to the cruise speed update. The robot "
                                "will use its current cruise speed."));
                        return;
                    }

                    QStringList rejected;
                    for (size_t i = 0; i < response->results.size(); ++i) {
                        if (!response->results[i].successful) {
                            const std::string& name = request->parameters[i].name;
                            const std::string& reason = response->results[i].reason;
                            RCLCPP_WARN(
                                rclcpp::get_logger("AppShellWindow"),
                                "mpc_accel_autonomous_controller rejected %s=%.2f: %s",
                                name.c_str(), max_linear_velocity_mps,
                                reason.c_str());
                            rejected << QString::fromStdString(name + ": " + reason);
                        }
                    }
                    if (!rejected.isEmpty()) {
                        BdrMessageBox::warning(
                            this,
                            QStringLiteral("Speed update partially rejected"),
                            QString::fromUtf8(
                                "The autonomous controller rejected the "
                                "following cruise speed parameters:\n\n%1\n\n"
                                "The robot may run at an unintended speed.")
                                .arg(rejected.join(QStringLiteral("\n"))));
                    }
                },
                Qt::QueuedConnection);
        });

    // Best-effort: dispatch confirmed. Caller proceeds to arm autonomy.
    // Any failure surfaces asynchronously via BdrMessageBox in the
    // response callback above.
    return true;
}

void AppShellWindow::sendDataCollectorSessionMetadata(
    const QString& building_name,
    const QString& operator_name,
    const QString& units_preference,
    std::function<void(bool ok)> on_complete) {
    // Mirrors the design of sendControllerMaxLinearVelocity above:
    // a freshly-built SetParameters client captured by value into the
    // response lambda so it outlives this function, and a Qt main
    // thread hop via QMetaObject::invokeMethod before any UI work.
    //
    // Service name comes from the coordinator node name in
    // pilot_control/scripts/data_collection_coordinator.py
    // (`super().__init__('data_collection_coordinator')`).
    //
    // Push raw strings — coordinator owns the slugifier
    // (slugify_building_name in the same file). Pushing both raw and
    // slug here would just create a divergence vector if the two
    // slug implementations ever drift. The C++ slugifier in
    // MissionMetadataDialog::slugify is QSettings/UI-preview only.
    //
    // Per Stage 4 plan: caller hard-blocks autonomy_enable on
    // on_complete(false) so the operator can retry, rather than
    // letting the section land under BDR_test/.
    auto invoke_complete = [this, on_complete](bool ok) {
        if (!on_complete) {
            return;
        }
        QMetaObject::invokeMethod(
            this,
            [on_complete, ok]() { on_complete(ok); },
            Qt::QueuedConnection);
    };

    if (!exploration_ros_node_) {
        ensureExplorationRosInterfaces();
    }
    if (!exploration_ros_node_ || !rclcpp::ok()) {
        BdrMessageBox::warning(
            this,
            QStringLiteral("Session metadata update failed"),
            QStringLiteral(
                "ROS interfaces are not initialized; the building / "
                "operator / units selection cannot be sent to the robot. "
                "Re-press Start Scan to retry."));
        invoke_complete(false);
        return;
    }

    auto set_params_client =
        exploration_ros_node_->create_client<rcl_interfaces::srv::SetParameters>(
            "/data_collection_coordinator/set_parameters");

    if (!set_params_client->wait_for_service(std::chrono::milliseconds(1500))) {
        RCLCPP_WARN(rclcpp::get_logger("AppShellWindow"),
                    "/data_collection_coordinator/set_parameters not "
                    "available after 1.5 s; session metadata push skipped.");
        BdrMessageBox::warning(
            this,
            QStringLiteral("Session metadata update failed"),
            QStringLiteral(
                "Could not reach the data collection coordinator's "
                "parameter service within 1.5 s. The robot stack is "
                "probably not fully up. Verify the launch sequence "
                "completed and re-press Start Scan to retry."));
        invoke_complete(false);
        return;
    }

    auto make_string_param = [](const char* name, const QString& value) {
        rcl_interfaces::msg::Parameter p;
        p.name = name;
        p.value.type = rcl_interfaces::msg::ParameterType::PARAMETER_STRING;
        p.value.string_value = value.toStdString();
        return p;
    };

    auto request = std::make_shared<rcl_interfaces::srv::SetParameters::Request>();
    request->parameters.reserve(3);
    request->parameters.push_back(make_string_param("building_name", building_name));
    request->parameters.push_back(make_string_param("operator_name", operator_name));
    request->parameters.push_back(make_string_param("units_preference", units_preference));

    set_params_client->async_send_request(
        request,
        [this, set_params_client, request, invoke_complete](
            rclcpp::Client<rcl_interfaces::srv::SetParameters>::SharedFuture future) {
            QMetaObject::invokeMethod(
                this,
                [this, future, request, invoke_complete]() mutable {
                    rcl_interfaces::srv::SetParameters::Response::SharedPtr response;
                    try {
                        response = future.get();
                    } catch (const std::exception& ex) {
                        RCLCPP_WARN(rclcpp::get_logger("AppShellWindow"),
                                    "Coordinator SetParameters future threw: %s",
                                    ex.what());
                        BdrMessageBox::warning(
                            this,
                            QStringLiteral("Session metadata update failed"),
                            QString::fromUtf8(
                                "The session metadata update raised an "
                                "exception: %1.")
                                .arg(QString::fromUtf8(ex.what())));
                        invoke_complete(false);
                        return;
                    }

                    if (!response ||
                        response->results.size() != request->parameters.size()) {
                        BdrMessageBox::warning(
                            this,
                            QStringLiteral("Session metadata update failed"),
                            QStringLiteral(
                                "The data collection coordinator returned "
                                "an unexpected response to the session "
                                "metadata update."));
                        invoke_complete(false);
                        return;
                    }

                    QStringList rejected;
                    for (size_t i = 0; i < response->results.size(); ++i) {
                        if (!response->results[i].successful) {
                            const std::string& name = request->parameters[i].name;
                            const std::string& reason = response->results[i].reason;
                            RCLCPP_WARN(
                                rclcpp::get_logger("AppShellWindow"),
                                "data_collection_coordinator rejected %s: %s",
                                name.c_str(), reason.c_str());
                            rejected << QString::fromStdString(name + ": " + reason);
                        }
                    }
                    if (!rejected.isEmpty()) {
                        BdrMessageBox::warning(
                            this,
                            QStringLiteral("Session metadata update rejected"),
                            QString::fromUtf8(
                                "The data collection coordinator rejected "
                                "the following session metadata "
                                "parameters:\n\n%1")
                                .arg(rejected.join(QStringLiteral("\n"))));
                        invoke_complete(false);
                        return;
                    }

                    invoke_complete(true);
                },
                Qt::QueuedConnection);
        });
}

void AppShellWindow::onExplorationLocalNavGrid(
    const std_msgs::msg::UInt8MultiArray::SharedPtr msg) {
    if (!msg || msg->data.size() != static_cast<size_t>(kLocalNavGridBytes)) {
        return;
    }
    exploration_local_nav_grid_bytes_ =
        QByteArray(reinterpret_cast<const char*>(msg->data.data()), kLocalNavGridBytes);
    exploration_local_nav_grid_ready_ = true;
    exploration_last_local_nav_grid_at_ms_ = QDateTime::currentMSecsSinceEpoch();
}

void AppShellWindow::onExplorationThermalThumb(
    const std_msgs::msg::UInt8MultiArray::SharedPtr msg) {
    if (!msg || msg->data.size() != static_cast<size_t>(kThermalThumbBytes)) {
        return;
    }
    exploration_thermal_thumb_bytes_ =
        QByteArray(reinterpret_cast<const char*>(msg->data.data()), kThermalThumbBytes);
    exploration_thermal_thumb_ready_ = true;
    exploration_last_thermal_thumb_at_ms_ = QDateTime::currentMSecsSinceEpoch();
}

void AppShellWindow::onExplorationThermalSummary(const std_msgs::msg::String::SharedPtr msg) {
    if (!msg) {
        return;
    }
    const QString payload = QString::fromStdString(msg->data).trimmed();
    if (payload.isEmpty()) {
        return;
    }

    double max_c = 0.0;
    double avg_c = 0.0;
    double min_c = 0.0;
    if (!parseKeyValueDouble(payload, "max_c", &max_c) ||
        !parseKeyValueDouble(payload, "avg_c", &avg_c) ||
        !parseKeyValueDouble(payload, "min_c", &min_c)) {
        return;
    }

    exploration_thermal_max_c_ = max_c;
    exploration_thermal_avg_c_ = avg_c;
    exploration_thermal_min_c_ = min_c;
    exploration_thermal_summary_ready_ = true;
    exploration_last_thermal_summary_at_ms_ = QDateTime::currentMSecsSinceEpoch();
}

void AppShellWindow::onExplorationLeftControllerStatus(
    const odrive_can::msg::ControllerStatus::SharedPtr msg) {
    if (link_monitor_) {
        link_monitor_->stamp(LinkHealthMonitor::Source::ControllerStatus);
    }
    if (!msg) {
        return;
    }
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    exploration_left_iq_measured_ = msg->iq_measured;
    exploration_left_axis_state_ = msg->axis_state;
    exploration_left_iq_at_ms_ = now_ms;
    updateExplorationMotorSampleFromLatest();
    pushExplorationTopMotorsChipState();
    pushPlannerMotorsChipState();
}

void AppShellWindow::onExplorationRightControllerStatus(
    const odrive_can::msg::ControllerStatus::SharedPtr msg) {
    if (link_monitor_) {
        link_monitor_->stamp(LinkHealthMonitor::Source::ControllerStatus);
    }
    if (!msg) {
        return;
    }
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    exploration_right_iq_measured_ = msg->iq_measured;
    exploration_right_axis_state_ = msg->axis_state;
    exploration_right_iq_at_ms_ = now_ms;
    updateExplorationMotorSampleFromLatest();
    pushExplorationTopMotorsChipState();
    pushPlannerMotorsChipState();
}

void AppShellWindow::updateExplorationMotorSampleFromLatest() {
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    const bool fresh_left = exploration_left_iq_at_ms_ > 0 &&
                            (now_ms - exploration_left_iq_at_ms_) <= kControllerStatusStaleMs;
    const bool fresh_right = exploration_right_iq_at_ms_ > 0 &&
                             (now_ms - exploration_right_iq_at_ms_) <= kControllerStatusStaleMs;
    if (!fresh_left || !fresh_right) {
        return;
    }

    const double current_abs_iq =
        0.5 * (std::abs(exploration_left_iq_measured_) + std::abs(exploration_right_iq_measured_));
    if (!std::isfinite(current_abs_iq)) {
        return;
    }

    if (exploration_scan_started_at_ms_ > 0 && !map_save_or_download_in_progress_) {
        exploration_motor_current_scan_samples_.push_back(current_abs_iq);
        if (exploration_motor_current_scan_samples_.size() > exploration_motor_current_max_samples_) {
            const size_t remove_n =
                exploration_motor_current_scan_samples_.size() - exploration_motor_current_max_samples_;
            exploration_motor_current_scan_samples_.erase(
                exploration_motor_current_scan_samples_.begin(),
                exploration_motor_current_scan_samples_.begin() + remove_n);
        }
    }

    if (!exploration_prev_scan_baseline_available_) {
        exploration_motor_spike_streak_ = 0;
        exploration_motor_warning_active_ = false;
        return;
    }

    const double median = std::max(0.0, exploration_prev_scan_median_abs_iq_);
    const double mad = std::max(0.01, exploration_prev_scan_mad_abs_iq_);
    const double threshold = median + std::max({3.0 * mad, 0.35 * median, 1.0});

    if (current_abs_iq > threshold) {
        ++exploration_motor_spike_streak_;
    } else {
        exploration_motor_spike_streak_ = std::max(0, exploration_motor_spike_streak_ - 1);
    }
    exploration_motor_warning_active_ = exploration_motor_spike_streak_ >= 3;
}

void AppShellWindow::loadPreviousScanMotorBaseline() {
    exploration_prev_scan_baseline_available_ = false;
    exploration_prev_scan_median_abs_iq_ = 0.0;
    exploration_prev_scan_mad_abs_iq_ = 0.0;

    QSettings settings(f2c_cpp::kSettingsOrgName, f2c_cpp::kSettingsAppName);
    bool median_ok = false;
    bool mad_ok = false;
    const double median = settings.value("exploration/motor_baseline_median_abs_iq").toDouble(&median_ok);
    const double mad = settings.value("exploration/motor_baseline_mad_abs_iq").toDouble(&mad_ok);
    if (!median_ok || !mad_ok || !std::isfinite(median) || !std::isfinite(mad) ||
        median <= 0.0 || mad < 0.0) {
        return;
    }

    exploration_prev_scan_median_abs_iq_ = median;
    exploration_prev_scan_mad_abs_iq_ = mad;
    exploration_prev_scan_baseline_available_ = true;
}

void AppShellWindow::persistCurrentScanMotorBaseline() {
    if (exploration_motor_current_scan_samples_.size() < 30) {
        return;
    }

    const double median = computeMedian(exploration_motor_current_scan_samples_);
    const double mad = computeMad(exploration_motor_current_scan_samples_, median);
    if (!std::isfinite(median) || !std::isfinite(mad) || median <= 0.0 || mad < 0.0) {
        return;
    }

    QSettings settings(f2c_cpp::kSettingsOrgName, f2c_cpp::kSettingsAppName);
    settings.setValue("exploration/motor_baseline_median_abs_iq", median);
    settings.setValue("exploration/motor_baseline_mad_abs_iq", mad);
    settings.setValue("exploration/motor_baseline_sample_count",
                      static_cast<qulonglong>(exploration_motor_current_scan_samples_.size()));
    settings.setValue("exploration/motor_baseline_updated_at_ms", QDateTime::currentMSecsSinceEpoch());

    exploration_prev_scan_median_abs_iq_ = median;
    exploration_prev_scan_mad_abs_iq_ = mad;
    exploration_prev_scan_baseline_available_ = true;
}

void AppShellWindow::loadExplorationRfConfigForActiveRobot() {
    exploration_rf_config_ready_ = false;
    exploration_rf_radio_ip_.clear();
    exploration_rf_snmp_ro_community_.clear();
    exploration_rf_rssi_oid_.clear();
    exploration_rf_snr_oid_.clear();

    QString active_robot_id = robot_id_.trimmed();
    if (active_robot_id.isEmpty()) {
        QSettings settings(f2c_cpp::kSettingsOrgName, f2c_cpp::kSettingsAppName);
        active_robot_id = settings.value("setup/robot_id", "").toString().trimmed();
    }
    if (active_robot_id.isEmpty()) {
        return;
    }

    RobotRegistry registry;
    QString registry_error;
    if (!registry.load(&registry_error)) {
        return;
    }

    const auto profile_opt = registry.findById(active_robot_id);
    if (!profile_opt.has_value()) {
        return;
    }

    const RobotProfile& profile = profile_opt.value();
    const QString radio_ip = profile.radio_ip.trimmed();
    const QString ro_community = profile.snmp_ro_community.trimmed();
    const QString rssi_oid = profile.snmp_rssi_oid.trimmed();
    if (radio_ip.isEmpty() || ro_community.isEmpty() || rssi_oid.isEmpty()) {
        return;
    }

    exploration_rf_radio_ip_ = radio_ip;
    exploration_rf_snmp_ro_community_ = ro_community;
    exploration_rf_rssi_oid_ = rssi_oid;
    exploration_rf_snr_oid_ = profile.snmp_snr_oid.trimmed();
    exploration_rf_config_ready_ = true;
}

void AppShellWindow::requestExplorationRfProbe() {
    if (!exploration_rf_probe_proc_) {
        return;
    }
    if (!stage4_ || stack_->currentWidget() != stage4_) {
        return;
    }
    if (exploration_rf_probe_in_flight_ ||
        exploration_rf_probe_proc_->state() != QProcess::NotRunning) {
        return;
    }
    if (!exploration_rf_config_ready_) {
        return;
    }

    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    if (exploration_last_rf_probe_at_ms_ > 0 &&
        (now_ms - exploration_last_rf_probe_at_ms_) < 900) {
        return;
    }

    QStringList args;
    args << "-v2c"
         << "-c"
         << exploration_rf_snmp_ro_community_
         << "-Oqv"
         << "-t"
         << "1"
         << "-r"
         << "0"
         << exploration_rf_radio_ip_
         << exploration_rf_rssi_oid_;

    exploration_rf_probe_in_flight_ = true;
    exploration_last_rf_probe_at_ms_ = now_ms;
    exploration_rf_probe_proc_->start("snmpget", args);
}

void AppShellWindow::handleExplorationRfProbeFinished(int exit_code,
                                                      QProcess::ExitStatus exit_status) {
    Q_UNUSED(exit_code);
    exploration_rf_probe_in_flight_ = false;
    if (!exploration_rf_probe_proc_) {
        return;
    }

    if (exit_status != QProcess::NormalExit || exploration_rf_probe_proc_->exitCode() != 0) {
        return;
    }

    const QString output =
        QString::fromUtf8(exploration_rf_probe_proc_->readAllStandardOutput()).trimmed();
    if (output.isEmpty()) {
        return;
    }

    const QStringList lines = output.split('\n', Qt::SkipEmptyParts);
    const QString rssi_line = lines.isEmpty() ? output : lines.constLast();
    int parsed_rssi_dbm = 0;
    if (!parseTrailingInteger(rssi_line, &parsed_rssi_dbm)) {
        return;
    }

    exploration_rf_rssi_dbm_ = parsed_rssi_dbm;
    exploration_rf_metric_ready_ = true;
    exploration_last_rf_success_at_ms_ = QDateTime::currentMSecsSinceEpoch();
}

void AppShellWindow::requestExplorationStorageProbe() {
    if (!exploration_storage_probe_proc_ || !stage4_) {
        return;
    }
    if (exploration_storage_probe_in_flight_ ||
        exploration_storage_probe_proc_->state() != QProcess::NotRunning) {
        return;
    }

    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    if (exploration_last_storage_probe_at_ms_ > 0 &&
        (now_ms - exploration_last_storage_probe_at_ms_) < 60000) {
        return;
    }

    const ResolvedRobotSshTarget ssh_target = resolveRobotSshForRemoteOps();
    if (ssh_target.host.trimmed().isEmpty()) {
        return;
    }

    QStringList args;
    args << "-o"
         << "ConnectTimeout=8"
         << "-o"
         << "StrictHostKeyChecking=no"
         << "-o"
         << "LogLevel=ERROR"
         << "-o"
         << "UserKnownHostsFile=/dev/null"
         << "-o"
         << "BatchMode=yes"
         << sshUserHostSpec(ssh_target)
         << "df -h /R_DATA/";

    exploration_storage_probe_in_flight_ = true;
    exploration_storage_probe_proc_->start("ssh", args);
}

void AppShellWindow::handleExplorationStorageProbeFinished(int exit_code,
                                                           QProcess::ExitStatus exit_status) {
    Q_UNUSED(exit_code);
    exploration_storage_probe_in_flight_ = false;
    exploration_last_storage_probe_at_ms_ = QDateTime::currentMSecsSinceEpoch();
    if (!exploration_storage_probe_proc_) {
        return;
    }

    if (exit_status != QProcess::NormalExit || exploration_storage_probe_proc_->exitCode() != 0) {
        exploration_storage_ready_ = false;
        exploration_storage_text_ = QStringLiteral("N/A");
        return;
    }

    const QString output =
        QString::fromUtf8(exploration_storage_probe_proc_->readAllStandardOutput()).trimmed();
    const QStringList lines = output.split('\n', Qt::SkipEmptyParts);
    QString data_line;
    for (int i = lines.size() - 1; i >= 0; --i) {
        const QString candidate = lines.at(i).trimmed();
        if (!candidate.startsWith("Filesystem", Qt::CaseInsensitive)) {
            data_line = candidate;
            break;
        }
    }
    if (data_line.isEmpty()) {
        exploration_storage_ready_ = false;
        exploration_storage_text_ = QStringLiteral("N/A");
        return;
    }

    const QStringList cols = data_line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    if (cols.size() < 5) {
        exploration_storage_ready_ = false;
        exploration_storage_text_ = QStringLiteral("N/A");
        return;
    }

    const QString avail = cols.at(3);
    const QString used_pct = cols.at(4);
    exploration_storage_text_ = QString("%1 free (%2 used)").arg(avail, used_pct);
    exploration_storage_ready_ = true;
}

void AppShellWindow::onExplorationLiveFastTick() {
    if (exploration_ros_node_ && rclcpp::ok()) {
        rclcpp::spin_some(exploration_ros_node_);
    }
    pushExplorationTelemetryToUiFast();
}

void AppShellWindow::onExplorationLiveSlowTick() {
    requestExplorationRfProbe();
    requestExplorationStorageProbe();
    pushExplorationTelemetryToUiSlow();
    // Stage 6 freeze detector: stamp LinkHealthMonitor::Source::FpvFrame
    // when the FPV pipeline has delivered a frame in the last 1 s.  The
    // FPV stream is RTP/UDP and independent of Zenoh, so when ROS dies
    // but FPV stays alive the BOT pill should reflect "video healthy".
    // When BOTH ROS and FPV freeze, that's the catastrophic case where
    // the operator must use the Complete Mission / SSH-offline path.
    // FPV proof-of-life stamp.  RTP/UDP video keeps flowing through
    // brief Microhard radio fades (it's stateless), while Zenoh ROS
    // topics can hang for 30+ s on a stale TCP socket.  As long as
    // SOMETHING is decoding RTP frames, the bot is verifiably alive
    // end-to-end and the link counts as Healthy regardless of Zenoh
    // peer state.
    //
    // CRITICAL: read from BOTH stages.  Stage 4 (Exploration) has its
    // own FPV widget; Stage 5 (Mission Planner / Scan) has a separate
    // `scan_camera_view_` instance.  Pre-fix this slot only read
    // stage4_, so once the operator advanced to Stage 5 the stamp went
    // stale instantly even with the camera streaming live frames into
    // stage5_'s widget — that produced the false-positive RECONNECTING
    // flap during normal Zenoh peer-rediscovery on Stage 5 (terminal
    // log lines 386-389, 2026-05-12 session).
    if (link_monitor_ && link_monitor_->isArmed()) {
        const qint64 stage4_last =
            stage4_ ? stage4_->lastFpvFrameWallMs() : 0;
        const qint64 stage5_last =
            stage5_ ? stage5_->lastScanFpvFrameWallMs() : 0;
        const qint64 fpv_last = std::max(stage4_last, stage5_last);
        if (fpv_last > 0) {
            const qint64 age = QDateTime::currentMSecsSinceEpoch() - fpv_last;
            if (age <= 2000) {
                link_monitor_->stamp(LinkHealthMonitor::Source::FpvFrame);
            }
        }
    }
    // Refresh BOT pill counter while in Reconnecting / Disconnected
    // so the "Xs" age in "BOT LIVE - SYNCING X s" / "BOT OFFLINE X s"
    // ticks up.  No-op when state is Healthy / Idle (those have
    // counter-less pill text).
    if (link_monitor_ && link_monitor_->isArmed()) {
        const auto state = link_monitor_->state();
        if (state == LinkHealthMonitor::State::Disconnected ||
            state == LinkHealthMonitor::State::Reconnecting) {
            const qint64 since = link_monitor_->msSinceLastSeen();
            onLinkStateChanged(state, state, since >= 0 ? since : 0);
        }
    }
}

void AppShellWindow::pushExplorationTelemetryToUiFast() {
    if (!stage4_) {
        return;
    }
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    const bool odom_fresh = exploration_last_odom_at_ms_ > 0 &&
                            (now_ms - exploration_last_odom_at_ms_) <= 1500;
    const bool thermal_thumb_fresh = exploration_thermal_thumb_ready_ &&
                                     exploration_last_thermal_thumb_at_ms_ > 0 &&
                                     (now_ms - exploration_last_thermal_thumb_at_ms_) <=
                                         kThermalThumbStaleMs;
    const bool local_nav_fresh = exploration_local_nav_grid_ready_ &&
                                 exploration_last_local_nav_grid_at_ms_ > 0 &&
                                 (now_ms - exploration_last_local_nav_grid_at_ms_) <=
                                     kLocalNavGridStaleMs;
    const double speed_mps =
        odom_fresh ? std::hypot(exploration_latest_vx_mps_, exploration_latest_vy_mps_) : 0.0;
    stage4_->setTelemetrySpeedMps(speed_mps);
    // Stage 5 uses the same odom-derived speed for ETA when a scan is active
    // (PlannerScreen::effectiveScanSpeedMps falls back to the cached
    // controller max_linear_velocity ROS param when the robot is idle).
    if (stage5_) {
        stage5_->setLiveRobotSpeedMps(speed_mps);
    }

    if (!exploration_launch_ready_) {
        stage4_->setThermalThumbnailStale(false);
    } else if (exploration_thermal_thumb_ready_ &&
               exploration_thermal_thumb_bytes_.size() == kThermalThumbBytes) {
        stage4_->setThermalThumbnailData(exploration_thermal_thumb_bytes_);
        stage4_->setThermalThumbnailStale(!thermal_thumb_fresh);
    } else {
        stage4_->setThermalThumbnailStale(false);
    }

    if (!exploration_launch_ready_) {
        stage4_->resetNavigationMap();
        return;
    }

    if (!exploration_local_nav_grid_ready_ || exploration_local_nav_grid_bytes_.size() != kLocalNavGridBytes) {
        stage4_->resetNavigationMap();
        return;
    }

    stage4_->setNavigationMapData(exploration_local_nav_grid_bytes_);
    stage4_->setNavigationMapYawRadians(exploration_latest_yaw_rad_);
    stage4_->setNavigationMapStale(!local_nav_fresh);
}

void AppShellWindow::pushExplorationTelemetryToUiSlow() {
    if (!stage4_ && !stage5_) {
        return;
    }

    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    const bool odom_fresh = exploration_last_odom_at_ms_ > 0 &&
                            (now_ms - exploration_last_odom_at_ms_) <= 1500;
    const bool rf_metric_fresh = exploration_rf_metric_ready_ &&
                                 exploration_last_rf_success_at_ms_ > 0 &&
                                 (now_ms - exploration_last_rf_success_at_ms_) <= 5000;
    const bool thermal_metric_fresh = exploration_thermal_summary_ready_ &&
                                      exploration_last_thermal_summary_at_ms_ > 0 &&
                                      (now_ms - exploration_last_thermal_summary_at_ms_) <= 5000;
    const bool motor_fresh = exploration_left_iq_at_ms_ > 0 && exploration_right_iq_at_ms_ > 0 &&
                             (now_ms - exploration_left_iq_at_ms_) <= 1500 &&
                             (now_ms - exploration_right_iq_at_ms_) <= 1500;

    SharedTopStatusState top_status = computeSharedTopStatus(exploration_launch_ready_,
                                                              exploration_launch_in_progress_,
                                                              exploration_launch_failed_,
                                                              laptop_launch_confirmed_,
                                                              robot_launch_confirmed_,
                                                              laptop_launch_started_,
                                                              robot_launch_started_,
                                                              local_zenoh_ready_,
                                                              rf_metric_fresh,
                                                              exploration_rf_rssi_dbm_);

    // Layered signal fallback — when SNMP RSSI is missing (no probe
    // result yet, snmpget binary missing, radio not responding to
    // SNMP, etc.) but a scan is armed and we have a real
    // LinkHealthMonitor verdict, surface that instead of the
    // permanent "Signal unavailable" muted text.  The dBm reading is
    // still preferred when fresh (richer info).
    //
    // Mapping:
    //   Healthy      -> "Connected"   Good
    //   Reconnecting -> "Reconnecting" Warning
    //   Disconnected -> "Offline"     Error
    //   Idle         -> leave as "Signal unavailable" (pre-launch)
    if (!rf_metric_fresh && link_monitor_) {
        const auto link_state = link_monitor_->state();
        switch (link_state) {
            case LinkHealthMonitor::State::Healthy:
                top_status.signal_text = QStringLiteral("Connected");
                top_status.signal_tone = SharedTopStatusTone::Good;
                break;
            case LinkHealthMonitor::State::Reconnecting:
                top_status.signal_text = QStringLiteral("Reconnecting");
                top_status.signal_tone = SharedTopStatusTone::Warning;
                break;
            case LinkHealthMonitor::State::Disconnected:
                top_status.signal_text = QStringLiteral("Offline");
                top_status.signal_tone = SharedTopStatusTone::Error;
                break;
            case LinkHealthMonitor::State::Idle:
                // Pre-launch — keep the placeholder.  Operator hasn't
                // armed yet, so "Signal unavailable" is honest.
                break;
        }
    }

    if (stage4_) {
        stage4_->setTopLockChipState(top_status.lock_text, toExplorationTone(top_status.lock_tone));
        stage4_->setTopSignalState(top_status.signal_text, toExplorationTone(top_status.signal_tone));
        pushExplorationTopMotorsChipState();
    }
    if (stage5_) {
        stage5_->setTopLockChipState(top_status.lock_text, toPlannerTone(top_status.lock_tone));
        stage5_->setTopSignalState(top_status.signal_text, toPlannerTone(top_status.signal_tone));
        pushPlannerMotorsChipState();
    }
    // Re-evaluate REC pill on every telemetry tick so STALE/NO ROWS
    // detection (driven by wall-clock deltas in pushUdcRecPillState)
    // updates even when /udc/health stops arriving.
    pushUdcRecPillState();

    if (!stage4_) {
        return;
    }

    stage4_->setTelemetryPositionMeters(exploration_latest_x_m_, exploration_latest_y_m_);
    stage4_->setTelemetryAltitudeMeters(exploration_latest_z_m_);

    int elapsed_sec = 0;
    if (exploration_scan_started_at_ms_ > 0) {
        elapsed_sec = static_cast<int>(std::max<qint64>(0, (now_ms - exploration_scan_started_at_ms_) / 1000));
    }
    stage4_->setTelemetryScanTimeSeconds(elapsed_sec);

    QString motors_text = "Unknown";
    ExplorationScreen::ValueTone motors_tone = ExplorationScreen::ValueTone::Muted;
    if (exploration_prev_scan_baseline_available_ && motor_fresh) {
        if (exploration_motor_warning_active_) {
            motors_text = "Warning";
            motors_tone = ExplorationScreen::ValueTone::Warning;
        } else {
            motors_text = "Good";
            motors_tone = ExplorationScreen::ValueTone::Good;
        }
    }

    const QString lidar_text = odom_fresh ? "Ready" : "No data";
    const ExplorationScreen::ValueTone lidar_tone =
        odom_fresh ? ExplorationScreen::ValueTone::Good : ExplorationScreen::ValueTone::Muted;

    QString rf_text = "Signal unavailable";
    ExplorationScreen::ValueTone rf_tone = ExplorationScreen::ValueTone::Muted;
    if (rf_metric_fresh) {
        if (exploration_rf_rssi_dbm_ >= -65) {
            rf_text = exploration_rf_link_latched_ ? "Connected and stable" : "Strong signal";
            rf_tone = ExplorationScreen::ValueTone::Good;
        } else if (exploration_rf_rssi_dbm_ >= -80) {
            rf_text = exploration_rf_link_latched_ ? "Medium connectivity" : "Medium signal";
            rf_tone = ExplorationScreen::ValueTone::Warning;
        } else {
            rf_text = exploration_rf_link_latched_ ? "Poor connectivity" : "Weak signal";
            rf_tone = ExplorationScreen::ValueTone::Error;
        }
    }

    if (!exploration_launch_ready_) {
        stage4_->setThermalHidden();
    } else if (thermal_metric_fresh) {
        QString thermal_state_text = "Normal";
        ExplorationScreen::ValueTone thermal_state_tone = ExplorationScreen::ValueTone::Good;
        if (exploration_thermal_max_c_ > 40.0) {
            thermal_state_text = "Hot";
            thermal_state_tone = ExplorationScreen::ValueTone::Error;
        } else if (exploration_thermal_max_c_ > 30.0) {
            thermal_state_text = "Elevated";
            thermal_state_tone = ExplorationScreen::ValueTone::Warning;
        }
        stage4_->setThermalSummary(
            exploration_thermal_max_c_,
            exploration_thermal_avg_c_,
            exploration_thermal_min_c_,
            thermal_state_text,
            thermal_state_tone);
    } else if (!exploration_thermal_summary_ready_) {
        stage4_->setThermalUnavailable();
    }

    const QString storage_text = exploration_storage_ready_ ? exploration_storage_text_ : QStringLiteral("N/A");
    const ExplorationScreen::ValueTone storage_tone = exploration_storage_ready_
                                                          ? ExplorationScreen::ValueTone::Good
                                                          : ExplorationScreen::ValueTone::Muted;

    stage4_->setHardwareStatus(motors_text,
                               motors_tone,
                               lidar_text,
                               lidar_tone,
                               rf_text,
                               rf_tone,
                               storage_text,
                               storage_tone);
}

}  // namespace f2c_cpp

