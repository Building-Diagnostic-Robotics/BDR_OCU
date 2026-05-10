/**
 * @file robot_registry.cpp
 * @brief Implementation of RobotRegistry
 */

#include "robot_registry.hpp"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QDir>
#include <QRegularExpression>
#include <QSettings>

#include "settings_constants.hpp"

namespace f2c_cpp {

static std::optional<QJsonDocument> readJsonFile(const QString& path, QString& error) {
    QFile f(path);
    if (!f.exists()) {
        error = QString("File not found: %1").arg(path);
        return std::nullopt;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        error = QString("Cannot open: %1 (%2)").arg(path, f.errorString());
        return std::nullopt;
    }
    QByteArray data = f.readAll();
    f.close();

    QJsonParseError parse_err{};
    QJsonDocument doc = QJsonDocument::fromJson(data, &parse_err);
    if (doc.isNull()) {
        error = QString("JSON parse error at offset %1: %2")
                    .arg(parse_err.offset)
                    .arg(parse_err.errorString());
        return std::nullopt;
    }
    return doc;
}

std::optional<RobotProfile> RobotProfile::fromJson(const QJsonObject& obj, QString& error) {
    RobotProfile p;

    p.robot_id = obj.value("robot_id").toString().trimmed();
    if (p.robot_id.isEmpty()) {
        error = "Missing required field: robot_id";
        return std::nullopt;
    }

    p.host = obj.value("host").toString().trimmed();
    if (p.host.isEmpty()) {
        error = QString("Robot %1: missing required field: host").arg(p.robot_id);
        return std::nullopt;
    }

    QString ssh_user = obj.value("ssh_user").toString().trimmed();
    if (!ssh_user.isEmpty()) {
        p.ssh_user = ssh_user;
    }
    QString data_path = obj.value("robot_data_path").toString().trimmed();
    if (!data_path.isEmpty()) {
        p.robot_data_path = data_path;
    }
    p.radio_ip = obj.value("radio_ip").toString().trimmed();
    p.snmp_ro_community = obj.value("snmp_ro_community").toString().trimmed();
    p.snmp_rssi_oid = obj.value("snmp_rssi_oid").toString().trimmed();
    p.snmp_snr_oid = obj.value("snmp_snr_oid").toString().trimmed();

    p.known_hosts_entry = obj.value("known_hosts_entry").toString().trimmed();
    p.host_key_fingerprint = obj.value("host_key_fingerprint").toString().trimmed();
    p.default_remote_upload_dir = obj.value("default_remote_upload_dir").toString().trimmed();

    QString slug = obj.value("robot_id_slug").toString().trimmed();
    if (slug.isEmpty()) {
        slug = RobotRegistry::slugifyRobotId(p.robot_id);
    }
    p.robot_id_slug = slug;

    return p;
}

QString RobotRegistry::slugifyRobotId(const QString& robot_id) {
    QString s = robot_id.trimmed().toLower();
    static QRegularExpression nonAlnum(R"([^a-z0-9]+)");
    s.replace(nonAlnum, "-");
    while (s.startsWith('-')) s.remove(0, 1);
    while (s.endsWith('-')) s.chop(1);
    if (s.isEmpty()) {
        return "robot";
    }
    return s;
}

QString RobotRegistry::defaultUserRegistryPath() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (dir.isEmpty()) {
        dir = QDir::homePath() + "/.config/PilotControl/BDRCoveragePlanner";
    }
    QDir().mkpath(dir);
    return dir + "/robots.json";
}

QStringList RobotRegistry::robotIds() const {
    QStringList out;
    out.reserve(robots_.size());
    for (const auto& r : robots_) {
        out << r.robot_id;
    }
    return out;
}

std::optional<RobotProfile> RobotRegistry::findById(const QString& robot_id) const {
    QString key = robot_id.trimmed();
    for (const auto& r : robots_) {
        if (r.robot_id == key) {
            return r;
        }
    }
    return std::nullopt;
}

