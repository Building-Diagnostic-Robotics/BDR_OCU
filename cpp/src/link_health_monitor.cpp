#include "link_health_monitor.hpp"

#include <QDateTime>
#include <QTimer>

#include <algorithm>
#include <limits>

namespace f2c_cpp {

LinkHealthMonitor::LinkHealthMonitor(QObject* parent)
    : QObject(parent),
      timer_(new QTimer(this)) {
    timer_->setInterval(kEvaluateIntervalMs);
    connect(timer_, &QTimer::timeout, this, &LinkHealthMonitor::evaluate);
    last_seen_ms_.fill(0);
}

LinkHealthMonitor::~LinkHealthMonitor() = default;

void LinkHealthMonitor::arm() {
    if (armed_) {
        return;
    }
    armed_ = true;
    last_seen_ms_.fill(0);
    // Honest starting state until the first stamp + first probe land.
    // We use Disconnected (not Reconnecting) because we have no
    // reachability data yet — be conservative.
    reachable_ = false;
    reachable_known_ = false;
    state_ = State::Disconnected;
    state_entered_at_ms_ = QDateTime::currentMSecsSinceEpoch();
    timer_->start();
    // Fire so consumers can immediately render "awaiting first contact".
    emit linkStateChanged(State::Idle, State::Disconnected, 0);
}

void LinkHealthMonitor::disarm() {
    if (!armed_) {
        return;
    }
    timer_->stop();
    const State old = state_;
    armed_ = false;
    state_ = State::Idle;
    state_entered_at_ms_ = QDateTime::currentMSecsSinceEpoch();
    last_seen_ms_.fill(0);
    reachable_ = false;
    reachable_known_ = false;
    if (old != State::Idle) {
        emit linkStateChanged(old, State::Idle, 0);
    }
}

void LinkHealthMonitor::stamp(Source source) {
    if (!armed_) {
        return;
    }
    const auto idx = static_cast<size_t>(source);
    if (idx >= last_seen_ms_.size()) {
        return;
    }
    last_seen_ms_[idx] = QDateTime::currentMSecsSinceEpoch();
    // Fast-path recovery: if we were in any non-Healthy state and a
    // fresh stamp lands, evaluate immediately so the UI un-freezes
    // within ms rather than waiting up to 250 ms for the next timer
    // tick.
    if (state_ != State::Healthy) {
        evaluate();
    }
}

void LinkHealthMonitor::setReachability(bool reachable, bool reachable_known) {
    if (reachable_ == reachable && reachable_known_ == reachable_known) {
        return;
    }
    reachable_ = reachable;
    reachable_known_ = reachable_known;
    // Reachability flips can change RECONNECTING <-> DISCONNECTED
    // without any new ROS stamp, so re-derive immediately.
    if (armed_) {
        evaluate();
    }
}

void LinkHealthMonitor::evaluateNow() {
    if (armed_) {
        evaluate();
    }
}

qint64 LinkHealthMonitor::msSinceLastSeen() const {
    qint64 newest = 0;
    for (qint64 t : last_seen_ms_) {
        newest = std::max(newest, t);
    }
    if (newest == 0) {
        return -1;
    }
    return QDateTime::currentMSecsSinceEpoch() - newest;
}

qint64 LinkHealthMonitor::msInCurrentState() const {
    if (state_entered_at_ms_ == 0) {
        return 0;
    }
    return QDateTime::currentMSecsSinceEpoch() - state_entered_at_ms_;
}

qint64 LinkHealthMonitor::msSinceLastSeen(Source source) const {
    const auto idx = static_cast<size_t>(source);
    if (idx >= last_seen_ms_.size() || last_seen_ms_[idx] == 0) {
        return -1;
    }
    return QDateTime::currentMSecsSinceEpoch() - last_seen_ms_[idx];
}

const char* LinkHealthMonitor::sourceName(Source source) {
    switch (source) {
        case Source::Odom:             return "odom";
        case Source::UdcHealth:        return "udc_health";
        case Source::ScanStatus:       return "scan_status";
        case Source::ControllerStatus: return "controller_status";
        case Source::StreamStatus:     return "stream_status";
        case Source::FpvFrame:         return "fpv_frame";
        case Source::kCount:           break;
    }
    return "unknown";
}

LinkHealthMonitor::State LinkHealthMonitor::derive(qint64 now_ms) const {
    qint64 newest_age = std::numeric_limits<qint64>::max();
    for (qint64 t : last_seen_ms_) {
        if (t > 0) {
            newest_age = std::min(newest_age, now_ms - t);
        }
    }
    const bool fresh = (newest_age != std::numeric_limits<qint64>::max()) &&
                       (newest_age < kStaleMaxMs);
    if (fresh) {
        // Topics flowing — call it healthy regardless of probe state.
        // (Probe might still be warming up; topic flow is the gold
        // standard for "I can talk to the robot end-to-end".)
        return State::Healthy;
    }
    // Topics stale (or no stamps yet).  Defer to the network probe to
    // distinguish "Zenoh hiccup, robot still up" from "really gone".
    if (reachable_known_ && reachable_) {
        return State::Reconnecting;
    }
    return State::Disconnected;
}

void LinkHealthMonitor::evaluate() {
    if (!armed_) {
        return;
    }
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    const State next = derive(now_ms);
    if (next == state_) {
        return;
    }
    const State old = state_;
    state_ = next;
    state_entered_at_ms_ = now_ms;
    qint64 since = msSinceLastSeen();
    if (since < 0) {
        since = 0;
    }
    emit linkStateChanged(old, next, since);
}

}  // namespace f2c_cpp
