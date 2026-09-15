#include "satellite_mission_controller.hpp"

#include "launch_env.hpp"
#include "robot_registry.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFileInfoList>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <algorithm>

namespace f2c_cpp {

namespace {

QStringList sshBaseArgs(const RobotTarget& target) {
    return QStringList()
           << "-tt"
           << "-o" << "ConnectTimeout=10"
           << "-o" << "StrictHostKeyChecking=no"
           << "-o" << "UserKnownHostsFile=/dev/null"
           << "-o" << "BatchMode=yes"
           << QStringLiteral("%1@%2").arg(target.ssh_user, target.host);
}

/**
 * Robot-side sweep: leave the robot with no Stage 6 stack running. Used
 * before a launch (a step-2 map-collection tree, whose Fast-LIO / Livox /
 * ODrive nodes routinely ignore the shutdown request, must not coexist
 * with the director) and at teardown.
 *
 * Order matters. SIGINT to `ros2 launch` is Ctrl-C: the launch shuts its
 * ~25 children down in dependency order and exits — never SIGKILL it
 * first, that orphans every node. Then the nodes known to ignore that
 * shutdown are killed by name (legacy's list plus the director stack),
 * the data collector is given time to release the Seek SDK, and only the
 * survivors get -9. Exits quickly when nothing is running: every pgrep
 * loop breaks on its first iteration.
 */
const char* kRobotSweep =
    "set +e; "
    "pkill -INT -f '[r]os2 launch pilot_control robot_' >/dev/null 2>&1 || true; "
    "for i in $(seq 1 100); do "
    "  pgrep -f '[r]os2 launch pilot_control robot_' >/dev/null 2>&1 || break; "
    "  sleep 0.1; "
    "done; "
    "pkill -f '[c]overage_director_node' >/dev/null 2>&1 || true; "
    "pkill -f '[m]pc_accel_autonomous_controller' >/dev/null 2>&1 || true; "
    "pkill -f '[m]ap_collection_node' >/dev/null 2>&1 || true; "
    "pkill -f '[f]astlio_mapping' >/dev/null 2>&1 || true; "
    "pkill -f '[l]ivox_ros_driver2_node' >/dev/null 2>&1 || true; "
    "pkill -f '[o]drive_can_node' >/dev/null 2>&1 || true; "
    "pkill -f '[d]iff_drive_controller' >/dev/null 2>&1 || true; "
    "pkill -f '[/]pilot_control/udc_supervisor' >/dev/null 2>&1 || true; "
    "pkill -f '[/]pilot_control/unified_data_collector' >/dev/null 2>&1 || true; "
    "pkill -f '[r]os2 bag record' >/dev/null 2>&1 || true; "
    "pkill -f '[z]enohd -c .*zenohd_robot' >/dev/null 2>&1 || true; "
    "for i in $(seq 1 50); do "
    "  pgrep -f '[/]pilot_control/unified_data_collector' >/dev/null 2>&1 || break; "
    "  sleep 0.1; "
    "done; "
    "pkill -9 -f '[/]pilot_control/unified_data_collector' >/dev/null 2>&1 || true; "
    "pkill -9 -f '[r]os2 launch pilot_control robot_' >/dev/null 2>&1 || true";

/** Upper bound for one robot sweep: 10 s launch wait + 5 s UDC wait +
    SSH round trip. Typical when nothing is running: ~1 s. */
constexpr int kRobotSweepTimeoutMs = 20000;

/** Give the laptop `ros2 launch` time to bring its tree down before
    SIGKILL — the 3 s used before left orphans. */
constexpr int kLaunchTerminateGraceMs = 10000;

void runLaptopSweep() {
    QProcess sweep;
    sweep.start("bash", {"-lc", QString::fromLatin1(kLaptopLaunchSweep)});
    sweep.waitForFinished(6000);
}

}  // namespace

MissionController::MissionController(QObject* parent) : QObject(parent) {
    laptop_proc_ = new QProcess(this);
    robot_proc_ = new QProcess(this);
    laptop_proc_->setProcessChannelMode(QProcess::MergedChannels);
    robot_proc_->setProcessChannelMode(QProcess::MergedChannels);
    hookProcessLogging(laptop_proc_, QStringLiteral("laptop"));
    hookProcessLogging(robot_proc_, QStringLiteral("robot"));
}

void MissionController::hookProcessLogging(QProcess* proc, const QString& tag) {
    const bool is_robot = proc == robot_proc_;
    connect(proc, &QProcess::readyReadStandardOutput,
            this, [this, proc, tag, is_robot] {
                const QStringList lines =
                    QString::fromUtf8(proc->readAllStandardOutput())
                        .split('\n', Qt::SkipEmptyParts);
                for (const QString& line : lines) {
                    const QString trimmed = line.trimmed();
                    emit logLine(QStringLiteral("[%1] %2").arg(tag, trimmed));
                    if (is_robot) {
                        robot_output_tail_.append(trimmed);
                        while (robot_output_tail_.size() > kRobotOutputTailMax) {
                            robot_output_tail_.removeFirst();
                        }
                    }
                }
            });
    connect(proc,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, tag](int code, QProcess::ExitStatus) {
                emit logLine(QStringLiteral("[%1] launch exited (rc=%2)")
                                 .arg(tag)
                                 .arg(code));
                // During teardown an exit is the expected outcome and
                // stays quiet; otherwise the screen turns it into an
                // operator-facing failure.
                if (mission_active_ && !tearing_down_) {
                    emit launchDied(tag, code);
                }
            });
}

