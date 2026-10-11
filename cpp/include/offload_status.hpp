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
    /// The stick is mounted but under its free-space floor, so the
    /// copy did not start. The copy is incomplete, and the Upload card
    /// stays enabled: uploading is what frees the stick.
    StickFull,
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

/**
 * What the Upload card knows when it decides whether a copy is still
 * owed. `robot_copy_incomplete` is the live probe when the robot
 * answered, and the saved report when it did not.
 */
struct ThumbCopyInputs {
    bool robot_reachable = false;
    bool robot_copy_incomplete = false;
    bool stick_full = false;
    StickSync stick = StickSync::Absent;
};

/**
 * Upload is allowed when the stick is under its free-space floor (that
 * is how the operator frees it), and when the robot cannot be reached
 * but the plugged-in stick is complete. A reachable robot that is still
 * copying, and a stick that is incomplete or not plugged in, block.
 */
bool thumbCopyIsReady(const ThumbCopyInputs& in);

/**
 * Tooltip for the one case upload is allowed despite a saved unfinished
 * report: the robot is unreachable and this stick is complete. Empty
 * otherwise.
 */
QString thumbCopyStaleReportNote(const ThumbCopyInputs& in);

}  // namespace f2c_cpp