bool RobotRegistry::load(QString* error) {
    QStringList candidates;
    candidates << defaultUserRegistryPath();

    const QString exe_dir = QCoreApplication::applicationDirPath();
    if (!exe_dir.isEmpty()) {
        candidates << (exe_dir + "/robots.json");
        candidates << (exe_dir + "/../config/robots.json");
        candidates << (exe_dir + "/../share/bdr_coverage_planner/robots.json");
        candidates << (exe_dir + "/../share/bdr_coverage_planner/config/robots.json");
    }

    QString last_err;
    for (const QString& path : candidates) {
        if (!QFileInfo::exists(path)) {
            continue;
        }
        if (loadFromFile(path, &last_err)) {
            return true;
        }
    }

    if (error) {
        *error = last_err.isEmpty()
            ? QString("Robot registry not found. Looked in:\n- %1").arg(candidates.join("\n- "))
            : last_err;
    }
    loaded_ = false;
    return false;
}

bool RobotRegistry::loadFromFile(const QString& path, QString* error) {
    QString err;
    auto doc_opt = readJsonFile(path, err);
    if (!doc_opt.has_value()) {
        if (error) *error = err;
        loaded_ = false;
        return false;
    }

    QJsonDocument doc = *doc_opt;
    QJsonArray arr;

    if (doc.isArray()) {
        arr = doc.array();
    } else if (doc.isObject()) {
        QJsonObject root = doc.object();
        if (root.contains("robots") && root.value("robots").isArray()) {
            arr = root.value("robots").toArray();
        } else {
            if (error) *error = "Robot registry JSON must be an array or an object with a 'robots' array.";
            loaded_ = false;
            return false;
        }
    } else {
        if (error) *error = "Robot registry JSON must be an array or object.";
        loaded_ = false;
        return false;
    }

    QList<RobotProfile> parsed;
    parsed.reserve(arr.size());
    for (int i = 0; i < arr.size(); ++i) {
        if (!arr[i].isObject()) {
            if (error) *error = QString("robots[%1] is not an object").arg(i);
            loaded_ = false;
            return false;
        }
        QString perr;
        auto rp = RobotProfile::fromJson(arr[i].toObject(), perr);
        if (!rp.has_value()) {
            if (error) *error = QString("robots[%1]: %2").arg(i).arg(perr);
            loaded_ = false;
            return false;
        }
        parsed.push_back(*rp);
    }

    robots_ = parsed;
    source_path_ = path;
    loaded_ = true;
    return true;
}

bool resolveRobotSshTargetFromSettings(ResolvedRobotSshTarget* out, QString* error_out) {
    if (!out) {
        return false;
    }
    *out = ResolvedRobotSshTarget{};

    QSettings settings(kSettingsOrgName, kSettingsAppName);
    const QString override_ip =
        settings.value(QStringLiteral("robot_ip"), QString()).toString().trimmed();
    if (!override_ip.isEmpty()) {
        out->host = override_ip;
        out->ssh_user = QStringLiteral("roofus");
        return true;
    }

    const QString robot_id =
        settings.value(QStringLiteral("setup/robot_id"), QString()).toString().trimmed();
    if (robot_id.isEmpty()) {
        if (error_out) {
            *error_out = QStringLiteral(
                "No robot is logged in. Complete setup login, or set the robot_ip "
                "setting for development overrides.");
        }
        return false;
    }

    RobotRegistry registry;
    QString load_err;
    if (!registry.load(&load_err)) {
        if (error_out) {
            *error_out = load_err.isEmpty()
                ? QStringLiteral("Robot registry could not be loaded.")
                : load_err;
        }
        return false;
    }

    const auto profile = registry.findById(robot_id);
    if (!profile.has_value()) {
        if (error_out) {
            *error_out =
                QStringLiteral("Robot \"%1\" not found in registry (%2).")
                    .arg(robot_id, registry.sourcePath());
        }
        return false;
    }

    out->host = profile->host.trimmed();
    out->ssh_user = profile->ssh_user.trimmed();
    if (out->ssh_user.isEmpty()) {
        out->ssh_user = QStringLiteral("roofus");
    }
    if (out->host.isEmpty()) {
        if (error_out) {
            *error_out =
                QStringLiteral("Registry entry for \"%1\" has an empty host.").arg(robot_id);
        }
        return false;
    }
    return true;
}

}  // namespace f2c_cpp
