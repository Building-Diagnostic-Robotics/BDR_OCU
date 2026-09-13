/**
 * @file ros_link.hpp
 * @brief ROS2 node bridging the satellite OCU to the autonomy stack.
 *
 * Publishes:  /cmd_vel (teleop), /mpc_autonomy_enable (RELIABLE, depth 1;
 *             the screen latches this at 2 Hz — autonomy's manager only
 *             starts planning on a false→true edge and the topic is volatile)
 * Subscribes: /coverage/global_occupancy, /coverage/planned_path,
 *             /coverage/planned_swaths, /coverage/status,
 *             /scan_segment_status, /Odometry_tilt_corrected_diff
 * Services:   /left/request_axis_state, /right/request_axis_state,
 *             /data_collection_coordinator/set_parameters,
 *             /dc/finalize_mission (offline fallback only),
 *             /coverage/conclude, /coverage/abort, /coverage/skip_copy
 *
 * The node spins on a background thread. Snapshots of the latest telemetry
 * are stored under a mutex; parameterless Qt signals notify the GUI thread,
 * which pulls via the getters (avoids queued-metatype plumbing and never
 * touches widgets from ROS callbacks — same discipline as the main OCU).
 */

#pragma once

#include <QColor>
#include <QImage>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace rclcpp {
class Node;
}

namespace f2c_cpp {

struct GridSnapshot {
    QImage image;            // colored occupancy, one pixel per cell
    double resolution = 0.1; // m per cell
    QPointF origin_body;     // body-frame position of image pixel (0,0) corner
    quint64 revision = 0;
};

struct PolylineSet {
    QVector<QVector<QPointF>> lines;  // body-frame points
    QVector<QColor> colors;
    quint64 revision = 0;
};

struct OdomSnapshot {
    double x = 0.0;
    double y = 0.0;
    double yaw = 0.0;  // radians, body frame
    qint64 wall_ms = 0;
    bool valid = false;
};

/** Latest ODrive axis state per side, with arrival stamps for staleness. */
struct MotorStatus {
    int left_axis_state = 0;
    int right_axis_state = 0;
    qint64 left_wall_ms = 0;
    qint64 right_wall_ms = 0;
};

/**
 * Decoded /coverage/status from coverage_director_node.py (1 Hz JSON).
 * Field names mirror the director's payload; see `_publish_coverage_status`
 * there for the authoritative list. `state` is the priority-ordered operator
 * state (ERROR > COMPLETE > WAITING_REVISIT > GPR_ROLLOVER > MANUAL_TAKEOVER
 * > DC_PAUSED > DC_STARTING > STALE_INPUT > STOPPED_* / REPLAN_FAILED / IDLE
 * / SWEEP_DONE > WAITING_READY > POSE_HOLD > PIVOT / TRANSIT / SWEEP_ALIGN /
 * SWEEP).
 */
struct CoverageStatus {
    QString state;
    QString phase;       // executor phase (TRANSIT / SWEEP / SWEEP_ALIGN / …)
    QString mode;        // executor mode string (ALIGN / TRACK / …)
    QString stop;        // stop_reason, empty when moving
    QString stale;       // staleness_stop_reason, empty when inputs fresh
    QString dc;          // IDLE / STARTING / RUNNING / PAUSED / ROLLOVER / ERROR
    QString copy;        // idle / pending / copying / done / failed / skipped
    QString copy_error;
    QString error;       // director fault string; state == ERROR when set
    QStringList not_ready;  // readiness inputs still false (WAITING_READY)
    bool autonomy = false;
    bool initialized = false;
    bool complete = false;
    bool copy_waived = false;
    bool waiting_revisit = false;
    bool takeover = false;
    bool dc_paused = false;
    int revisit = 0;
    // Ledger totals (same unit as each other; only their ratio is used).
    double remaining = -1.0;
    double swept = -1.0;
    double deferred = -1.0;
    qint64 wall_ms = 0;
    bool valid = false;

