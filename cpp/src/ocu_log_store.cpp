/**
 * @file ocu_log_store.cpp
 * @brief See ocu_log_store.hpp.
 */
#include "ocu_log_store.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStorageInfo>

#include <algorithm>

namespace f2c_cpp::ocu_logs {

namespace {

bool copyAtomically(const QString& src, const QString& dst, QString* err) {
    const QString part = dst + QStringLiteral(".part");
    QFile::remove(part);
    if (!QFile::copy(src, part)) {
        *err = QStringLiteral("copy %1 failed").arg(QFileInfo(src).fileName());
        QFile::remove(part);
        return false;
    }
    QFile::remove(dst);  // QFile::rename refuses to overwrite
    if (!QFile::rename(part, dst)) {
        *err = QStringLiteral("rename %1 failed").arg(QFileInfo(dst).fileName());
        QFile::remove(part);
        return false;
    }
    return true;
}

QStringList groupFiles(const QString& store_dir, const QString& id) {
    QStringList out;
    for (const char* suffix : {kLogSuffix, kLaptopRosSuffix, kUploadedSuffix}) {
        const QString p = QDir(store_dir).filePath(id + QLatin1String(suffix));
        if (QFileInfo::exists(p)) {
            out << p;
        }
    }
    // Leftover ".part" from a packer that was interrupted.
    const QString part = QDir(store_dir).filePath(
        id + QLatin1String(kLaptopRosSuffix) + QStringLiteral(".part"));
    if (QFileInfo::exists(part)) {
        out << part;
    }
    return out;
}

}  // namespace

QString storeDir() {
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
        QStringLiteral("/mission_logs");
    return QDir().mkpath(dir) ? dir : QString();
}

QString sanitizeLogId(const QString& raw) {
    static const QRegularExpression ok(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$"));
    const QString id = raw.trimmed();
    if (!ok.match(id).hasMatch() || id.contains(QStringLiteral(".."))) {
        return QString();
    }
    return id;
}

QStringList idsForMission(const QString& mission_dir) {
    QFile f(QDir(mission_dir).filePath(QStringLiteral("mission_config.json")));
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    QStringList out;
    for (const QJsonValue& v : doc.object().value(QStringLiteral("ocu_logs")).toArray()) {
        const QString id = sanitizeLogId(v.toString());
        if (!id.isEmpty() && !out.contains(id)) {
            out << id;
        }
    }
    return out;
}

bool missionAlreadyUploaded(const QString& mission_dir) {
    return QFileInfo::exists(QDir(mission_dir).filePath(QStringLiteral("manifest.json")));
}

AttachResult attachToMission(const QString& mission_dir, const QString& store_dir) {
    // Runs on a worker thread. A cancel + restart can briefly overlap two
    // attaches of the same mission: serialise them so they never share a
    // ".part"; the second then finds the files present.
    static QMutex mutex;
    QMutexLocker lock(&mutex);
    AttachResult r;
    r.ids = idsForMission(mission_dir);
    if (r.ids.isEmpty() || store_dir.isEmpty()) {
        return r;
    }
    const QString dest_dir = QDir(mission_dir).filePath(QLatin1String(kMissionSubdir));
    for (const QString& id : r.ids) {
        bool any = false;
        for (const char* suffix : {kLogSuffix, kLaptopRosSuffix}) {
            const QString name = id + QLatin1String(suffix);
            const QFileInfo src(QDir(store_dir).filePath(name));
            if (!src.isFile()) {
                continue;
            }
            any = true;
            if (!QDir().mkpath(dest_dir)) {
                if (r.error.isEmpty()) {
                    r.error = QStringLiteral("cannot create %1").arg(kMissionSubdir);
                }
                return r;
            }
            const QString dst = QDir(dest_dir).filePath(name);
            const QFileInfo existing(dst);
            if (existing.isFile() && existing.size() == src.size()) {
                r.present << name;
                continue;
            }
            QString err;
            if (copyAtomically(src.filePath(), dst, &err)) {
                r.copied << name;
            } else if (r.error.isEmpty()) {
                r.error = err;
            }
        }
        if (!any) {
            r.missing << id;
        }
    }
    return r;
}

QStringList markUploaded(const QString& mission_dir, const QString& store_dir) {
    QStringList marked;
    if (store_dir.isEmpty()) {
        return marked;
    }
    const QDir dest(QDir(mission_dir).filePath(QLatin1String(kMissionSubdir)));
    for (const QString& id : idsForMission(mission_dir)) {
        const QString name = id + QLatin1String(kLogSuffix);
        const QFileInfo mine(QDir(store_dir).filePath(name));
        const QFileInfo sent(dest.filePath(name));
        if (!mine.isFile()) {
            continue;  // not this laptop's log
        }
        if (!sent.isFile() || sent.size() != mine.size()) {
            continue;  // this copy is not what went up
        }
        QFile m(QDir(store_dir).filePath(id + QLatin1String(kUploadedSuffix)));
        if (m.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            m.write(QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toUtf8());
            marked << id;
        }
    }
    return marked;
}

PruneResult prune(const QString& store_dir, qint64 floor,
                  const std::function<qint64()>& free_bytes,
                  const QString& keep_id) {
    PruneResult r;
    if (store_dir.isEmpty() || !free_bytes) {
        return r;
    }
    if (free_bytes() < 0 || free_bytes() >= floor) {
        return r;  // unknown free space: never delete blind
    }
    // One group per id; age = the .log's mtime (else the bundle's).
    struct Group {
        QString id;
        QDateTime age;
        bool uploaded = false;
    };
    QMap<QString, Group> groups;
    const QFileInfoList files = QDir(store_dir).entryInfoList(QDir::Files);
    for (const QFileInfo& fi : files) {
        const QString name = fi.fileName();
        QString id;
        for (const char* suffix : {kLaptopRosSuffix, kLogSuffix, kUploadedSuffix}) {
            const QString s = QLatin1String(suffix);
            if (name.endsWith(s)) {
                id = name.left(name.size() - s.size());
                break;
            }
        }
        const QString part_suffix =
            QLatin1String(kLaptopRosSuffix) + QStringLiteral(".part");
        if (name.endsWith(part_suffix)) {
            id = name.left(name.size() - part_suffix.size());
        }
        id = sanitizeLogId(id);
        if (id.isEmpty() || id == keep_id) {
            continue;
        }
        Group& g = groups[id];
        g.id = id;
        if (name.endsWith(QLatin1String(kUploadedSuffix))) {
            g.uploaded = true;
        } else if (!g.age.isValid() || name.endsWith(QLatin1String(kLogSuffix))) {
            g.age = fi.lastModified();
        }
    }
    QList<Group> order = groups.values();
    std::sort(order.begin(), order.end(), [](const Group& a, const Group& b) {
        if (a.uploaded != b.uploaded) {
            return a.uploaded;  // uploaded first
        }
        return a.age < b.age;   // then oldest first
    });
    for (const Group& g : order) {
        if (free_bytes() >= floor) {
            break;
        }
        for (const QString& p : groupFiles(store_dir, g.id)) {
            QFile::remove(p);
        }
        (g.uploaded ? r.deleted_uploaded : r.deleted_not_uploaded) << g.id;
    }
    r.floor_reached = free_bytes() >= floor;
    return r;
}

qint64 freeBytesAt(const QString& path) {
    const QStorageInfo info(path);
    return info.isValid() && info.isReady() ? info.bytesAvailable() : -1;
}

}  // namespace f2c_cpp::ocu_logs
