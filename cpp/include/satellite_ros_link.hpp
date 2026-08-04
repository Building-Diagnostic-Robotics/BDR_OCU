/**
 * @file ros_link.hpp
 * @brief ROS2 node bridging the satellite OCU to the autonomy stack.
 *
 * Publishes:  /cmd_vel (teleop), /mpc_autonomy_enable
 * Subscribes: /coverage/global_occupancy, /coverage/planned_path,
 *             /coverage/planned_swaths, /coverage/status,
 *             /scan_segment_status, /Odometry_tilt_corrected_diff
 * Services:   /left/request_axis_state, /right/request_axis_state
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
#include <QVector>

#include <atomic>
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

struct CoverageStatus {
    QString state;    // executor state string ("running", "blocked", ...)
    QString mode;     // horizon mode ("TRACKING", "PIVOT", ...)
    QString reason;   // blocked/idle reason
    double coverage = -1.0;
    qint64 wall_ms = 0;
    bool valid = false;
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

    // Telemetry snapshots (safe from GUI thread).
    GridSnapshot gridSnapshot() const;
    PolylineSet pathSnapshot() const;
    PolylineSet swathsSnapshot() const;
    OdomSnapshot odomSnapshot() const;
    CoverageStatus coverageStatus() const;
    QString lastSegmentStatus() const;

    static constexpr int kAxisIdle = 1;
    static constexpr int kAxisClosedLoop = 8;

signals:
    void gridUpdated();
    void pathUpdated();
    void swathsUpdated();
    void odomUpdated();
    void statusUpdated();
    void segmentStatusUpdated();
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
};

}  // namespace f2c_cpp
