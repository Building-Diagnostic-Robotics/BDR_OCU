/**
 * @file robot_login.cpp
 * @brief Login to robot via SSH + pilot_control_auth.
 */

#include "robot_login.hpp"
#include "robot_registry.hpp"
#include "settings_constants.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>

namespace f2c_cpp {

namespace {

QString ensurePinnedKnownHostsFile(const RobotProfile& profile, QString* errorOut) {
    const QString entry = profile.known_hosts_entry.trimmed();
    if (entry.isEmpty()) {
        return QString();
    }

    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (dir.isEmpty()) {
        dir = QDir::homePath() + "/.config/PilotControl/BDRCoveragePlanner";
    }

    if (!QDir().mkpath(dir)) {
        if (errorOut) *errorOut = QString("Failed to create config directory: %1").arg(dir);
        return QString();
    }

    const QString slug = RobotRegistry::slugifyRobotId(profile.robot_id);
    const QString path = dir + "/known_hosts_" + slug;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut) *errorOut = QString("Failed to write known_hosts file: %1").arg(path);
        return QString();
    }

    QByteArray data = entry.toUtf8();
    f.write(data);
    if (!entry.endsWith('\n')) {
        f.write("\n");
    }
    f.close();

    return path;
}

}  // namespace

bool loginToRobot(const QString& robotId,
                  const QString& pin,
                  const RobotRegistry& registry,
                  QString* errorOut,
                  QString* hostOut) {
    const QString rid = robotId.trimmed();
    if (rid.isEmpty()) {
        if (errorOut) *errorOut = "Robot ID is empty.";
        return false;
    }

    auto profileOpt = registry.findById(rid);
    if (!profileOpt.has_value()) {
        if (errorOut) {
            *errorOut = "Robot ID not found in registry. Add this robot to robots.json (see app config directory).";
        }
        return false;
    }

    const RobotProfile& profile = *profileOpt;
    const QString host = profile.host.trimmed();
    const QString user = profile.ssh_user.trimmed().isEmpty() ? QStringLiteral("roofus") : profile.ssh_user.trimmed();

    if (host.isEmpty()) {
        if (errorOut) *errorOut = "Robot profile has no host.";
        return false;
    }

    QString pinnedKh = ensurePinnedKnownHostsFile(profile, errorOut);

    QProcess proc;
    proc.setProcessChannelMode(QProcess::SeparateChannels);

    QStringList args;
    args << "-o" << "ConnectTimeout=8"
         << "-o" << "BatchMode=yes";

    if (!pinnedKh.isEmpty()) {
        args << "-o" << "StrictHostKeyChecking=yes"
             << "-o" << QString("UserKnownHostsFile=%1").arg(pinnedKh)
             << "-o" << "GlobalKnownHostsFile=/dev/null";
    } else {
        args << "-o" << "StrictHostKeyChecking=no";
    }

    args << QString("%1@%2").arg(user, host)
         << "pilot_control_auth"
         << "login"
         << "--robot-id" << rid
         << "--pin-stdin"
         << "--json";

    proc.start("ssh", args);
    if (!proc.waitForStarted(3000)) {
        if (errorOut) *errorOut = "Failed to start ssh process.";
        return false;
    }

    proc.write(pin.toUtf8());
    proc.write("\n");
    proc.closeWriteChannel();

    if (!proc.waitForFinished(12000)) {
        proc.kill();
        proc.waitForFinished(3000);
        if (errorOut) *errorOut = "Login timed out.";
        return false;
    }

    const QByteArray out = proc.readAllStandardOutput();
    const QByteArray err = proc.readAllStandardError();

    if (proc.exitCode() != 0) {
        QString msg = QString::fromUtf8(err).trimmed();
        if (msg.isEmpty()) msg = QString::fromUtf8(out).trimmed();
        if (msg.isEmpty()) msg = QString("ssh exited with code %1").arg(proc.exitCode());
        if (errorOut) *errorOut = msg;
        return false;
    }

    QJsonParseError jerr{};
    QJsonDocument doc = QJsonDocument::fromJson(out, &jerr);
    if (doc.isNull() || !doc.isObject()) {
        if (errorOut) {
            *errorOut = QString("Invalid JSON from robot auth (offset %1): %2")
                            .arg(jerr.offset)
                            .arg(jerr.errorString());
        }
        return false;
    }

    const QJsonObject obj = doc.object();
    const QString token = obj.value("token").toString().trimmed();

    if (token.isEmpty()) {
        if (errorOut) *errorOut = "Robot auth response missing token.";
        return false;
    }

    if (hostOut) {
        *hostOut = host;
    }

    return true;
}

}  // namespace f2c_cpp
