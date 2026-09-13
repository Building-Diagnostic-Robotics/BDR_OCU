/**
 * @file thumb_drive_watcher.cpp
 * @brief Implementation of ThumbDriveWatcher.
 */

#include "thumb_drive_watcher.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTimer>

namespace f2c_cpp {

const char* const ThumbDriveWatcher::kDefaultLabel = "RDATA_EXT";

namespace {

// /proc/self/mounts escapes space, tab, newline and backslash in the
// device and mount-point fields as \040, \011, \012, \134.
QString unescapeMountField(const QString& field) {
    QString out;
    out.reserve(field.size());
    for (int i = 0; i < field.size(); ++i) {
        const QChar c = field.at(i);
        if (c == QLatin1Char('\\') && i + 3 < field.size()) {
            bool ok = false;
            const int code = field.mid(i + 1, 3).toInt(&ok, 8);
            if (ok) {
                out += QChar(code);
                i += 3;
                continue;
            }
        }
        out += c;
    }
    return out;
}

QString readProcMounts() {
    QFile f(QStringLiteral("/proc/self/mounts"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    return QString::fromUtf8(f.readAll());
}

// A mount point is only usable as a data root if we can list it. A stick
// mounted read-only is fine for enumeration but the uploader must write
// `upload_state.json` next to the data, so demand writability too.
bool usableDataRoot(const QString& path) {
    if (path.isEmpty()) return false;
    const QFileInfo fi(path);
    return fi.exists() && fi.isDir() && fi.isReadable() && fi.isWritable();
}

}  // namespace

ThumbDriveWatcher::ThumbDriveWatcher(QObject* parent)
    : QObject(parent), label_(QString::fromLatin1(kDefaultLabel)) {
    timer_ = new QTimer(this);
    timer_->setInterval(kPollIntervalMs);
    timer_->setSingleShot(false);
    connect(timer_, &QTimer::timeout, this, &ThumbDriveWatcher::evaluate);
}

ThumbDriveWatcher::~ThumbDriveWatcher() {
    if (mount_proc_ && mount_proc_->state() != QProcess::NotRunning) {
        mount_proc_->disconnect(this);
        mount_proc_->kill();
        mount_proc_->waitForFinished(200);
    }
}

void ThumbDriveWatcher::setLabel(const QString& label) {
    const QString trimmed = label.trimmed();
    if (trimmed.isEmpty() || trimmed == label_) return;
    label_ = trimmed;
    mount_attempted_device_.clear();
    if (isActive()) evaluate();
}

void ThumbDriveWatcher::setManualPath(const QString& path) {
    manual_path_ = QDir::cleanPath(path.trimmed());
    if (manual_path_ == QLatin1String(".")) manual_path_.clear();
    if (isActive()) evaluate();
}

void ThumbDriveWatcher::start() {
    if (!timer_->isActive()) timer_->start();
    evaluate();
}

void ThumbDriveWatcher::stop() {
    timer_->stop();
}

bool ThumbDriveWatcher::isActive() const {
    return timer_ && timer_->isActive();
}

void ThumbDriveWatcher::poll() {
    evaluate();
}

qint64 ThumbDriveWatcher::freeBytes() const {
    if (state_ != State::Mounted) return -1;
    const QStorageInfo info(mount_path_);
    return info.isValid() && info.isReady() ? info.bytesAvailable() : -1;
}

QString ThumbDriveWatcher::mountPointForDevice(const QString& device,
                                               const QString& mounts_text) {
    if (device.isEmpty()) return QString();
    const QStringList lines = mounts_text.split(QLatin1Char('\n'),
                                                Qt::SkipEmptyParts);
    // Later lines shadow earlier ones when a device is mounted twice
    // (bind mounts); the last mount is the one the desktop shows.
    QString found;
    for (const QString& line : lines) {
        const QStringList fields = line.split(QLatin1Char(' '),
                                              Qt::SkipEmptyParts);
        if (fields.size() < 2) continue;
        if (unescapeMountField(fields.at(0)) == device) {
            found = unescapeMountField(fields.at(1));
        }
    }
    return found;
}

void ThumbDriveWatcher::evaluate() {
    // 1. Operator override wins while it exists.
    if (!manual_path_.isEmpty()) {
        if (usableDataRoot(manual_path_)) {
            applyState(State::Mounted, manual_path_);
            return;
        }
        // A manual path that vanished (stick pulled) falls through to
        // auto-detection rather than pinning the dialog to a dead path.
    }

    // 2. Label → device → mount point.
    const QString by_label =
        QStringLiteral("/dev/disk/by-label/") + label_;
    const QFileInfo link(by_label);
    QString device;
    if (link.exists()) {
        device = link.isSymLink() ? QFileInfo(link.symLinkTarget()).canonicalFilePath()
                                  : link.canonicalFilePath();
    }
    if (!device.isEmpty()) {
        const QString mount = mountPointForDevice(device, readProcMounts());
        if (usableDataRoot(mount)) {
            mount_attempted_device_.clear();
            applyState(State::Mounted, mount);
            return;
        }
        if (mount.isEmpty()) {
            requestMount(device);
            applyState(State::PresentUnmounted, QString());
            return;
        }
        // Mounted but not writable (read-only stick, foreign ownership):
        // report as mounted so the dialog can explain, rather than
        // silently pretending the stick is absent.
        if (QFileInfo(mount).isDir()) {
            applyState(State::Mounted, mount);
            return;
        }
    }

    // 3. Conventional desktop automount paths.
    const QString user = qEnvironmentVariable("USER");
    const QStringList candidates = {
        QStringLiteral("/media/%1/%2").arg(user, label_),
        QStringLiteral("/run/media/%1/%2").arg(user, label_),
        QStringLiteral("/media/%1").arg(label_),
        QStringLiteral("/mnt/%1").arg(label_),
    };
    for (const QString& c : candidates) {
        if (usableDataRoot(c)) {
            applyState(State::Mounted, c);
            return;
        }
    }

    applyState(State::Absent, QString());
}

void ThumbDriveWatcher::requestMount(const QString& device) {
    if (device == mount_attempted_device_) return;   // already tried this stick
    if (mount_proc_ && mount_proc_->state() != QProcess::NotRunning) return;
    mount_attempted_device_ = device;

    // udisksctl mounts as the calling user via polkit — the same call the
    // robot's rdata_thumbdrive_sync.py makes. Best-effort: on failure
    // the state stays PresentUnmounted and the dialog tells the operator
    // to mount it from the file manager or Browse… to it.
    mount_proc_ = new QProcess(this);
    connect(mount_proc_,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int, QProcess::ExitStatus) {
                if (mount_proc_) {
                    mount_proc_->deleteLater();
                    mount_proc_ = nullptr;
                }
                evaluate();
            });
    mount_proc_->setProgram(QStringLiteral("udisksctl"));
    mount_proc_->setArguments({QStringLiteral("mount"),
                               QStringLiteral("-b"), device,
                               QStringLiteral("--no-user-interaction")});
    mount_proc_->start();
}

void ThumbDriveWatcher::applyState(State state, const QString& mount_path) {
    if (state == state_ && mount_path == mount_path_) return;
    state_ = state;
    mount_path_ = mount_path;
    emit stateChanged(state_, mount_path_);
}

}  // namespace f2c_cpp