    /** Fraction swept of everything the ledger ever held, or -1 if unknown. */
    double coverageFraction() const;
    /** Status arrived within `max_age_ms` — the director is alive. */
    bool fresh(qint64 max_age_ms) const;
};

class RosLink : public QObject {
    Q_OBJECT

public:
    explicit RosLink(QObject* parent = nullptr);
    ~RosLink() override;

    bool start(QString* error = nullptr);
    void stop();
    bool isRunning() const { return running_; }

    // Commands (safe from GUI thread).
    void publishTwist(double linear, double angular);
    void publishAutonomyEnable(bool enabled);
    /** ODrive axis state: 1 = IDLE (disarm), 8 = CLOSED_LOOP_CONTROL (arm). */
    void requestAxisState(int state);

    /**
     * Calls /dc/finalize_mission — stops continuous GNSS and stamps
     * mission_finalized_at into mission_config.json. Non-blocking; the
     * callback is marshalled to the GUI thread and fires false (rather than
     * hanging) when the service never appears.
     */
    void finalizeMission(std::function<void(bool ok, QString detail)> on_done);

    using TriggerCallback = std::function<void(bool ok, QString detail)>;
    /**
     * /coverage/conclude — operator Finish. The director ends and saves the
     * section (/dc/end_and_save), finalizes the mission and starts the
     * thumb-drive copy itself; `complete` in /coverage/status goes true only
     * once all of that is accounted for. Refused while coverage work is
     * still active, so callers drop autonomy first.
     */
    void concludeCoverage(TriggerCallback on_done);
    /** /coverage/abort — end mid-run. `save` keeps the swept data as a
        partial section; false discards it. */
    void abortCoverage(bool save, TriggerCallback on_done);
    /** /coverage/skip_copy — waive a pending / failed thumb-drive copy. */
    void skipCopy(TriggerCallback on_done);
    /** True when /coverage/conclude has a server (director alive). */
    bool directorServicesReady() const;

    /**
     * Push building/operator/units to /data_collection_coordinator as
     * string parameters — the autonomy arming gate (same contract as the
     * classic flow's sendDataCollectorSessionMetadata: the caller must
     * hard-block autonomy_enable until on_complete(true)).
     *
     * Non-blocking: if the coordinator's parameter service is not ready
     * the callback fires false immediately (the caller retries on a
     * timer while the robot stack boots). Callback is marshalled to the
     * GUI thread.
     */
    void pushSessionMetadata(const QString& building_name,
                             const QString& operator_name,
                             const QString& units_preference,
                             std::function<void(bool ok)> on_complete);

    // Telemetry snapshots (safe from GUI thread).
    GridSnapshot gridSnapshot() const;
    PolylineSet pathSnapshot() const;
    PolylineSet swathsSnapshot() const;
    OdomSnapshot odomSnapshot() const;
    CoverageStatus coverageStatus() const;
    QString lastSegmentStatus() const;
    MotorStatus motorStatus() const;
    /** True when both axes report IDLE on fresh controller_status. */
    bool motorsIdle() const;
    /** True when both axes report CLOSED_LOOP_CONTROL on fresh status. */
    bool motorsArmed() const;

    static constexpr int kAxisIdle = 1;
    static constexpr int kAxisClosedLoop = 8;
    static constexpr qint64 kControllerStatusStaleMs = 1500;

signals:
    void gridUpdated();
    void pathUpdated();
    void swathsUpdated();
    void odomUpdated();
    void statusUpdated();
    void segmentStatusUpdated();
    void motorStatusUpdated();
    void axisResult(bool ok, const QString& detail);

private:
    void spinLoop();

    class Impl;
    std::unique_ptr<Impl> impl_;
    std::thread spin_thread_;
    std::atomic<bool> running_{false};

    mutable std::mutex mutex_;
    GridSnapshot grid_;
    PolylineSet path_;
    PolylineSet swaths_;
    OdomSnapshot odom_;
    CoverageStatus status_;
    QString segment_status_;
    MotorStatus motors_;
};

}  // namespace f2c_cpp
