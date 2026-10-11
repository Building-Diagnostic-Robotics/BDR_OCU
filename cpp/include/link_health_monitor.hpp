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
// Layered model (see also `RobotReachabilityProbe`):
//
//   LinkHealthMonitor combines two independent signals:
//     1. Application-layer freshness — the timestamps stamped by ROS
//        callbacks.  Tells us "is data flowing?".
//     2. Network-layer reachability — pushed in via `setReachability()`
//        from `RobotReachabilityProbe` (ICMP / TCP-22 every 1 s).
//        Tells us "is the host on the network at all?".
//
//   Combining them lets us distinguish:
//     - Connecting     → armed, but nothing has been stamped since
//                        this arm(), and the session is younger than
//                        kConnectingTimeoutMs. The launch is still
//                        coming up. Not Offline yet.
//     - Healthy        → topics are fresh (always treated as
//                        connected; reachability irrelevant).
//     - Reconnecting   → topics stale, but probe says host is up.
//                        Likely a Zenoh peer-rediscovery window after
//                        a brief radio fade.  Block commands but
//                        keep visual treatment soft (no banner / halo,
//                        amber pill, grey-out).
//     - Disconnected   → topics stale AND probe failed, after this
//                        session has already had contact — or no stamp
//                        at all for kConnectingTimeoutMs, even when the
//                        probe says the host is up. True offline:
//                        red pill, banner, halo, OfflineFinalizeDialog.
//     - Idle           → pre-arm state during dashboard / preflight.
//
// Threshold:
//   `kStaleMaxMs` (10 s) is the single freshness threshold.  We dropped
//   the old 2-5 s "Degraded/LAGGY" tier — with the layered model the
//   intermediate state we actually care about is RECONNECTING, which
//   is driven by the probe rather than by topic age.
//
// Pre-arm (no scan running) it reports Idle. `arm()` opens a session
// in Connecting — no contact yet, which is not Offline. The first
// stamp of that session moves it to Healthy. If that first stamp
// never arrives, Connecting becomes Disconnected after
// kConnectingTimeoutMs; a host that still answers pings does not
// extend the wait. `disarm()` returns to
// Idle. `arm()` always opens a new session, even if one is already
// running: a mission must not inherit the previous mission's verdict
// or its "seconds in this state" clock. Call `arm()` when a launch
// starts and `disarm()` when teardown starts, not after the sweep
// has already killed the topics.
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
        Idle,           // pre-arm: no scan in progress, monitor inert
        Connecting,     // armed, no stamp yet, under kConnectingTimeoutMs.
                        // Not offline: the stack has not had a chance
                        // to speak. Past the timeout this is
                        // Disconnected instead.
        Healthy,        // any tracked source < kStaleMaxMs old
        Reconnecting,   // sources stale, but probe reports reachable
        Disconnected,   // sources stale AND probe reports unreachable
                        // (or no probe state available + sources stale)
    };

    explicit LinkHealthMonitor(QObject* parent = nullptr);
    ~LinkHealthMonitor() override;

    // Open a tracking session. Clears every source timestamp, the
    // reachability mirror and the state clock, then reports Connecting.
    // Not a no-op when already armed — that was how a finished mission
    // left the next one in Offline with a running timer.
    void arm();

    // Stop emitting linkStateChanged and revert to Idle.
    // Idempotent. Use when teardown starts, before the sweep kills topics.
    void disarm();

    // Stamp `now()` for the given source.  Cheap; safe to call from any
    // ROS callback.  When transitioning out of Disconnected /
    // Reconnecting, the next `evaluate()` tick fires
    // `linkStateChanged(...)`.
    void stamp(Source source);

    // Force an immediate evaluation (otherwise driven by 250 ms timer).
    void evaluateNow();

    // Push the latest network-layer reachability reading from
    // `RobotReachabilityProbe`.  `reachable` flips the state machine
    // between Reconnecting and Disconnected when topics are stale.
    // `reachable_known=false` means "no probe data yet" — we treat
    // unknown as not-reachable so a stale-and-unprobed link surfaces
    // as DISCONNECTED rather than RECONNECTING (conservative).  Cheap;
    // triggers an immediate re-evaluation if armed.
    void setReachability(bool reachable, bool reachable_known = true);

    State state() const { return state_; }
    bool isArmed() const { return armed_; }

    // Wall-clock ms of the current arm(), or 0 when disarmed. A stamp
    // or an FPV frame from before this belongs to the previous session.
    qint64 armedAtMs() const { return armed_at_ms_; }

    // Milliseconds since the most recent stamp from any tracked source.
    // Returns -1 if no source has ever been stamped since arm().
    qint64 msSinceLastSeen() const;

    // How long the robot has been silent: msSinceLastSeen(), or the time
    // since arm() when this session has had no stamp. -1 when disarmed.
    // Feeds the OFFLINE counters, so a connecting timeout reads "120 s",
    // not "0 s".
    qint64 msWithoutContact() const;

    // True once any source has been stamped since arm().
    bool hadContact() const { return msSinceLastSeen() >= 0; }

    // Milliseconds since the monitor entered the current `state_`.
    // Useful for "Robot offline 12s" UI strings.
    qint64 msInCurrentState() const;

    // Per-source freshness.  -1 if never stamped since arm().
    qint64 msSinceLastSeen(Source source) const;

    static const char* sourceName(Source source);

    // Single freshness threshold for the layered model.  The RF /
    // Zenoh combo can chew up 5-8 s on a peer-rediscovery cycle after
    // a brief fade; 10 s leaves headroom for that without bleeding
    // into the no-data-coming-back true-offline window.  Tunable here
    // so field-test feedback can adjust without touching call sites.
    static constexpr qint64 kStaleMaxMs = 10000;

    // How long an armed session may sit with no stamp before it is
    // Disconnected. A reachable host does not extend it: the stack never
    // spoke, so Complete Mission takes the offline path instead of the
    // save-and-disarm wait. Once any stamp has landed, this ceiling no
    // longer applies. Stage 6's "no data from the robot" prompt is a
    // separate, later clock (180 s); that prompt only asks whether to
    // keep waiting, this decides the Complete Mission path.
    static constexpr qint64 kConnectingTimeoutMs = 120000;

    // Pure decision derive() uses. newest_stamp_ms == 0 means this
    // session has had no stamp. Tests call this with a synthetic clock
    // so the 120 s ceiling does not need a real wait.
    static State deriveLinkState(qint64 now_ms, qint64 armed_at_ms,
                                 qint64 newest_stamp_ms, bool reachable_known,
                                 bool reachable);

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
    qint64 armed_at_ms_ = 0;
    std::array<qint64, static_cast<size_t>(Source::kCount)> last_seen_ms_{};

    // Network-layer reachability mirror, pushed in by AppShell from
    // RobotReachabilityProbe.  `reachable_known_` flips to true on the
    // first probe completion; until then we conservatively treat the
    // host as unreachable.
    bool reachable_ = false;
    bool reachable_known_ = false;

    static constexpr int kEvaluateIntervalMs = 250;
};

}  // namespace f2c_cpp
