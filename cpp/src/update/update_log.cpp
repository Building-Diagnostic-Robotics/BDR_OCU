#include "update/update_log.hpp"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QString>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>

namespace f2c_cpp::update::log {

namespace {

// Process-wide guard around log emission. POSIX O_APPEND gives us the
// CROSS-process atomicity (writes <= PIPE_BUF land at end-of-file as a
// single contiguous chunk); this mutex only serializes IN-process callers
// so we don't shred log lines when two threads call info() at once.
QMutex& mutex() {
    static QMutex m;
    return m;
}

QString cachedLogPath_;

// Process tag printed on every line. Defaults to "app" if a binary forgets
// to call setProcessTag(); won't crash but the merged log is harder to read.
std::atomic<const char*> g_process_tag{"app"};

QString resolveLogPath() {
    if (!cachedLogPath_.isEmpty()) {
        return cachedLogPath_;
    }
    const QString cacheDir =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cacheDir.isEmpty()) {
        return {};
    }
    QDir().mkpath(cacheDir);
    cachedLogPath_ = cacheDir + QStringLiteral("/update.log");
    return cachedLogPath_;
}

// Rotation. Best-effort cross-process: rename() is atomic so any process
// holding an fd to update.log keeps writing to the rotated file (which then
// gets a fresh update.log created by the next opener). Worst case: one or
// two log lines land in the .1 file instead of update.log immediately
// after a rotation. Acceptable for a debug log.
void rotateIfNeeded(const QString& path) {
    QFileInfo fi(path);
    if (!fi.exists() || fi.size() < kRotateThresholdBytes) {
        return;
    }
    const QString oldest = path + QStringLiteral(".%1").arg(kMaxRotations);
    if (QFileInfo::exists(oldest)) {
        QFile::remove(oldest);
    }
    for (int i = kMaxRotations - 1; i >= 1; --i) {
        const QString src = path + QStringLiteral(".%1").arg(i);
        const QString dst = path + QStringLiteral(".%1").arg(i + 1);
        if (QFileInfo::exists(src)) {
            QFile::rename(src, dst);
        }
    }
    QFile::rename(path, path + QStringLiteral(".1"));
}

// Single-syscall append. Returns true on success.
bool appendLineAtomic(const QString& path, const QByteArray& line) {
    // O_APPEND is the magic ingredient: each write seeks-to-end-and-writes
    // as a single atomic op, so the OCU and the runner can both write to
    // this file concurrently without interleaving bytes.
    const int fd = ::open(path.toLocal8Bit().constData(),
                          O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC,
                          0644);
    if (fd < 0) {
        std::cerr << "[OTA-log] open(" << path.toStdString()
                  << ") failed: " << std::strerror(errno) << std::endl;
        return false;
    }
    const ssize_t want = line.size();
    ssize_t got = ::write(fd, line.constData(), static_cast<size_t>(want));
    int saved_errno = errno;
    ::close(fd);
    if (got != want) {
        std::cerr << "[OTA-log] short write to " << path.toStdString()
                  << ": got=" << got << " want=" << want
                  << " errno=" << std::strerror(saved_errno) << std::endl;
        return false;
    }
    return true;
}

void writeLine(const char* level, const char* component, const QString& msg) {
    QMutexLocker locker(&mutex());

    const QString path = resolveLogPath();
    const char* tag = g_process_tag.load(std::memory_order_relaxed);
    const QString stamp =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);

    // Format: "<UTC ISO timestamp> <LEVEL> [tag] component: message\n".
    // The bracketed tag separates OCU lines from runner lines in the merged
    // file (concern #1).
    const QString line = QStringLiteral("%1 %2 [%3] %4: %5\n")
                             .arg(stamp)
                             .arg(QString::fromLatin1(level))
                             .arg(QString::fromLatin1(tag))
                             .arg(QString::fromLatin1(component))
                             .arg(msg);
    const QByteArray bytes = line.toUtf8();

    if (!path.isEmpty()) {
        rotateIfNeeded(path);
        appendLineAtomic(path, bytes);
    } else {
        std::cerr << "[OTA-log] cannot resolve log path" << std::endl;
    }

    // Tee to stderr so dev runs still surface the message.
    std::cerr.write(bytes.constData(), bytes.size());
}

}  // namespace

void setProcessTag(const char* tag) {
    if (tag == nullptr || tag[0] == '\0') return;
    g_process_tag.store(tag, std::memory_order_relaxed);
}

void info(const char* component, const QString& msg) {
    writeLine("INFO", component, msg);
}

void warn(const char* component, const QString& msg) {
    writeLine("WARN", component, msg);
}

void error(const char* component, const QString& msg) {
    writeLine("ERROR", component, msg);
}

QString currentLogPath() {
    QMutexLocker locker(&mutex());
    return resolveLogPath();
}

}  // namespace f2c_cpp::update::log
