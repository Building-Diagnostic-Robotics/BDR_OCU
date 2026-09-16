#include "satellite_mission_controller.hpp"

#include "async_process.hpp"
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
           << "-o" << "LogLevel=ERROR"
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
 *
 * `set +e` and `|| true` mean this script exits 0 whenever ssh lands;
 * a non-zero from runRobotSweep is reachability (255) or the hang
 * deadline (-1), not "a process survived".
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

/** Pre-launch only. A wedged CLI daemon can swallow the next
    `ros2 launch`. Must not run at teardown — conclude / disarm still
    need `ros2 service call`. `pkill` of `_ros2_daemon`, not
    `ros2 daemon stop`: this sweep does not source kLaunchEnvPreamble,
    so `ros2` is not on PATH (rc=127). Echo only when something died
    so a no-op cannot hide in the mission log. */
const char* kPreLaunchDaemonKill =
    "; if pkill -f '[_]ros2_daemon' >/dev/null 2>&1; then "
    "echo swept:ros2_daemon; fi";

/** Printed after the login shell finishes sourcing, before `ros2 launch`.
    The mute alarm keys off this so a MOTD / profile echo cannot count
    as the robot having started. */
constexpr const char* kLaunchBeginMarker = "BDR_LAUNCH_BEGIN";

/** Upper bound for one robot sweep: 10 s launch wait + 5 s UDC wait +
    SSH round trip. Typical when nothing is running: ~1 s. */
constexpr int kRobotSweepTimeoutMs = 20000;

/** Give the laptop `ros2 launch` time to bring its tree down before
    SIGKILL — the 3 s used before left orphans. */
constexpr int kLaunchTerminateGraceMs = 10000;

/** The remote launch exits with its own sweep; only orphans need killing. */
constexpr int kReapRobotGraceMs = 2000;

