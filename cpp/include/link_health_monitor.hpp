#pragma once

#include <QObject>
#include <QString>
#include <QtGlobal>

#include <array>

class QTimer;

namespace f2c_cpp {

// LinkHealthMonitor — single source of truth for "is the robot reachable?"
//
// AppShellWindow stamps `lastSeen()` from existing ROS callbacks (odom,
// udc_health, scan_status, controller_status, stream_status, fpv_frame),
// and a 250 ms internal evaluator emits `linkStateChanged` whenever the
// derived state crosses a threshold.
//
// State derivation (per `evaluate()`):
//   - Healthy:      any source <  2000 ms old
//   - Degraded:     any source <  5000 ms old (else)
//   - Disconnected: all sources >= 5000 ms old (or never seen and >5s
//                   after monitor armed)
//
// The monitor stays Healthy/Degraded/Disconnected until it is `arm()`ed.
// Pre-arm (no scan running) it reports Idle so the UI doesn't false-positive
// during the dashboard / pre-flight flow.  Call `arm()` when the
// exploration / scan launch is up and `disarm()` when it tears down.
class LinkHealthMonitor : public QObject {
    Q_OBJECT

public:
    enum class Source {
        Odom = 0,
        UdcHealth,
        ScanStatus,
        ControllerStatus,
        StreamStatus,
        FpvFrame,
        kCount  // sentinel
    };

    enum class State {
        Idle,          // pre-arm: no scan in progress, monitor inert
        Healthy,       // any tracked source < 2 s old
        Degraded,      // any tracked source 2-5 s old
        Disconnected,  // all tracked sources >= 5 s old
    };

    explicit LinkHealthMonitor(QObject* parent = nullptr);
    ~LinkHealthMonitor() override;

    // Begin tracking link health.  Resets all source timestamps.
    // Idempotent.
    void arm();

    // Stop emitting linkStateChanged and revert to Idle.
    // Idempotent.  Use when the launch tree tears down.
    void disarm();

    // Stamp `now()` for the given source.  Cheap; safe to call from any
    // ROS callback.  When transitioning out of Disconnected, the next
    // `evaluate()` tick fires `linkStateChanged(...)`.
    void stamp(Source source);

    // Force an immediate evaluation (otherwise driven by 250 ms timer).
    void evaluateNow();

    State state() const { return state_; }
    bool isArmed() const { return armed_; }

    // Milliseconds since the most recent stamp from any tracked source.
    // Returns -1 if no source has ever been stamped since arm().
    qint64 msSinceLastSeen() const;

    // Milliseconds since the monitor entered the current `state_`.
    // Useful for "Robot offline 12s" UI strings.
    qint64 msInCurrentState() const;

    // Per-source freshness.  -1 if never stamped since arm().
    qint64 msSinceLastSeen(Source source) const;

    static const char* sourceName(Source source);

signals:
    // Fires when state transitions across one of the thresholds.
    // `since_ms` is the "ms since most-recent-stamp" at the moment of
    // transition (informational; may be 0 on Healthy entry).
    void linkStateChanged(State old_state, State new_state, qint64 since_ms);

private:
    void evaluate();
    State derive(qint64 now_ms) const;

    QTimer* timer_ = nullptr;
    bool armed_ = false;
    State state_ = State::Idle;
    qint64 state_entered_at_ms_ = 0;
    std::array<qint64, static_cast<size_t>(Source::kCount)> last_seen_ms_{};

    // Thresholds (milliseconds).  Tunable here so field-test feedback
    // can adjust without touching call sites.
    static constexpr qint64 kHealthyMaxMs = 2000;
    static constexpr qint64 kDegradedMaxMs = 5000;
    static constexpr int kEvaluateIntervalMs = 250;
};

}  // namespace f2c_cpp