void MissionController::openMissionLog() {
    closeMissionLog();
    const QString dir_path =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
        QStringLiteral("/mission_logs");
    QDir dir(dir_path);
    if (!dir.mkpath(QStringLiteral("."))) {
        return;
    }
    // QDir::Time is newest-first, so everything from index kMissionLogsKept-1
    // on is the oldest: dropping them leaves room for the one about to open.
    const QFileInfoList existing = dir.entryInfoList(
        {QStringLiteral("*.log")}, QDir::Files, QDir::Time);
    for (int i = kMissionLogsKept - 1; i < existing.size(); ++i) {
        QFile::remove(existing.at(i).absoluteFilePath());
    }
    const QString path =
        dir.filePath(QStringLiteral("mission_%1.log")
                         .arg(QDateTime::currentDateTime().toString(
                             QStringLiteral("yyyyMMdd_hhmmss"))));
    auto* file = new QFile(path, this);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Append |
                    QIODevice::Text)) {
        delete file;
        return;
    }
    mission_log_ = file;
    mission_log_path_ = path;
}

void MissionController::closeMissionLog() {
    if (!mission_log_) {
        return;
    }
    mission_log_->close();
    delete mission_log_;
    mission_log_ = nullptr;
    // mission_log_path_ deliberately survives: the failure dialogs shown
    // after teardown still need to name the file.
}

void MissionController::appendMissionLog(const QString& line) {
    if (!mission_log_) {
        return;
    }
    mission_log_->write(QStringLiteral("%1 %2\n")
                            .arg(QDateTime::currentDateTimeUtc().toString(
                                     Qt::ISODateWithMs),
                                 line)
                            .toUtf8());
    // Flushed per line: the whole point is to survive a crash or a kill.
    mission_log_->flush();
}

QStringList MissionController::recentRobotOutput(int max_lines) const {
    if (max_lines <= 0 || robot_output_tail_.size() <= max_lines) {
        return robot_output_tail_;
    }
    return robot_output_tail_.mid(robot_output_tail_.size() - max_lines);
}

RobotTarget MissionController::resolveRobotTarget(QString* error) {
    // Same resolution the rest of the OCU uses: QSettings dev override
    // (`robot_ip`) first, then the logged-in robot's registry entry.
    RobotTarget target;
    ResolvedRobotSshTarget ssh_target;
    QString resolve_error;
    if (!resolveRobotSshTargetFromSettings(&ssh_target, &resolve_error)) {
        if (error) {
            *error = resolve_error;
        }
        return target;
    }
    target.host = ssh_target.host;
    target.ssh_user = ssh_target.ssh_user;
    target.robot_id = ssh_target.host;  // display fallback; id not required
    target.valid = !target.host.isEmpty();
    return target;
}

QString MissionController::roiVerticesArgument(const RoiPolygon& poly,
                                               const geo::GeoPose& robot) {
    const geo::GeoPoint anchor{robot.lat, robot.lon};
    QStringList values;
    for (const geo::GeoPoint& corner : poly.vertices) {
        const QPointF enu = geo::enuFromGeo(anchor, corner);
        const QPointF body = geo::bodyFromEnu(enu, robot.heading_deg);
        values << QString::number(body.x(), 'f', 3)
               << QString::number(body.y(), 'f', 3);
    }
    return QStringLiteral("[%1]").arg(values.join(QStringLiteral(",")));
}

QString MissionController::roiEdgeFlagsArgument(const RoiPolygon& poly) {
    QStringList values;
    const int n = poly.vertices.size();
    for (int i = 0; i < n; ++i) {
        const bool marked =
            i < poly.roof_edges.size() ? poly.roof_edges[i] : false;
        values << (marked ? QStringLiteral("1") : QStringLiteral("0"));
    }
    return QStringLiteral("[%1]").arg(values.join(QStringLiteral(",")));
}

