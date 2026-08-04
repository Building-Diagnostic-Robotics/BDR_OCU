/**
 * @file mission_controller.hpp
 * @brief Launch orchestration for an autonomous coverage mission.
 *
 * Mirrors the main OCU's process model:
 *  - laptop side: `ros2 launch pilot_control laptop_teleop.launch.py`
 *    (zenoh bridge + host_teleop, which supplies the 10 Hz safety heartbeat)
 *  - robot side: ssh -tt ... `ros2 launch pilot_control
 *    robot_autonomous_coverage.launch.py roi_vertices:='[...]'`
 *
 * Robot host/user come from the same robots.json the main OCU reads.
 */

#pragma once

#include "satellite_job_model.hpp"

#include <QObject>
#include <QProcess>
#include <QString>
#include <QVector>

namespace f2c_cpp {

struct RobotTarget {
    QString robot_id;
    QString host;
    QString ssh_user = QStringLiteral("roofus");
    bool valid = false;
};

class MissionController : public QObject {
    Q_OBJECT

public:
    explicit MissionController(QObject* parent = nullptr);

    /** First robot entry from robots.json (same search paths as the OCU). */
    static RobotTarget resolveRobotTarget(QString* error = nullptr);

    /**
     * ROI corners -> robot_init body-frame flat list "[x1,y1,x2,y2,...]".
     * The robot marker pose is the anchor: body x = marker heading.
     */
    static QString roiVerticesArgument(const RoiRect& roi,
                                       const geo::GeoPose& robot);

    bool missionActive() const { return mission_active_; }
    RobotTarget target() const { return target_; }

    /** Starts laptop launch + SSH robot launch. Emits log/state signals. */
    bool startMission(const RoiRect& roi, const geo::GeoPose& robot,
                      QString* error = nullptr);

    /** Kills both launch trees (remote pkill + local process kill). */
    void teardownMission();

signals:
    void logLine(const QString& line);
    void missionStateChanged(bool active);

private:
    void hookProcessLogging(QProcess* proc, const QString& tag);

    RobotTarget target_;
    QProcess* laptop_proc_ = nullptr;
    QProcess* robot_proc_ = nullptr;
    bool mission_active_ = false;
};

}  // namespace f2c_cpp
