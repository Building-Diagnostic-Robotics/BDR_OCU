/**
 * @file update_state.hpp
 * @brief OTA marker file — the bridge between Phase 8 (writer) and
 *        Phase 9 (reader / rollback watchdog).
 *
 * Lives in bdr_update_core so both the OCU and bdr-update-runner can
 * read+write the same on-disk schema.
 *
 * Single JSON file at <CacheLocation>/update_state.json with shape:
 *
 *   {
 *     "schema": 1,
 *     "state": "dpkg_running" | "probing_health" | ...,
 *     "current_deb_path": "/home/.../updates/x.deb",
 *     "previous_deb_path": "/home/.../updates/y.deb"
 *   }
 *
 * Phase 8 writes this at:
 *   - DownloadComplete  → dpkg_running   (just before invoking dpkg)
 *   - dpkg success      → probing_health (just before execv launcher)
 *
 * Phase 9 will read this on OCU startup:
 *   - state == dpkg_running   → invoke `bdr-apply-update recover` to
 *                                run dpkg --configure -a, then clear.
 *   - state == probing_health → start a health-probe timer; if OCU
 *                                doesn't reach `done` within N seconds,
 *                                rollback by re-installing previous_deb.
 *   - state == done | absent  → no-op; normal startup.
 *
 * Atomicity: write is atomic via QSaveFile (writes a tmp file, fsyncs,
 * then renames over). A power loss mid-write leaves either the old file
 * intact or no file at all — never a partially-written file.
 */

#pragma once

#include <QString>
#include <QtGlobal>

namespace f2c_cpp::update {

/// Schema version stamped into the marker file. Bump whenever the JSON
/// shape changes in a non-backward-compatible way; readers should
/// gracefully ignore markers with an unknown schema.
constexpr int kUpdateStateSchemaVersion = 1;

enum class UpdateStage {
    None,                    ///< no marker on disk, or schema unknown
    Downloading,             ///< reserved for future use; not written today
    DpkgRunning,             ///< dpkg invocation in flight
    InstalledPendingProbe,   ///< runner finished dpkg; OCU has not yet
                             ///< started the health probe. The OCU
                             ///< rewrites this to ProbingHealth on first
                             ///< sight so a subsequent ctor-crash launch
                             ///< sees ProbingHealth (= "we already tried")
                             ///< and triggers rollback. Without this
                             ///< two-state separation, the legitimate
                             ///< first-launch-after-install would be
                             ///< indistinguishable from a crash-relaunch.
    ProbingHealth,            ///< health probe in flight in this process
    Done,                    ///< new OCU passed health probe; steady state
    RolledBack,              ///< Phase 9 reverted to previous_deb_path;
                             ///< banner shown until operator dismisses
};

struct UpdateStateData {
    UpdateStage stage = UpdateStage::None;
    QString currentDebPath;
    QString previousDebPath;
};

/**
 * @brief Absolute path of the marker file. Resolves to
 *        <QStandardPaths::CacheLocation>/update_state.json. Empty if
 *        cache dir is unwritable.
 *
 * Both binaries call this via the same Qt org/app names (set in
 * each main.cpp), so the path matches across the OCU↔runner handoff.
 */
QString updateStateFilePath();

/**
 * @brief Read and parse the marker file.
 * @param out  populated on success.
 * @return true if a well-formed marker was read; false otherwise
 *         (file missing, JSON malformed, schema mismatch). On false
 *         `out` is set to a default-constructed UpdateStateData so
 *         callers can treat "no marker" and "unreadable marker" the
 *         same way (both → no-op recovery).
 */
bool readUpdateState(UpdateStateData* out);

/**
 * @brief Atomically write the marker file. Uses QSaveFile so a power
 *        loss mid-write cannot corrupt the existing file.
 * @return true on success, false on filesystem error (logged).
 */
bool writeUpdateState(const UpdateStateData& in);

/**
 * @brief Remove the marker file. No-op if it doesn't exist. Logged.
 *        Phase 9 calls this after a successful recover or after the
 *        health probe passes.
 * @return true on success or if file was already absent.
 */
bool clearUpdateState();

/// String form of an UpdateStage, used in the JSON file. Stable; do not
/// rename without bumping the schema version.
const char* updateStageToWire(UpdateStage stage);
UpdateStage updateStageFromWire(const QString& wire);

}  // namespace f2c_cpp::update