bool MissionController::startMission(const RoiPolygon& poly,
                                     const geo::GeoPose& robot,
                                     const ScanParams& scan,
                                     QString* error) {
    if (mission_active_) {
        if (error) *error = QStringLiteral("A mission is already active.");
        return false;
    }
    robot_output_tail_.clear();
    if (!poly.valid() || !robot.valid) {
        if (error) *error = QStringLiteral("ROI and robot placement are both required.");
        return false;
    }
    QString target_error;
    target_ = resolveRobotTarget(&target_error);
    if (!target_.valid) {
        if (error) *error = target_error;
        return false;
    }

    // Before the first logLine, so the launch args themselves are on record.
    openMissionLog();

    const QString roi_arg = roiVerticesArgument(poly, robot);
    emit logLine(QStringLiteral("[send] roi_vertices=%1 (robot_init frame)")
                     .arg(roi_arg));
    emit logLine(QStringLiteral("[send] scan params%1").arg(scan.launchArgs()));

    // Leave nothing stale on either side, then launch. Same launch + args
    // as the legacy path; the robot sweep is what stops a lingering step-2
    // map-collection tree from fighting the director for the LiDAR /
    // FAST-LIO / ODrive nodes.
    runLaptopSweep();
    runRobotSweep();

    const QString laptop_cmd = QString::fromLatin1(kLaunchEnvPreamble) +
        QStringLiteral(
            "ros2 launch pilot_control laptop_teleop.launch.py "
            "robot_ip:=%1 use_xterm:=false interactive_sdl:=false "
            "cmd_vel_enabled:=false")
            .arg(target_.host);
    laptop_proc_->start("bash", QStringList() << "-lc" << laptop_cmd);
    if (!laptop_proc_->waitForStarted(3000)) {
        if (error) {
            *error = QStringLiteral("Failed to start laptop launch: %1")
                         .arg(laptop_proc_->errorString());
        }
        return false;
    }

    // Pass the YAML list as a bare launch arg. Do NOT wrap it in
    // `$BDR_ROI_VERTICES`: the ssh remote is `bash -c` of a double-quoted
    // `bash -lc "..."`, so `$BDR_ROI_VERTICES` expands in the outer
    // shell (unset → empty) before `export` ever runs. An empty
    // `roi_vertices:=` either kills the manager on yaml parse or silently
    // substitutes the default 20×20 box.
    //
    // `[...]` is safe here: no spaces (one argv), the inner -lc string is
    // double-quoted (no glob), and `set -f` covers a dropped quote layer.
    QString remote_script = QString::fromLatin1(kLaunchEnvPreamble) +
        QStringLiteral("set -f; ");
    const bool any_roof_edge =
        std::any_of(poly.roof_edges.begin(), poly.roof_edges.end(),
                    [](bool marked) { return marked; });
    if (any_roof_edge) {
        emit logLine(QStringLiteral(
            "[send] roof-edge flags %1")
                         .arg(roiEdgeFlagsArgument(poly)));
    }
    remote_script += QStringLiteral(
                         "ros2 launch pilot_control "
                         "robot_autonomous_coverage_director.launch.py "
                         "roi_vertices:=%1")
                         .arg(roi_arg);
    if (any_roof_edge) {
        remote_script += QStringLiteral(" roi_edge_flags:=%1")
                             .arg(roiEdgeFlagsArgument(poly));
    }
    remote_script += scan.launchArgs();
    const QString remote_cmd =
        QStringLiteral("bash -lc \"%1\"")
            .arg(remote_script.replace(QLatin1Char('"'), QLatin1String("\\\"")));
    QStringList args = sshBaseArgs(target_);
    args << remote_cmd;
    robot_proc_->start("ssh", args);
    if (!robot_proc_->waitForStarted(3000)) {
        if (error) {
            *error = QStringLiteral("Failed to start robot launch: %1")
                         .arg(robot_proc_->errorString());
        }
        laptop_proc_->kill();
        return false;
    }

    mission_active_ = true;
    emit missionStateChanged(true);
    emit logLine(QStringLiteral("[send] mission launched on %1 (%2)")
                     .arg(target_.robot_id, target_.host));
    return true;
}

