#include "link_health_monitor.hpp"

#include <QDateTime>
#include <QTimer>

#include <algorithm>

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
    const State old = state_;
    armed_ = true;
    last_seen_ms_.fill(0);
    // Reachability from the previous session must not decide this one.
    // AppShell pushes the probe's current verdict again after arm()
    // when the probe is already past Idle.
    reachable_ = false;
    reachable_known_ = false;
    // No stamp yet. Connecting, not Disconnected: the stack has not
    // had a chance to speak, and Offline is reserved for a session
    // that already had contact and then lost it.
    state_ = State::Connecting;
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    state_entered_at_ms_ = now_ms;
    armed_at_ms_ = now_ms;
    if (!timer_->isActive()) {
        timer_->start();
    }
    emit linkStateChanged(old, State::Connecting, 0);
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
    armed_at_ms_ = 0;
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

qint64 LinkHealthMonitor::msWithoutContact() const {
    if (!armed_) {
        return -1;
    }
    const qint64 since = msSinceLastSeen();
    if (since >= 0) {
        return since;
    }
    return std::max<qint64>(0, QDateTime::currentMSecsSinceEpoch() -
                                   armed_at_ms_);
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

LinkHealthMonitor::State LinkHealthMonitor::deriveLinkState(
    qint64 now_ms, qint64 armed_at_ms, qint64 newest_stamp_ms,
    bool reachable_known, bool reachable) {
    if (newest_stamp_ms <= 0) {
        // No contact this session. The launch gets one window to speak.
        // Past it, a ping-reachable host is still a stack that never
        // came up, and Complete Mission must take the offline path.
        if (armed_at_ms > 0 &&
            now_ms - armed_at_ms >= kConnectingTimeoutMs) {
            return State::Disconnected;
        }
        return State::Connecting;
    }
    const qint64 newest_age = now_ms - newest_stamp_ms;
    if (newest_age < kStaleMaxMs) {
        // Topics flowing — call it healthy regardless of probe state.
        // (Probe might still be warming up; topic flow is the gold
        // standard for "I can talk to the robot end-to-end".)
        return State::Healthy;
    }
    // This session has had contact, and every stamp is now stale.
    // Defer to the network probe to distinguish "Zenoh hiccup, robot
    // still up" from "really gone". The no-contact timeout does not
    // apply once a stamp has landed.
    if (reachable_known && reachable) {
        return State::Reconnecting;
    }
    return State::Disconnected;
}

LinkHealthMonitor::State LinkHealthMonitor::derive(qint64 now_ms) const {
    qint64 newest = 0;
    for (qint64 t : last_seen_ms_) {
        newest = std::max(newest, t);
    }
    return deriveLinkState(now_ms, armed_at_ms_, newest, reachable_known_,
                           reachable_);
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
    emit linkStateChanged(old, next, std::max<qint64>(0, msWithoutContact()));
}

}  // namespace f2c_cpp
