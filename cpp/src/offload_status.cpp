#include "offload_status.hpp"

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

#include <limits>

namespace f2c_cpp {

namespace {

constexpr const char* kThumbsyncManifest = "thumbsync_manifest.json";

bool isSectionFolderName(const QString& name) {
    return name.startsWith(QLatin1String("Section_")) ||
           name.startsWith(QLatin1String("Mission_"));
}

OffloadCopyState stateFromString(const QString& raw) {
    const QString s = raw.trimmed().toLower();
    if (s == QLatin1String("idle") || s == QLatin1String("done") ||
        s == QLatin1String("skipped")) {
        return OffloadCopyState::Idle;
    }
    if (s == QLatin1String("queued") || s == QLatin1String("pending")) {
        return OffloadCopyState::Queued;
    }
    if (s == QLatin1String("copying")) {
        return OffloadCopyState::Copying;
    }
    if (s == QLatin1String("waiting_for_drive")) {
        return OffloadCopyState::WaitingForDrive;
    }
    if (s == QLatin1String("stick_full")) {
        return OffloadCopyState::StickFull;
    }
    if (s == QLatin1String("error") || s == QLatin1String("failed")) {
        return OffloadCopyState::Error;
    }
    return OffloadCopyState::Unknown;
}

std::optional<qint64> readByteCount(const QJsonValue& value) {
    if (!value.isDouble()) {
        return std::nullopt;
    }
    const double bytes = value.toDouble();
    if (bytes < 0.0 ||
        bytes > static_cast<double>(std::numeric_limits<qint64>::max())) {
        return std::nullopt;
    }
    return static_cast<qint64>(bytes);
}

}  // namespace

bool OffloadSnapshot::copyIncomplete() const {
    if (!parse_ok) {
        return false;
    }
    if (queued > 0) {
        return true;
    }
    switch (state) {
        case OffloadCopyState::Queued:
        case OffloadCopyState::Copying:
        case OffloadCopyState::WaitingForDrive:
        case OffloadCopyState::StickFull:
        case OffloadCopyState::Error:
            return true;
        case OffloadCopyState::Idle:
        case OffloadCopyState::Unknown:
            break;
    }
    return false;
}

OffloadSnapshot parseOffloadStatusJson(const QByteArray& json) {
    OffloadSnapshot out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return out;
    }
    const QJsonObject obj = doc.object();
    out.parse_ok = true;
    out.state = stateFromString(obj.value(QStringLiteral("state")).toString());
    const QJsonObject jobs = obj.value(QStringLiteral("jobs")).toObject();
    out.queued = obj.value(QStringLiteral("queued")).toInt(0);
    if (out.queued <= 0) {
        out.queued = jobs.size();
    }
    out.mission = obj.value(QStringLiteral("mission")).toString();
    out.error = obj.value(QStringLiteral("error")).toString();
    out.robot_free_bytes =
        readByteCount(obj.value(QStringLiteral("robot_free_bytes")));
    out.robot_floor_bytes =
        readByteCount(obj.value(QStringLiteral("robot_floor_bytes")));

    // The worker's top-level `state` is a coarse rollup: any non-current,
    // non-waiting queue is written as "error", including jobs that are
    // merely queued. Per-job state is the truth.
    bool any_job_error = false;
    bool any_waiting = false;
    bool any_copying = false;
    bool any_queued = false;
    bool any_stick_full = false;
    QString job_error;
    QString stick_full_error;
    for (auto it = jobs.begin(); it != jobs.end(); ++it) {
        const QJsonObject job = it.value().toObject();
        OffloadCopyState js =
            stateFromString(job.value(QStringLiteral("state")).toString());
        const QString je = job.value(QStringLiteral("error")).toString();
        // Unplugging a full stick moves the job to waiting_for_drive but
        // keeps the "stick full: …" error. The stick is still full, and
        // the operator is about to upload from it on the laptop.
        if (js == OffloadCopyState::WaitingForDrive &&
            je.startsWith(QLatin1String("stick full"), Qt::CaseInsensitive)) {
            js = OffloadCopyState::StickFull;
        }
        switch (js) {
            case OffloadCopyState::StickFull:
                any_stick_full = true;
                if (stick_full_error.isEmpty() && !je.isEmpty()) {
                    stick_full_error = je;
                }
                break;
            case OffloadCopyState::Error:
                any_job_error = true;
                if (job_error.isEmpty() && !je.isEmpty()) {
                    job_error = je;
                }
                break;
            case OffloadCopyState::WaitingForDrive:
                any_waiting = true;
                break;
            case OffloadCopyState::Copying:
                any_copying = true;
                break;
            case OffloadCopyState::Queued:
                any_queued = true;
                break;
            default:
                break;
        }
    }
    if (out.error.isEmpty()) {
        out.error = job_error;
    }
    if (out.state == OffloadCopyState::Error && !any_job_error) {
        if (any_waiting) {
            out.state = OffloadCopyState::WaitingForDrive;
        } else if (any_copying || !out.mission.isEmpty()) {
            out.state = OffloadCopyState::Copying;
        } else if (any_queued || out.queued > 0) {
            out.state = OffloadCopyState::Queued;
        } else {
            out.state = OffloadCopyState::Idle;
        }
        out.error.clear();
    }
    // Same precedence as the worker's rollup: a live copy wins.
    if (any_stick_full && out.state != OffloadCopyState::Copying &&
        out.state != OffloadCopyState::Error) {
        out.state = OffloadCopyState::StickFull;
        if (out.error.isEmpty()) {
            out.error = stick_full_error;
        }
    }

