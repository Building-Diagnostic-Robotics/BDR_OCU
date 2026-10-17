/**
 * @file single_instance.hpp
 * @brief One OCU per user: a second launch raises the first and exits.
 *
 * Two OCUs on one laptop each open a ROS node, can both drive and arm the
 * robot, and each one's pre-launch sweep kills the other's mission and
 * zenohd. So only the first copy runs; it is never touched.
 *
 * The lock is update::Lockfile — flock(2) on an O_CLOEXEC fd, the same
 * mechanism the update runner uses. The kernel drops it when the process
 * dies (crash or kill -9 included), so it can never strand the OCU, and
 * O_CLOEXEC keeps it out of everything the OCU spawns (ssh, launch trees,
 * the update runner), so a child can never keep holding it after the OCU
 * is gone.
 *
 * The raise request is a per-user QLocalServer. It is a courtesy only: the
 * lock alone decides who runs.
 */

#pragma once

#include <QObject>
#include <QString>

#include "update/update_lockfile.hpp"

class QLocalServer;

namespace f2c_cpp {

/// <CacheLocation>/ocu_instance.lock. Needs the app/org names set first.
QString instanceLockPath();
/// Per-user local socket name for the raise request.
QString instanceServerName();

class SingleInstance : public QObject {
    Q_OBJECT

public:
    explicit SingleInstance(QObject* parent = nullptr);
    ~SingleInstance() override;

    /** Take the lock, retrying for up to `wait_ms` so a copy that is still
        shutting down (close-and-reopen) does not bounce the new one.
        `path` empty = instanceLockPath(). */
    bool acquire(int wait_ms, const QString& path = QString());
    bool held() const { return lock_.held(); }

    /** Holder only: start answering raise requests (emits raiseRequested). */
    void listen(const QString& server_name = QString());
    /** Stop answering, e.g. once the window has accepted close: a closing
        copy must not be raised. The lock is kept until the process exits. */
    void stopListening();

    /** Second copy: ask the running one to come to the front. True if it
        answered within `timeout_ms`. */
    static bool askRunningToRaise(int timeout_ms,
                                  const QString& server_name = QString());

signals:
    void raiseRequested();

private:
    update::Lockfile lock_;
    QLocalServer* server_ = nullptr;
};

}  // namespace f2c_cpp
