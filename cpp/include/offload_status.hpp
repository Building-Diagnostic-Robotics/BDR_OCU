/**
 * @file offload_status.hpp
 * @brief Parse the robot offload worker's status.json and classify a
 *        laptop RDATA_EXT tree by thumbsync_manifest.json.
 *
 * The worker on cliff-on-autonomy writes /R_DATA/.offload/status.json
 * atomically and documents it as SSH-pollable by the OCU. A destination
 * folder is copy-complete only when thumbsync_manifest.json exists.
 */
#pragma once

#include <QDateTime>
#include <QString>

class QByteArray;

namespace f2c_cpp {

enum class OffloadCopyState {
    Unknown = 0,
    Idle,
    Queued,
    Copying,
    WaitingForDrive,
    Error,
};

struct OffloadSnapshot {
    OffloadCopyState state = OffloadCopyState::Unknown;
    int queued = 0;
    QString mission;
    QDateTime updated_at;
    /// First per-job `error` (the worker has no top-level error field).
    QString error;
    bool parse_ok = false;

    /// True while any job is unfinished. Idle + queued==0 is done.
    bool copyIncomplete() const;
};

OffloadSnapshot parseOffloadStatusJson(const QByteArray& json);

enum class StickSync {
    Absent = 0,     ///< Mount path missing / not a directory.
    Empty,          ///< Mounted, no Section_* / Mission_* folders.
    Incomplete,     ///< A Mission_* folder is missing thumbsync_manifest.json,
                    ///  or only orphan Section_* folders exist.
    Complete,       ///< Every Mission_* folder has thumbsync_manifest.json.
};

/// Depth-3 walk. Copy-complete is per Mission_* — the robot writes
/// thumbsync_manifest.json only there after the mission and its sections
/// land on the stick. Section_* siblings are not checked.
StickSync classifyStickSync(const QString& data_root);

}  // namespace f2c_cpp