void MissionController::remoteServiceCall(const QString& service,
                                          const QString& type,
                                          const QString& request,
                                          RemoteCallback on_done) {
    if (!target_.valid) {
        QString error;
        target_ = resolveRobotTarget(&error);
        if (!target_.valid) {
            if (on_done) {
                on_done(false, error.isEmpty()
                                   ? QStringLiteral("no robot host")
                                   : error);
            }
            return;
        }
    }
    // `timeout` bounds the call: `ros2 service call` waits forever for a
    // service that is not there. Single-quote the YAML for the remote shell.
    QString yaml = request;
    yaml.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    const QString script =
        QString::fromLatin1(kLaunchEnvPreamble) +
        QStringLiteral("timeout 10 ros2 service call %1 %2 '%3'")
            .arg(service, type, yaml);
    const QString remote_cmd =
        QStringLiteral("bash -lc \"%1\"")
            .arg(QString(script).replace(QLatin1Char('"'),
                                         QLatin1String("\\\"")));
    auto* proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    QStringList args = sshBaseArgs(target_);
    args.removeAll(QStringLiteral("-tt"));
    args << remote_cmd;
    connect(proc,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this, proc, service, on_done](int code, QProcess::ExitStatus) {
                const QString out =
                    QString::fromUtf8(proc->readAllStandardOutput()).trimmed();
                proc->deleteLater();
                // ros2 prints "response:\n<type>(success=True, ...)".
                const bool ok = code == 0 &&
                                !out.contains(QLatin1String("success=False"));
                emit logLine(QStringLiteral("[ssh-svc] %1 rc=%2 %3")
                                 .arg(service)
                                 .arg(code)
                                 .arg(out.section(QLatin1Char('\n'), -1)));
                if (on_done) {
                    on_done(ok, out.isEmpty() ? QStringLiteral("rc=%1").arg(code)
                                              : out.section(QLatin1Char('\n'), -1));
                }
            });
    proc->start("ssh", args);
}

void MissionController::remoteDisarm(RemoteCallback on_done) {
    if (!target_.valid) {
        QString error;
        target_ = resolveRobotTarget(&error);
        if (!target_.valid) {
            if (on_done) {
                on_done(false, error);
            }
            return;
        }
    }
    // Join each axis by pid: a bare `wait` reports 0 even when both failed.
    const QString script =
        QString::fromLatin1(kLaunchEnvPreamble) +
        QStringLiteral(
            "timeout 4 ros2 service call /left/request_axis_state "
            "odrive_can/srv/AxisState '{axis_requested_state: 1}' >/dev/null & "
            "left=\\$!; "
            "timeout 4 ros2 service call /right/request_axis_state "
            "odrive_can/srv/AxisState '{axis_requested_state: 1}' >/dev/null & "
            "right=\\$!; "
            "wait \\$left; lrc=\\$?; wait \\$right; rrc=\\$?; "
            "[ \\$lrc -eq 0 ] && [ \\$rrc -eq 0 ]");
    const QString remote_cmd =
        QStringLiteral("bash -lc \"%1\"")
            .arg(QString(script).replace(QLatin1Char('"'),
                                         QLatin1String("\\\"")));
    auto* proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    QStringList args = sshBaseArgs(target_);
    args.removeAll(QStringLiteral("-tt"));
    args << remote_cmd;
    connect(proc,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, proc, on_done](int code, QProcess::ExitStatus) {
                const QString out =
                    QString::fromUtf8(proc->readAllStandardOutput()).trimmed();
                proc->deleteLater();
                emit logLine(QStringLiteral("[ssh-svc] disarm rc=%1").arg(code));
                if (on_done) {
                    on_done(code == 0, out);
                }
            });
    proc->start("ssh", args);
}

void MissionController::runRobotSweep() {
    if (!target_.valid) {
        return;
    }
    QProcess sweep;
    QStringList args = sshBaseArgs(target_);
    args.removeAll(QStringLiteral("-tt"));
    args << QString::fromLatin1(kRobotSweep);
    sweep.start("ssh", args);
    sweep.waitForFinished(kRobotSweepTimeoutMs);
}

void MissionController::teardownMission() {
    if (!mission_active_ && robot_proc_->state() == QProcess::NotRunning &&
        laptop_proc_->state() == QProcess::NotRunning) {
        return;
    }
    emit logLine(QStringLiteral("[teardown] stopping launches…"));
    tearing_down_ = true;

    // Robot first: the sweep Ctrl-Cs the remote launch, which shuts its
    // tree down in order and exits, and with it the `ssh -tt` carrying it
    // (robot_proc_). Killing the local ssh instead would hang up the pty
    // and SIGHUP the launch — an instant death that orphans every node.
    runRobotSweep();
    if (robot_proc_->state() != QProcess::NotRunning &&
        !robot_proc_->waitForFinished(2000)) {
        robot_proc_->kill();
        robot_proc_->waitForFinished(1000);
    }

    // Laptop: SIGTERM reaches `ros2 launch`, which reaps its children in
    // order; only escalate once it has had a real chance.
    if (laptop_proc_->state() != QProcess::NotRunning) {
        laptop_proc_->terminate();
        if (!laptop_proc_->waitForFinished(kLaunchTerminateGraceMs)) {
            laptop_proc_->kill();
            laptop_proc_->waitForFinished(1000);
        }
    }
    runLaptopSweep();

    mission_active_ = false;
    tearing_down_ = false;
    emit missionStateChanged(false);
    emit logLine(QStringLiteral("[teardown] done"));
    // After the last logLine, so it lands in the file.
    closeMissionLog();
}

}  // namespace f2c_cpp
