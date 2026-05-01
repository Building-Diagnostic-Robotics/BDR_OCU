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

namespace f2c_cpp {

namespace {

QString canonicalizeRobotId(const QString& robot_id) {
    QString s = robot_id.trimmed().toLower();
    static QRegularExpression non_alnum(R"([^a-z0-9]+)");
    s.remove(non_alnum);
    return s;
}

void appendCandidate(QStringList& candidates, const QString& candidate) {
    if (candidate.isEmpty()) return;
    const QString clean = QDir::cleanPath(candidate);
    if (!candidates.contains(clean)) {
        candidates << clean;
    }
}

void appendRegistryCandidatesForBase(QStringList& candidates, const QString& base_dir) {
    if (base_dir.isEmpty()) return;

    QDir base(base_dir);
    appendCandidate(candidates, base.absoluteFilePath("robots.json"));
    appendCandidate(candidates, base.absoluteFilePath("config/robots.json"));

    const QStringList share_dirs = {
        "bdr_coverage_planner",
        "bdr-coverage-planner",
        "pilot_control",
    };
    for (const QString& share_dir : share_dirs) {
        appendCandidate(candidates, base.absoluteFilePath(QString("share/%1/robots.json").arg(share_dir)));
        appendCandidate(candidates, base.absoluteFilePath(QString("share/%1/config/robots.json").arg(share_dir)));
    }

    // Development-tree fallbacks for running from a ROS workspace without installing assets.
    appendCandidate(candidates, base.absoluteFilePath("src/pilot_control/scripts/F2C/cpp/config/robots.json"));
    appendCandidate(candidates, base.absoluteFilePath("src/pilot_control/config/robots.json"));
}

}  // namespace

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

    // Optional fields
    QString ssh_user = obj.value("ssh_user").toString().trimmed();
    if (!ssh_user.isEmpty()) {
        p.ssh_user = ssh_user;
    }
    QString data_path = obj.value("robot_data_path").toString().trimmed();
    if (!data_path.isEmpty()) {
        p.robot_data_path = data_path;
    }

    p.known_hosts_entry = obj.value("known_hosts_entry").toString().trimmed();
    p.host_key_fingerprint = obj.value("host_key_fingerprint").toString().trimmed();
    p.default_remote_upload_dir = obj.value("default_remote_upload_dir").toString().trimmed();

    // Derive slug if not provided
    QString slug = obj.value("robot_id_slug").toString().trimmed();
    if (slug.isEmpty()) {
        slug = RobotRegistry::slugifyRobotId(p.robot_id);
    }
    p.robot_id_slug = slug;

    return p;
}

QString RobotRegistry::slugifyRobotId(const QString& robot_id) {
    QString s = robot_id.trimmed().toLower();

    // Replace any run of non [a-z0-9] with "-"
    static QRegularExpression nonAlnum(R"([^a-z0-9]+)");
    s.replace(nonAlnum, "-");

    // Trim '-' from ends
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
        // Fallback to ~/.config if Qt can't resolve
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
    if (key.isEmpty()) {
        return std::nullopt;
    }

    for (const auto& r : robots_) {
        if (r.robot_id == key) {
            return r;
        }
    }

    const QString folded = key.toLower();
    for (const auto& r : robots_) {
        if (r.robot_id.trimmed().toLower() == folded) {
            return r;
        }
    }

    const QString canonical = canonicalizeRobotId(key);
    for (const auto& r : robots_) {
        if (canonicalizeRobotId(r.robot_id) == canonical ||
            canonicalizeRobotId(r.robot_id_slug) == canonical) {
            return r;
        }
    }

    return std::nullopt;
}

bool RobotRegistry::load(QString* error) {
    // Search order:
    // 1) User config location (~/.config/PilotControl/BDRCoveragePlanner/robots.json)
    // 2) Relative to executable/install roots (standalone install, deb, ROS workspace)
    // 3) Relative to current working directory (developer runs from a workspace)
    QStringList candidates;
    appendCandidate(candidates, defaultUserRegistryPath());

    QStringList roots;
    auto appendRoot = [&roots](const QString& path) {
        if (path.isEmpty()) return;
        const QString clean = QDir(path).absolutePath();
        if (!roots.contains(clean)) {
            roots << clean;
        }
    };

    const QString exe_dir = QCoreApplication::applicationDirPath();
    appendRoot(exe_dir);
    appendRoot(QDir(exe_dir).absoluteFilePath(".."));
    appendRoot(QDir(exe_dir).absoluteFilePath("../.."));

    const QString cwd = QDir::currentPath();
    appendRoot(cwd);
    appendRoot(QDir(cwd).absoluteFilePath(".."));

    for (const QString& root : roots) {
        appendRegistryCandidatesForBase(candidates, root);
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
        if (!last_err.isEmpty()) {
            *error = last_err;
        } else {
            *error = QString("Robot registry not found. Looked in:\n- %1")
                         .arg(candidates.join("\n- "));
        }
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

}  // namespace f2c_cpp

