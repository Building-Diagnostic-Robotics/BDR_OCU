#include "robot_reachability_probe.hpp"

#include <QProcess>
#include <QTcpSocket>
#include <QTimer>

namespace f2c_cpp {

RobotReachabilityProbe::RobotReachabilityProbe(QObject* parent)
    : QObject(parent),
      tick_timer_(new QTimer(this)),
      tcp_timeout_timer_(new QTimer(this)) {
    tick_timer_->setInterval(kProbeIntervalMs);
    tick_timer_->setSingleShot(false);
    connect(tick_timer_, &QTimer::timeout, this, &RobotReachabilityProbe::onTick);

    tcp_timeout_timer_->setInterval(kTcpProbeTimeoutMs);
    tcp_timeout_timer_->setSingleShot(true);
    connect(tcp_timeout_timer_, &QTimer::timeout,
            this, &RobotReachabilityProbe::onTcpError);
}

RobotReachabilityProbe::~RobotReachabilityProbe() {
    cancelInFlight();
}

void RobotReachabilityProbe::arm(const QString& host) {
    if (host.isEmpty()) {
        // Refuse to arm without a target — caller bug.
        return;
    }
    const bool host_changed = (host_ != host);
    host_ = host;
    if (armed_ && !host_changed) {
        return;  // already running on this host
    }
    armed_ = true;
    consecutive_failures_ = 0;
    // Don't pre-emit a state change here.  The first probe completion
    // will publish the real result; consumers see Idle until then so a
    // brief "Unreachable" flicker before the first ping never appears.
    state_ = State::Idle;
    cancelInFlight();
    tick_timer_->start();
    // Fire one probe immediately so the first reading is sub-second
    // instead of 1 s out.
    QTimer::singleShot(0, this, [this]() {
        if (armed_) {
            startProbe();
        }
    });
}

void RobotReachabilityProbe::disarm() {
    if (!armed_) {
        return;
    }
    armed_ = false;
    tick_timer_->stop();
    cancelInFlight();
    consecutive_failures_ = 0;
    state_ = State::Idle;
    // Intentionally no signal emit — see header.
}

void RobotReachabilityProbe::onTick() {
    if (!armed_) {
        return;
    }
    if (probe_in_flight_) {
        // Previous probe still pending (e.g. host doesn't respond and
        // both ping + TCP are timing out).  Skip this tick rather
        // than stack subprocess fan-out.  The pending probe's
        // failure path will recordFailure() when its timeout fires.
        return;
    }
    startProbe();
}

void RobotReachabilityProbe::startProbe() {
    if (!armed_ || host_.isEmpty()) {
        return;
    }
    probe_in_flight_ = true;
    ping_phase_ = true;

    // ICMP first.  -c 1 = single packet, -W 1 = 1 s wait, -q = quiet.
    // The exit code tells us reachability; we don't parse stdout.
    if (!ping_proc_) {
        ping_proc_ = new QProcess(this);
        ping_proc_->setProcessChannelMode(QProcess::MergedChannels);
        connect(ping_proc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int exit_code, QProcess::ExitStatus /*status*/) {
                    onPingFinished(exit_code);
                });
        connect(ping_proc_, &QProcess::errorOccurred, this,
                [this](QProcess::ProcessError /*err*/) {
                    // Treat spawn failure ('ping' missing, etc.) as a
                    // ping-phase failure so we fall through to TCP.
                    if (probe_in_flight_ && ping_phase_) {
                        onPingFinished(-1);
                    }
                });
    }
    if (ping_proc_->state() != QProcess::NotRunning) {
        ping_proc_->kill();
        ping_proc_->waitForFinished(50);
    }
    QStringList args;
    args << "-c" << "1" << "-W" << "1" << "-q" << host_;
    ping_proc_->start("ping", args);
}

void RobotReachabilityProbe::onPingFinished(int exit_code) {
    if (!armed_ || !probe_in_flight_) {
        return;
    }
    if (exit_code == 0) {
        probe_in_flight_ = false;
        recordSuccess();
        return;
    }
    // Ping failed (host down OR ICMP blocked OR cap_net_raw missing).
    // Fall back to TCP-22.  Since this is a layered model we treat any
    // path that gets bytes to/from the host as "Reachable".
    ping_phase_ = false;

    if (!tcp_socket_) {
        tcp_socket_ = new QTcpSocket(this);
        connect(tcp_socket_, &QTcpSocket::connected,
                this, &RobotReachabilityProbe::onTcpConnected);
        // Connection refused / unreachable / DNS fail all funnel here.
        connect(tcp_socket_,
                QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::errorOccurred),
                this, [this](QAbstractSocket::SocketError /*err*/) {
                    onTcpError();
                });
    }
    if (tcp_socket_->state() != QAbstractSocket::UnconnectedState) {
        tcp_socket_->abort();
    }
    tcp_timeout_timer_->start(kTcpProbeTimeoutMs);
    tcp_socket_->connectToHost(host_, 22);
}

void RobotReachabilityProbe::onTcpConnected() {
    if (!armed_ || !probe_in_flight_) {
        return;
    }
    tcp_timeout_timer_->stop();
    if (tcp_socket_) {
        tcp_socket_->abort();  // bare connect was the test; we don't speak SSH
    }
    probe_in_flight_ = false;
    recordSuccess();
}

void RobotReachabilityProbe::onTcpError() {
    if (!armed_ || !probe_in_flight_) {
        return;
    }
    tcp_timeout_timer_->stop();
    if (tcp_socket_) {
        tcp_socket_->abort();
    }
    probe_in_flight_ = false;
    recordFailure();
}

void RobotReachabilityProbe::cancelInFlight() {
    probe_in_flight_ = false;
    ping_phase_ = true;
    if (tcp_timeout_timer_) {
        tcp_timeout_timer_->stop();
    }
    if (ping_proc_ && ping_proc_->state() != QProcess::NotRunning) {
        ping_proc_->kill();
        ping_proc_->waitForFinished(50);
    }
    if (tcp_socket_ && tcp_socket_->state() != QAbstractSocket::UnconnectedState) {
        tcp_socket_->abort();
    }
}

void RobotReachabilityProbe::recordSuccess() {
    consecutive_failures_ = 0;
    setState(State::Reachable);
}

void RobotReachabilityProbe::recordFailure() {
    if (consecutive_failures_ < 1000000) {
        ++consecutive_failures_;
    }
    if (consecutive_failures_ >= kFailuresToUnreachable) {
        setState(State::Unreachable);
    }
    // Otherwise stay in current state (debounce).  A single ping miss
    // on a flaky RF link doesn't change anything.
}

void RobotReachabilityProbe::setState(State next) {
    if (next == state_) {
        return;
    }
    const State old = state_;
    state_ = next;
    emit reachabilityChanged(old, next);
}

}  // namespace f2c_cpp
