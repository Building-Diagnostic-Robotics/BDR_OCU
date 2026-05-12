#pragma once

#include <QObject>
#include <QString>
#include <QtGlobal>

class QProcess;
class QTcpSocket;
class QTimer;

namespace f2c_cpp {

// RobotReachabilityProbe
//
// Lightweight network-layer liveness check for the robot, used by the
// layered connectivity model alongside `LinkHealthMonitor`.
//
// Why this exists:
//   `LinkHealthMonitor` infers reachability from ROS topic freshness
//   (odom, scan_status, controller_status, fpv_frame, etc).  When
//   Zenoh's peer rediscovery flaps after a brief radio fade — which can
//   last 10-30 s on perfectly-healthy Microhard sessions — every ROS
//   topic goes stale in lock-step, so the OCU's UI snaps from HEALTHY
//   to OFFLINE even though the robot is still happily on the radio.
//   That false positive flapping is what the operator is hitting in the
//   field.
//
//   This probe answers a strictly narrower question: "Is the robot's
//   IP reachable on the network at all?"  When the answer is yes but
//   ROS topics are stale, `LinkHealthMonitor` resolves to RECONNECTING
//   (amber, button lockout, no banner) instead of DISCONNECTED (red,
//   banner + halo + offline-finalize dialog).
//
// Probe strategy (per 1 s tick):
//   1. ICMP via `ping -c 1 -W 1 <host>`.  Reachable if exit code 0.
//   2. If ICMP fails (e.g. firewall blocks ICMP, ping not available
//      with cap_net_raw), fall back to TCP-22 connect via QTcpSocket.
//      Reachable if connect succeeds within 1 s.
//
// Debounce:
//   - 2 consecutive failed ticks (~2 s) flip the state to Unreachable.
//   - 1 successful tick flips the state back to Reachable (we want
//     fast recovery; a successful ICMP/TCP is high-confidence).
//
// Lifecycle:
//   - `arm(host)` from `AppShellWindow` when exploration launch starts
//     (same call site as `LinkHealthMonitor::arm()`).
//   - `disarm()` at teardown.  No probe traffic outside Stages 4/5.
//   - Idempotent.  `arm()` while already armed re-applies the host.
class RobotReachabilityProbe : public QObject {
    Q_OBJECT

public:
    enum class State {
        Idle,         // pre-arm: probe inert
        Reachable,    // most recent ICMP or TCP-22 succeeded
        Unreachable,  // 2+ consecutive failed ticks
    };

    explicit RobotReachabilityProbe(QObject* parent = nullptr);
    ~RobotReachabilityProbe() override;

    // Begin probing `host` on a 1 s cadence.  Idempotent — re-arming
    // with the same host is a no-op; with a different host triggers
    // an immediate fresh probe.  Empty `host` is a no-op (doesn't
    // arm).
    void arm(const QString& host);

    // Stop probing.  Cancels any in-flight probe and reverts to Idle.
    // Does NOT emit a state-change signal on disarm to avoid
    // confusing downstream consumers — the LinkHealthMonitor disarm
    // is the source of truth for "stop reporting".  Idempotent.
    void disarm();

    State state() const { return state_; }
    bool isArmed() const { return armed_; }
    QString host() const { return host_; }

    // True if the probe is armed and the latest evaluation succeeded.
    // Consumed by `LinkHealthMonitor::derive()` via `setReachability`.
    bool isReachable() const { return state_ == State::Reachable; }

    // Tunable so field-test feedback can adjust without touching
    // call sites.
    static constexpr int kProbeIntervalMs = 1000;
    static constexpr int kPingTimeoutMs = 1200;
    static constexpr int kTcpProbeTimeoutMs = 1200;
    static constexpr int kFailuresToUnreachable = 2;

signals:
    // Fires when the debounced state crosses Reachable <-> Unreachable.
    // Initial transition out of Idle (first arm) does NOT fire — it
    // waits for the first probe to complete so consumers don't see a
    // false "Unreachable" before the first round trip.
    void reachabilityChanged(State old_state, State new_state);

private slots:
    void onTick();
    void onPingFinished(int exit_code);
    void onTcpConnected();
    void onTcpError();

private:
    void startProbe();
    void cancelInFlight();
    void recordSuccess();
    void recordFailure();
    void setState(State next);

    bool armed_ = false;
    QString host_;
    State state_ = State::Idle;
    int consecutive_failures_ = 0;
    bool probe_in_flight_ = false;
    bool ping_phase_ = true;  // current probe is in ping phase (vs TCP fallback)

    QTimer* tick_timer_ = nullptr;
    QProcess* ping_proc_ = nullptr;
    QTcpSocket* tcp_socket_ = nullptr;
    QTimer* tcp_timeout_timer_ = nullptr;
};

}  // namespace f2c_cpp