    const QString updated = obj.value(QStringLiteral("updated_at")).toString();
    if (!updated.isEmpty()) {
        out.updated_at = QDateTime::fromString(updated, Qt::ISODate);
        if (!out.updated_at.isValid()) {
            out.updated_at = QDateTime::fromString(updated, Qt::ISODateWithMs);
        }
    }
    return out;
}

bool thumbCopyIsReady(const ThumbCopyInputs& in) {
    if (in.stick_full) {
        return true;
    }
    // The stick in hand is the copy record. A saved robot report must
    // not block an office upload of a stick the robot already finished.
    if (!in.robot_reachable && in.stick == StickSync::Complete) {
        return true;
    }
    if (in.robot_copy_incomplete) {
        return false;
    }
    return in.stick != StickSync::Incomplete;
}

QString thumbCopyStaleReportNote(const ThumbCopyInputs& in) {
    if (in.stick_full || in.robot_reachable || !in.robot_copy_incomplete) {
        return {};
    }
    if (in.stick != StickSync::Complete) {
        return {};
    }
    return QStringLiteral(
        "The robot's last report said a copy had not finished. "
        "This stick is complete, so it can be uploaded.");
}

int scansAtRiskOfDeletion(const OffloadSnapshot& snap) {
    if (!snap.parse_ok || snap.queued <= 0 || !snap.robot_free_bytes ||
        !snap.robot_floor_bytes) {
        return 0;
    }
    const qint64 free = *snap.robot_free_bytes;
    const qint64 floor = *snap.robot_floor_bytes;
    if (floor <= 0) {
        return 0;
    }
    // Under twice the floor. free >= floor && free - floor >= floor is
    // free >= 2 * floor without overflowing.
    if (free >= floor && free - floor >= floor) {
        return 0;
    }
    return snap.queued;
}

StickSync classifyStickSync(const QString& data_root) {
    const QDir root(data_root);
    if (data_root.isEmpty() || !root.exists()) {
        return StickSync::Absent;
    }
    const QDir::Filters dir_filters =
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks;
    int missions = 0;
    int missing = 0;
    int sections = 0;
    for (const QString& date : root.entryList(dir_filters, QDir::Name)) {
        if (date.startsWith(QLatin1Char('.'))) {
            continue;
        }
        const QDir date_dir(root.filePath(date));
        for (const QString& building : date_dir.entryList(dir_filters, QDir::Name)) {
            const QDir building_dir(date_dir.filePath(building));
            for (const QString& name :
                 building_dir.entryList(dir_filters, QDir::Name)) {
                if (!isSectionFolderName(name)) {
                    continue;
                }
                if (name.startsWith(QLatin1String("Section_"))) {
                    ++sections;
                    continue;
                }
                ++missions;
                const QString manifest =
                    building_dir.filePath(name) + QLatin1Char('/') +
                    QLatin1String(kThumbsyncManifest);
                if (!QFileInfo::exists(manifest)) {
                    ++missing;
                }
            }
        }
    }
    if (missions == 0) {
        return sections > 0 ? StickSync::Incomplete : StickSync::Empty;
    }
    return missing > 0 ? StickSync::Incomplete : StickSync::Complete;
}

}  // namespace f2c_cpp
