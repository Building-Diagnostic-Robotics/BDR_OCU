#include "satellite_mission_controller.hpp"

#include "robot_registry.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace f2c_cpp {

namespace {

/** Same env preamble the main OCU uses for both launch sides. */
const char* kEnvPreamble =
    "set -e; "
    "if [ -f \"$HOME/.bashrc\" ]; then source \"$HOME/.bashrc\"; fi; "
    "if [ -f /opt/ros/humble/setup.bash ]; then source /opt/ros/humble/setup.bash; fi; "
    "if [ -f \"$HOME/pilot_ws/install/setup.bash\" ]; then source \"$HOME/pilot_ws/install/setup.bash\"; fi; "
    "case \"${CYCLONEDDS_URI:-}\" in *rf_cyclonedds.xml*) unset CYCLONEDDS_URI ;; esac; "
    "if [ -z \"${CYCLONEDDS_URI:-}\" ] && [ -f \"$HOME/cyclone_loopback.xml\" ]; then "
    "export CYCLONEDDS_URI=\"file://$HOME/cyclone_loopback.xml\"; "
    "fi; "
    "export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp; "
    "export ROS_DOMAIN_ID=0; ";

QStringList sshBaseArgs(const RobotTarget& target) {
    return QStringList()
           << "-tt"
           << "-o" << "ConnectTimeout=10"
           << "-o" << "StrictHostKeyChecking=no"
           << "-o" << "UserKnownHostsFile=/dev/null"
           << "-o" << "BatchMode=yes"
           << QStringLiteral("%1@%2").arg(target.ssh_user, target.host);
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
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc, tag] {
        const QStringList lines =
            QString::fromUtf8(proc->readAllStandardOutput())
                .split('\n', Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            emit logLine(QStringLiteral("[%1] %2").arg(tag, line.trimmed()));
        }
    });
    connect(proc,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, tag](int code, QProcess::ExitStatus) {
                emit logLine(QStringLiteral("[%1] launch exited (rc=%2)")
                                 .arg(tag)
                                 .arg(code));
            });
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

QString MissionController::roiVerticesArgument(const RoiRect& roi,
                                               const geo::GeoPose& robot) {
    const geo::GeoPoint anchor{robot.lat, robot.lon};
    QStringList values;
    const auto corners = roi.corners();
    for (const geo::GeoPoint& corner : corners) {
        const QPointF enu = geo::enuFromGeo(anchor, corner);
        const QPointF body = geo::bodyFromEnu(enu, robot.heading_deg);
        values << QString::number(body.x(), 'f', 3)
               << QString::number(body.y(), 'f', 3);
    }
    return QStringLiteral("[%1]").arg(values.join(QStringLiteral(",")));
}

bool MissionController::startMission(const RoiRect& roi,
                                     const geo::GeoPose& robot,
                                     QString* error) {
    if (mission_active_) {
        if (error) *error = QStringLiteral("A mission is already active.");
        return false;
    }
    if (!roi.valid || !robot.valid) {
        if (error) *error = QStringLiteral("ROI and robot placement are both required.");
        return false;
    }
    QString target_error;
    target_ = resolveRobotTarget(&target_error);
    if (!target_.valid) {
        if (error) *error = target_error;
        return false;
    }

    const QString roi_arg = roiVerticesArgument(roi, robot);
    emit logLine(QStringLiteral("[send] roi_vertices=%1 (robot_init frame)")
                     .arg(roi_arg));

    // Clear any stale local launch, then start the laptop side (zenoh bridge
    // + host_teleop heartbeat). Same launch + args as the main OCU.
    {
        QProcess cleanup;
        cleanup.start("bash",
                      {"-lc",
                       "pkill -f '[r]os2 launch pilot_control laptop_teleop.launch.py' "
                       ">/dev/null 2>&1 || true"});
        cleanup.waitForFinished(4000);
    }
    QString laptop_cmd = QString::fromLatin1(kEnvPreamble) +
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

    // Clear any stale remote launch, then start the autonomy stack with the
    // ROI baked into the launch arguments.
    {
        QProcess cleanup;
        QStringList args = sshBaseArgs(target_);
        args << "pkill -f '[r]os2 launch pilot_control "
                "robot_autonomous_coverage.launch.py' >/dev/null 2>&1 || true";
        cleanup.start("ssh", args);
        cleanup.waitForFinished(8000);
    }
    QString remote_script = QString::fromLatin1(kEnvPreamble) +
        QStringLiteral(
            "ros2 launch pilot_control robot_autonomous_coverage.launch.py "
            "roi_vertices:='%1'")
            .arg(roi_arg);
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

void MissionController::teardownMission() {
    if (!mission_active_ && robot_proc_->state() == QProcess::NotRunning &&
        laptop_proc_->state() == QProcess::NotRunning) {
        return;
    }
    emit logLine(QStringLiteral("[teardown] stopping launches…"));

    if (target_.valid) {
        QProcess killer;
        QStringList args = sshBaseArgs(target_);
        args << "pkill -f '[r]os2 launch pilot_control "
                "robot_autonomous_coverage.launch.py' >/dev/null 2>&1 || true; "
                "sleep 2; "
                "pkill -9 -f '[r]os2 launch pilot_control "
                "robot_autonomous_coverage.launch.py' >/dev/null 2>&1 || true";
        killer.start("ssh", args);
        killer.waitForFinished(12000);
    }
    if (robot_proc_->state() != QProcess::NotRunning) {
        robot_proc_->terminate();
        if (!robot_proc_->waitForFinished(3000)) {
            robot_proc_->kill();
            robot_proc_->waitForFinished(1000);
        }
    }
    if (laptop_proc_->state() != QProcess::NotRunning) {
        laptop_proc_->terminate();
        if (!laptop_proc_->waitForFinished(3000)) {
            laptop_proc_->kill();
            laptop_proc_->waitForFinished(1000);
        }
    }
    {
        QProcess cleanup;
        cleanup.start("bash",
                      {"-lc",
                       "pkill -f '[r]os2 launch pilot_control laptop_teleop.launch.py' "
                       ">/dev/null 2>&1 || true"});
        cleanup.waitForFinished(4000);
    }

    mission_active_ = false;
    emit missionStateChanged(false);
    emit logLine(QStringLiteral("[teardown] done"));
}

}  // namespace f2c_cpp
