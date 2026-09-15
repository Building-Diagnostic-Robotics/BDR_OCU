/**
 * @file mission_controller.hpp
 * @brief Launch orchestration for an autonomous coverage mission.
 *
 * Mirrors the main OCU's process model:
 *  - laptop side: `ros2 launch pilot_control laptop_teleop.launch.py`
 *    (zenoh bridge + host_teleop, which supplies the 10 Hz safety heartbeat)
 *  - robot side: ssh -tt ... `set -f; ros2 launch
 *    pilot_control robot_autonomous_coverage_director.launch.py
 *    roi_vertices:=[x1,y1,...]`  (no `$` env expansion — the outer ssh
 *    `bash -c` would eat `$BDR_ROI_VERTICES` before the inner script ran)
 *
 * Robot host/user come from the same robots.json the main OCU reads.
 */

#pragma once

#include "satellite_job_model.hpp"

#include <QFile>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

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
     *
     * Polygon-only on purpose. A RoiRect overload here would silently drop
     * every vertex past the fourth for any authored shape, so callers must
     * convert with RoiPolygon::fromRect() at the boundary and see that they
     * are doing it.
     */
    static QString roiVerticesArgument(const RoiPolygon& poly,
                                       const geo::GeoPose& robot);

    /** Per-edge roof flags "[0,1,0,0]" (edge i = corner i -> i+1). */
    static QString roiEdgeFlagsArgument(const RoiPolygon& poly);

    bool missionActive() const { return mission_active_; }
    RobotTarget target() const { return target_; }

    /** Starts laptop launch + SSH robot launch. Emits log/state signals.
        `scan` rides along as director launch args (ScanParams::launchArgs);
        the robot must be on `cliff-on-autonomy`, which declares them. */
    bool startMission(const RoiPolygon& poly, const geo::GeoPose& robot,
                      const ScanParams& scan, QString* error = nullptr);

    /** Stops both launch trees: Ctrl-C to the robot launch over SSH (so
        it shuts its tree down in order) + name sweep of what ignores it,
        then SIGTERM to the laptop launch. Synchronous; emits
        missionStateChanged(false) before returning. */
    void teardownMission();

    /** Last lines of the robot launch's merged stdout/stderr — the
        director's traceback lives here when the stack dies at startup.
        Developer-facing: the cliff / tfmini nodes log at 10-20 Hz, so this
        is a rolling window, not a record. Use the mission log for that. */
    QStringList recentRobotOutput(int max_lines = 12) const;

    /**
     * Absolute path of the current (or most recent) mission log, empty
     * before the first launch.
     *
     * The in-GUI log view is capped at 600 blocks and `applyStepVisibility`
     * hides it entirely on the field trim, so without this file a failed
     * launch leaves no evidence at all once the operator tears down.
     */
    QString missionLogPath() const { return mission_log_path_; }

    /**
     * Mirrors one already-formatted log line into the mission log.
     *
     * The screen routes every line it displays through here — including the
     * ones this class emits via `logLine` — so the file is the complete
     * record and each line is written exactly once. Do not also call this
     * from `hookProcessLogging`, or process output lands twice.
     */
    void appendMissionLog(const QString& line);

    using RemoteCallback = std::function<void(bool ok, QString detail)>;
    /**
     * `ros2 service call` on the robot over SSH — the legacy autonomy
     * screen's path for director services. Bypasses the Zenoh bridge
     * entirely, so it still lands when zenoh queries are timing out on a
     * congested radio. `request` is the YAML body ("{}" for Trigger).
     * Non-blocking; callback on the GUI thread with the reply text.
     */
    void remoteServiceCall(const QString& service, const QString& type,
                           const QString& request, RemoteCallback on_done);
    /** Both ODrive axes -> IDLE via SSH `ros2 service call`. */
    void remoteDisarm(RemoteCallback on_done);

signals:
    void logLine(const QString& line);
    void missionStateChanged(bool active);
    /**
     * A launch exited while the mission was active and nobody asked it to
     * (not during teardownMission). `side` is "robot" (the SSH session
     * carrying the director stack ended — rc=0 included, a session that
     * ends is a stack that is gone) or "laptop" (zenoh client + heartbeat
     * gone; the MPC's 1 s heartbeat timeout halts the robot and nothing
     * the OCU publishes gets across). Both are fatal to the run.
     */
    void launchDied(const QString& side, int exit_code);

private:
    void hookProcessLogging(QProcess* proc, const QString& tag);
    /** One SSH round trip that leaves the robot with no Stage 6 stack
        running (see kRobotSweep). Blocking; ~1 s when already clean. */
    void runRobotSweep();
    /** Opens a fresh mission log and prunes older ones. */
    void openMissionLog();
    void closeMissionLog();

    RobotTarget target_;
    QProcess* laptop_proc_ = nullptr;
    QProcess* robot_proc_ = nullptr;
    bool mission_active_ = false;
    bool tearing_down_ = false;
    QStringList robot_output_tail_;
    QFile* mission_log_ = nullptr;
    QString mission_log_path_;
    // 40 was too short to be useful: cliff_horizon logs at 10 Hz and
    // tfmini_cliff_evidence at 20 Hz, so a startup traceback was overwritten
    // within a second and the launch-wait prompt only ever showed INFO spam.
    static constexpr int kRobotOutputTailMax = 200;
    /** Mission logs kept on disk; oldest are pruned at the next launch. */
    static constexpr int kMissionLogsKept = 10;
};

}  // namespace f2c_cpp
