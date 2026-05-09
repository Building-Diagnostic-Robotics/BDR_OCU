#include "update/update_lockfile.hpp"

#include <QByteArray>
#include <QDir>
#include <QStandardPaths>
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/types.h>
#include <unistd.h>

namespace f2c_cpp::update {

QString runnerLockfilePath() {
    const QString cacheDir =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cacheDir.isEmpty()) return {};
    QDir().mkpath(cacheDir);
    return cacheDir + QStringLiteral("/update_runner.lock");
}

Lockfile::~Lockfile() {
    release();
}

bool Lockfile::tryAcquire(const QString& path_in) {
    if (held()) return true;

    path_ = path_in.isEmpty() ? runnerLockfilePath() : path_in;
    if (path_.isEmpty()) return false;

    // O_CLOEXEC so the fd doesn't leak into any QProcess we spawn.
    const int fd = ::open(path_.toLocal8Bit().constData(),
                          O_RDWR | O_CREAT | O_CLOEXEC,
                          0644);
    if (fd < 0) return false;

    if (::flock(fd, LOCK_EX | LOCK_NB) < 0) {
        // EWOULDBLOCK = held by another process. Anything else is unexpected
        // but treated the same way: refuse to acquire.
        ::close(fd);
        return false;
    }

    // Best-effort: write our PID into the file so the OCU's wait loop can
    // confirm the runner is the one holding the lock and so a postmortem
    // shows who's holding it. Truncate first because the lockfile may
    // contain a previous runner's PID.
    if (::ftruncate(fd, 0) == 0) {
        const QByteArray payload = QByteArray::number(qlonglong(::getpid()))
                                       + '\n';
        ssize_t n = ::write(fd, payload.constData(),
                            static_cast<size_t>(payload.size()));
        (void)n;  // PID write failure is non-fatal; lock semantics still hold.
    }

    fd_ = fd;
    return true;
}

void Lockfile::release() {
    if (fd_ < 0) return;
    // close() implicitly releases the flock — no need to LOCK_UN explicitly.
    ::close(fd_);
    fd_ = -1;
    // We deliberately do NOT unlink() the file here: leaving a zero-byte
    // marker in place is fine and a tiny bit cheaper for the next runner.
    // Stale-PID corner cases are handled by tryAcquire's truncate.
}

bool isRunnerLockfileHeld() {
    const QString path = runnerLockfilePath();
    if (path.isEmpty()) return false;

    // Probe: open the file (don't create it) and try a non-blocking
    // exclusive lock. EWOULDBLOCK means somebody else owns it.
    const int fd = ::open(path.toLocal8Bit().constData(),
                          O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        // No file yet → nobody holds it.
        return false;
    }
    bool held = false;
    if (::flock(fd, LOCK_EX | LOCK_NB) < 0) {
        held = (errno == EWOULDBLOCK);
    } else {
        // We got the lock — release it immediately. close() does that.
    }
    ::close(fd);
    return held;
}

}  // namespace f2c_cpp::update
