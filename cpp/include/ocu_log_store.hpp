/**
 * @file ocu_log_store.hpp
 * @brief The laptop's mission logs, and getting them into the upload.
 *
 * Each Stage 6 mission writes `mission_<yyyyMMdd_hhmmss>.log` (the OCU log,
 * which also carries the robot launch's console output) and, when it
 * closes, `<id>.laptop_ros_logs.tar.gz` (the laptop's own ROS logs for
 * that window, packed by scripts/ros_log_bundle.py). Both live in
 * AppDataLocation/mission_logs/.
 *
 * Exact match, no clocks: the OCU pushes its log id to the robot's
 * data_collection_coordinator ("ocu_log_id"), which lists it in the
 * mission's mission_config.json "ocu_logs". At upload, attachToMission()
 * copies exactly those files into <Mission>/logs/ocu/ on the stick, which
 * uploader.py uploads although the robot's copy record does not list them.
 *
 * Retention is by free disk space only, like the robot's data: when the
 * volume drops below the floor, the oldest logs whose mission has uploaded
 * go first, then (logged) the oldest that have not.
 *
 * Qt Core only; unit-tested (tests/ocu_log_store_tests.cpp).
 */
#pragma once

#include <QString>
#include <QStringList>

#include <functional>

namespace f2c_cpp::ocu_logs {

/// Laptop free space kept by pruning (robot keeps 20 GB on its own disk).
constexpr qint64 kFreeFloorBytes = 10LL * 1024 * 1024 * 1024;

/// Suffixes of the files that make up one log id's group in the store.
inline constexpr const char* kLogSuffix = ".log";
inline constexpr const char* kLaptopRosSuffix = ".laptop_ros_logs.tar.gz";
inline constexpr const char* kUploadedSuffix = ".uploaded";

/// Folder under a mission on the stick, matched by uploader.py OCU_LOG_PREFIX.
inline constexpr const char* kMissionSubdir = "logs/ocu";

/// AppDataLocation/mission_logs (created if missing). Empty on failure.
QString storeDir();

/// Same rule as ros_log_bundle.sanitize_log_id: '' unless a plain name.
QString sanitizeLogId(const QString& id);

/// Valid ids listed in <mission_dir>/mission_config.json "ocu_logs".
QStringList idsForMission(const QString& mission_dir);

/// The uploader already finished this folder (its manifest.json exists):
/// adding files now would only force a re-upload, so nothing is attached.
bool missionAlreadyUploaded(const QString& mission_dir);

struct AttachResult {
    QStringList ids;        ///< ids the mission lists
    QStringList copied;     ///< file names copied to the stick
    QStringList present;    ///< already there, same size
    QStringList missing;    ///< listed but not in this laptop's store
    QString error;          ///< first copy error, empty if none
};

/// Copy each listed id's .log and .laptop_ros_logs.tar.gz into
/// <mission_dir>/logs/ocu/ (via ".part" + rename, so a pulled stick never
/// leaves a half file that uploads). Safe to repeat.
AttachResult attachToMission(const QString& mission_dir, const QString& store_dir);

/// After a successful upload of mission_dir: mark uploaded (eligible for
/// pruning first) each id whose .log went up with it, i.e. is in
/// <mission_dir>/logs/ocu/ at the store's size. Returns the ids marked.
QStringList markUploaded(const QString& mission_dir, const QString& store_dir);

struct PruneResult {
    QStringList deleted_uploaded;
    QStringList deleted_not_uploaded;  ///< data loss; caller logs it loudly
    bool floor_reached = true;
};

/// Delete whole id groups, oldest first (uploaded before not-uploaded),
/// until free_bytes() >= floor. `keep_id` (the log being written) is never
/// deleted.
PruneResult prune(const QString& store_dir, qint64 floor,
                  const std::function<qint64()>& free_bytes,
                  const QString& keep_id = QString());

/// Free bytes on the volume holding `path` (QStorageInfo); -1 if unknown.
qint64 freeBytesAt(const QString& path);

}  // namespace f2c_cpp::ocu_logs
