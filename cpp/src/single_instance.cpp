#include "single_instance.hpp"

#include <QDeadlineTimer>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QThread>

#include <unistd.h>

namespace f2c_cpp {

namespace {
constexpr char kRaiseRequest[] = "raise\n";
constexpr char kRaiseReply[] = "ok\n";
}  // namespace

QString instanceLockPath() {
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (dir.isEmpty()) {
        return {};
    }
    QDir().mkpath(dir);
    return dir + QStringLiteral("/ocu_instance.lock");
}

QString instanceServerName() {
    return QStringLiteral("bdr-ocu-%1").arg(::getuid());
}

SingleInstance::SingleInstance(QObject* parent) : QObject(parent) {}

SingleInstance::~SingleInstance() = default;  // lock_ releases in its dtor

bool SingleInstance::acquire(int wait_ms, const QString& path) {
    const QString lock_path = path.isEmpty() ? instanceLockPath() : path;
    if (lock_path.isEmpty()) {
        // No writable cache dir: refusing to start would strand the operator
        // over a guard. Run unguarded, as the OCU always has.
        return true;
    }
    const QDeadlineTimer deadline(wait_ms);
    while (true) {
        if (lock_.tryAcquire(lock_path)) {
            return true;
        }
        if (deadline.hasExpired()) {
            return false;
        }
        QThread::msleep(100);
    }
}

void SingleInstance::listen(const QString& server_name) {
    if (server_ || !lock_.held()) {
        return;
    }
    const QString name =
        server_name.isEmpty() ? instanceServerName() : server_name;
    // We hold the lock, so no live OCU owns this name: anything left is a
    // socket file from a crashed run.
    QLocalServer::removeServer(name);
    server_ = new QLocalServer(this);
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server_->listen(name)) {
        // Raising is a courtesy; the lock still keeps a second copy out.
        return;
    }
    connect(server_, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket* socket = server_->nextPendingConnection()) {
            connect(socket, &QLocalSocket::disconnected, socket,
                    &QObject::deleteLater);
            connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
                if (!socket->readAll().startsWith("raise")) {
                    return;
                }
                socket->write(kRaiseReply);
                socket->flush();
                emit raiseRequested();
            });
        }
    });
}

void SingleInstance::stopListening() {
    if (server_) {
        server_->close();
    }
}

bool SingleInstance::askRunningToRaise(int timeout_ms,
                                       const QString& server_name) {
    QLocalSocket socket;
    socket.connectToServer(server_name.isEmpty() ? instanceServerName()
                                                 : server_name);
    const QDeadlineTimer deadline(timeout_ms);
    if (!socket.waitForConnected(int(deadline.remainingTime()))) {
        return false;
    }
    socket.write(kRaiseRequest);
    if (!socket.waitForBytesWritten(int(deadline.remainingTime()))) {
        return false;
    }
    while (!socket.canReadLine()) {
        if (deadline.hasExpired() ||
            !socket.waitForReadyRead(int(deadline.remainingTime()))) {
            return false;
        }
    }
    return socket.readLine() == kRaiseReply;
}

}  // namespace f2c_cpp