constexpr int kLaptopSweepTimeoutMs = 6000;

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
                        if (!robot_marker_seen_) {
                            if (trimmed.contains(
                                    QLatin1String(kLaunchBeginMarker))) {
                                robot_marker_seen_ = true;
                            }
                        } else {
                            robot_launch_spoke_ = true;
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
    // A spawn failure emits errorOccurred and never `finished`, so without
    // this a missing `bash` / `ssh` would leave the operator on the scan
    // page with a stack that was never launched and no failure at all.
    connect(proc, &QProcess::errorOccurred, this,
            [this, tag](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart) {
                    return;
                }
                emit logLine(
                    QStringLiteral("[%1] launch failed to start").arg(tag));
                if (mission_active_ && !tearing_down_) {
                    emit launchDied(tag, -1);
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
    robot_marker_seen_ = false;
    robot_launch_spoke_ = false;
    robot_spawn_ms_ = 0;
    parked_launch_seq_ = -1;
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

    if (tearing_down_) {
        if (error) {
            *error = QStringLiteral("The previous mission is still stopping.");
        }
        return false;
    }

    // Before the first logLine, so the launch args themselves are on record.
    openMissionLog();

    const QString roi_arg = roiVerticesArgument(poly, robot);
    emit logLine(QStringLiteral("[send] roi_vertices=%1 (robot_init frame)")
                     .arg(roi_arg));
    emit logLine(QStringLiteral("[send] scan params%1").arg(scan.launchArgs()));

    pending_laptop_cmd_ = QString::fromLatin1(kLaunchEnvPreamble) +
        QStringLiteral(
            "ros2 launch pilot_control laptop_teleop.launch.py "
            "robot_ip:=%1 use_xterm:=false interactive_sdl:=false "
            "cmd_vel_enabled:=false")
            .arg(target_.host);

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
        QStringLiteral("set -f; echo %1; ").arg(QLatin1String(kLaunchBeginMarker));
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
    pending_robot_args_ = sshBaseArgs(target_);
    pending_robot_args_ << QStringLiteral("bash -lc \"%1\"")
                               .arg(remote_script.replace(
                                   QLatin1Char('"'), QLatin1String("\\\"")));

    // Active from here, before anything is spawned: the operator lands on
    // the scan page immediately and Cancel has to work during the sweeps.
    // The pill narrates the phases from launchPhase.
    mission_active_ = true;
    force_stop_ = false;
    const int seq = ++mission_seq_;
    emit missionStateChanged(true);

    // Leave nothing stale on either side, then launch. Same launch + args
    // as the legacy path; the robot sweep is what stops a lingering step-2
    // map-collection tree from fighting the director for the LiDAR /
    // FAST-LIO / ODrive nodes.
    emit launchPhase(QStringLiteral("Clearing old processes…"));
    runLaptopSweep([this, seq] {
        if (mission_seq_ != seq) {
            return;
        }
        runRobotSweep([this, seq](int rc, int) {
            if (mission_seq_ != seq) {
                return;
            }
            if (rc != 0) {
                parked_launch_seq_ = seq;
                emit launchPhase(QStringLiteral("Cleanup did not finish"));
                emit robotSweepFailed(rc);
                return;
            }
            spawnLaunches();
        }, /*kill_daemon=*/true);
    });
    return true;
}

void MissionController::spawnLaunches() {
    robot_marker_seen_ = false;
    robot_launch_spoke_ = false;
    robot_spawn_ms_ = QDateTime::currentMSecsSinceEpoch();
    emit launchPhase(QStringLiteral("Starting the robot software…"));
    laptop_proc_->start("bash", QStringList() << "-lc" << pending_laptop_cmd_);
    robot_proc_->start("ssh", pending_robot_args_);
    pending_laptop_cmd_.clear();
    pending_robot_args_.clear();
    emit launchPhase(QString());
    emit logLine(QStringLiteral("[send] mission launched on %1 (%2)")
                     .arg(target_.robot_id, target_.host));
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

void MissionController::runRobotSweep(
    std::function<void(int rc, int elapsed_ms)> on_done, bool kill_daemon) {
    if (!target_.valid) {
        emit logLine(QStringLiteral("[sweep] robot skipped (no target)"));
        if (on_done) {
            on_done(0, 0);
        }
        return;
    }
    QStringList args = sshBaseArgs(target_);
    args.removeAll(QStringLiteral("-tt"));
    QString remote = QString::fromLatin1(kRobotSweep);
    if (kill_daemon) {
        remote += QString::fromLatin1(kPreLaunchDaemonKill);
    }
    args << remote;
    const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
    active_sweep_ = async_proc::run(
        this, QStringLiteral("ssh"), args, kRobotSweepTimeoutMs,
        [this, on_done, t0](int rc, const QString& out) {
            const int elapsed_ms =
                int(QDateTime::currentMSecsSinceEpoch() - t0);
            emit logLine(QStringLiteral("[sweep] robot rc=%1 elapsed=%2ms")
                             .arg(rc)
                             .arg(elapsed_ms));
            if (!out.isEmpty()) {
                const QStringList lines =
                    out.split('\n', Qt::SkipEmptyParts);
                for (const QString& line : lines) {
                    emit logLine(QStringLiteral("[sweep] %1")
                                     .arg(line.trimmed()));
                }
            }
            if (on_done) {
                on_done(rc, elapsed_ms);
            }
        });
}

void MissionController::resumeAfterSweep(bool proceed) {
    if (!proceed || !mission_active_ || tearing_down_ ||
        mission_seq_ != parked_launch_seq_) {
        return;
    }
    if (pending_robot_args_.isEmpty()) {
        emit logLine(QStringLiteral("[send] sweep resume with nothing to launch"));
        parked_launch_seq_ = -1;
        return;
    }
    parked_launch_seq_ = -1;
    spawnLaunches();
}

bool MissionController::robotLaunchRunning() const {
    return robot_proc_ && robot_proc_->state() == QProcess::Running;
}

void MissionController::runLaptopSweep(std::function<void()> on_done) {
    active_sweep_ = async_proc::run(
        this, QStringLiteral("bash"),
        QStringList() << QStringLiteral("-lc")
                      << QString::fromLatin1(kLaptopLaunchSweep),
        kLaptopSweepTimeoutMs, [on_done](int, const QString&) {
            if (on_done) {
                on_done();
            }
        });
}

void MissionController::teardownMission() {
    if (tearing_down_) {
        return;
    }
    if (!mission_active_ && robot_proc_->state() == QProcess::NotRunning &&
        laptop_proc_->state() == QProcess::NotRunning) {
        return;
    }
    emit logLine(QStringLiteral("[teardown] stopping launches…"));
    tearing_down_ = true;
    force_stop_ = false;
    // Abandons an in-flight launch chain: a Cancel during the sweeps must
    // not be followed by spawnLaunches().
    ++mission_seq_;
    emit launchPhase(QString());
    teardownStep(RobotSweep);
}

void MissionController::forceStopTeardown() {
    if (!tearing_down_ || force_stop_) {
        return;
    }
    force_stop_ = true;
    emit logLine(
        QStringLiteral("[teardown] force stop — killing the local launches"));
    // Kill whatever the chain is currently waiting on so its callback lands
    // now rather than at the end of its grace, then let that callback notice
    // force_stop_ and jump ahead.
    if (active_sweep_) {
        active_sweep_->kill();
    }
    if (robot_proc_->state() != QProcess::NotRunning) {
        robot_proc_->kill();
    }
    if (laptop_proc_->state() != QProcess::NotRunning) {
        laptop_proc_->kill();
    }
}

void MissionController::teardownStep(TeardownStep step) {
    if (force_stop_ && step < LaptopSweep) {
        // The local launches are already dead (forceStopTeardown); skip the
        // remaining remote waits and go clean up the laptop.
        step = LaptopSweep;
    }
    switch (step) {
        case RobotSweep:
            // Robot first: the sweep Ctrl-Cs the remote launch, which shuts
            // its tree down in order and exits, and with it the `ssh -tt`
            // carrying it (robot_proc_). Killing the local ssh instead would
            // hang up the pty and SIGHUP the launch — an instant death that
            // orphans every node.
            emit teardownPhase(QStringLiteral("Stopping the robot software…"));
            runRobotSweep([this](int, int) { teardownStep(ReapRobot); });
            return;
        case ReapRobot:
            emit teardownPhase(QStringLiteral("Closing the robot session…"));
            async_proc::reap(robot_proc_, kReapRobotGraceMs,
                             [this] { teardownStep(StopLaptop); });
            return;
        case StopLaptop:
            // SIGTERM reaches `ros2 launch`, which reaps its children in
            // order; only escalate once it has had a real chance.
            emit teardownPhase(QStringLiteral("Stopping the laptop link…"));
            if (laptop_proc_->state() != QProcess::NotRunning) {
                laptop_proc_->terminate();
            }
            async_proc::reap(laptop_proc_, kLaunchTerminateGraceMs,
                             [this] { teardownStep(LaptopSweep); });
            return;
        case LaptopSweep:
            emit teardownPhase(QStringLiteral("Cleaning up…"));
            runLaptopSweep([this] { teardownStep(Done); });
            return;
        case Done:
            break;
    }
    mission_active_ = false;
    tearing_down_ = false;
    force_stop_ = false;
    emit missionStateChanged(false);
    emit logLine(QStringLiteral("[teardown] done"));
    emit teardownPhase(QString());
    emit teardownFinished();
    // After the last logLine, so everything lands in the file.
    closeMissionLog();
}

}  // namespace f2c_cpp
