#include "update/update_state.hpp"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include "update/update_log.hpp"

namespace f2c_cpp::update {

namespace {

constexpr const char* kKeySchema      = "schema";
constexpr const char* kKeyStage       = "state";
constexpr const char* kKeyCurrentDeb  = "current_deb_path";
constexpr const char* kKeyPreviousDeb = "previous_deb_path";

}  // namespace

const char* updateStageToWire(UpdateStage stage) {
    switch (stage) {
        case UpdateStage::None:                  return "none";
        case UpdateStage::Downloading:           return "downloading";
        case UpdateStage::DpkgRunning:           return "dpkg_running";
        case UpdateStage::InstalledPendingProbe: return "installed_pending_probe";
        case UpdateStage::ProbingHealth:         return "probing_health";
        case UpdateStage::Done:                  return "done";
        case UpdateStage::RolledBack:            return "rolled_back";
    }
    return "none";
}

UpdateStage updateStageFromWire(const QString& wire) {
    if (wire == QLatin1String("none"))
        return UpdateStage::None;
    if (wire == QLatin1String("downloading"))
        return UpdateStage::Downloading;
    if (wire == QLatin1String("dpkg_running"))
        return UpdateStage::DpkgRunning;
    if (wire == QLatin1String("installed_pending_probe"))
        return UpdateStage::InstalledPendingProbe;
    if (wire == QLatin1String("probing_health"))
        return UpdateStage::ProbingHealth;
    if (wire == QLatin1String("done"))
        return UpdateStage::Done;
    if (wire == QLatin1String("rolled_back"))
        return UpdateStage::RolledBack;
    return UpdateStage::None;
}

QString updateStateFilePath() {
    const QString cacheDir =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cacheDir.isEmpty()) return {};
    QDir().mkpath(cacheDir);
    return cacheDir + QStringLiteral("/update_state.json");
}

bool readUpdateState(UpdateStateData* out) {
    if (out) *out = UpdateStateData{};

    const QString path = updateStateFilePath();
    if (path.isEmpty()) return false;
    if (!QFileInfo::exists(path)) return false;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        log::warn("state",
                  QStringLiteral("readUpdateState: open failed: %1")
                      .arg(f.errorString()));
        return false;
    }
    const QByteArray bytes = f.readAll();
    f.close();

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        log::warn("state",
                  QStringLiteral("readUpdateState: malformed JSON: %1")
                      .arg(perr.errorString()));
        return false;
    }
    const QJsonObject obj = doc.object();
    const int schema =
        obj.value(QLatin1String(kKeySchema)).toInt(0);
    if (schema != kUpdateStateSchemaVersion) {
        log::warn("state",
                  QStringLiteral("readUpdateState: schema=%1 (expected %2), "
                                 "ignoring marker")
                      .arg(schema).arg(kUpdateStateSchemaVersion));
        return false;
    }
    if (!out) return true;

    out->stage = updateStageFromWire(
        obj.value(QLatin1String(kKeyStage)).toString());
    out->currentDebPath =
        obj.value(QLatin1String(kKeyCurrentDeb)).toString();
    out->previousDebPath =
        obj.value(QLatin1String(kKeyPreviousDeb)).toString();
    return true;
}

bool writeUpdateState(const UpdateStateData& in) {
    const QString path = updateStateFilePath();
    if (path.isEmpty()) {
        log::error("state",
                   QStringLiteral("writeUpdateState: empty path"));
        return false;
    }

    QJsonObject obj;
    obj.insert(QLatin1String(kKeySchema), kUpdateStateSchemaVersion);
    obj.insert(QLatin1String(kKeyStage),
               QString::fromLatin1(updateStageToWire(in.stage)));
    obj.insert(QLatin1String(kKeyCurrentDeb), in.currentDebPath);
    obj.insert(QLatin1String(kKeyPreviousDeb), in.previousDebPath);

    const QByteArray bytes = QJsonDocument(obj).toJson(QJsonDocument::Indented);

    // QSaveFile = atomic-rename under the hood. We get either the old
    // file (if power-loss before commit) or the new file (after commit),
    // never a half-written one. Critical because Phase 9 reads this
    // marker on every OCU startup; corruption == no rollback signal.
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        log::error("state",
                   QStringLiteral("writeUpdateState: open failed: %1")
                       .arg(f.errorString()));
        return false;
    }
    if (f.write(bytes) != bytes.size()) {
        log::error("state",
                   QStringLiteral("writeUpdateState: short write: %1")
                       .arg(f.errorString()));
        f.cancelWriting();
        return false;
    }
    if (!f.commit()) {
        log::error("state",
                   QStringLiteral("writeUpdateState: commit failed: %1")
                       .arg(f.errorString()));
        return false;
    }
    log::info("state",
              QStringLiteral("wrote marker stage=%1 current=%2 previous=%3")
                  .arg(QString::fromLatin1(updateStageToWire(in.stage)))
                  .arg(in.currentDebPath)
                  .arg(in.previousDebPath));
    return true;
}

bool clearUpdateState() {
    const QString path = updateStateFilePath();
    if (path.isEmpty()) return false;
    if (!QFileInfo::exists(path)) return true;
    QFile f(path);
    if (!f.remove()) {
        log::warn("state",
                  QStringLiteral("clearUpdateState: remove failed: %1")
                      .arg(f.errorString()));
        return false;
    }
    log::info("state", QStringLiteral("cleared marker"));
    return true;
}

}  // namespace f2c_cpp::update
