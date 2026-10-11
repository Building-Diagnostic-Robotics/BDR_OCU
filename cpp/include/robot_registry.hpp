/**
 * @file robot_registry.hpp
 * @brief Robot registry: map robot_id -> connection/profile details
 *
 * Used by the Coverage Planner to select a robot by ID without exposing IPs in the UI.
 */

#pragma once

#include <QString>
#include <QStringList>
#include <QList>
#include <QJsonObject>

class QSettings;

#include <optional>

namespace f2c_cpp {

struct RobotProfile {
    QString robot_id;                  // e.g., "Roofus#001"
    QString robot_id_slug;             // e.g., "roofus-001" (derived)
    QString host;                      // static IP/hostname (hidden from UI)
    QString ssh_user = "roofus";
    QString robot_data_path = "/R_DATA";
    QString radio_ip;                  // e.g., "192.168.168.1" (radio management IP)
    QString snmp_ro_community;         // e.g., "public" (read-only SNMP community)
    QString snmp_rssi_oid;             // e.g., ".1.3.6.1.4.1...." (RSSI OID)
    QString snmp_snr_oid;              // optional SNR/link-quality OID

    // Optional SSH pinning material (used for StrictHostKeyChecking=yes)
    QString known_hosts_entry;         // e.g., "192.168.168.101 ssh-ed25519 AAAA..."
    QString host_key_fingerprint;      // e.g., "SHA256:...."

    // Optional mission file upload destination on robot
    QString default_remote_upload_dir; // e.g., "/R_DATA/waypoints"

    // Cloud upload credentials (presigned-URL backend). Persisted in
    // robots.json per-robot so each laptop's deploy carries its own
    // x-client-id / x-device-token without an extra config dialog.
    // The backend differentiates customers by client_id and individual
    // robots by device_token; both headers are required on every
    // /presign and /complete call. See AGENTS.md "Upload pipeline" for
    // the contract and `cpp/scripts/uploader.py` for the runtime
    // consumer (run on the laptop against the RDATA_EXT stick).
    QString cloud_client_id;        // e.g. "sig_roofing_ID"
    QString cloud_device_token;     // e.g. "roofus#0001"

    static std::optional<RobotProfile> fromJson(const QJsonObject& obj, QString& error);
};

class RobotRegistry {
public:
    RobotRegistry() = default;

    bool load(QString* error = nullptr);
    bool loadFromFile(const QString& path, QString* error = nullptr);

    bool isLoaded() const { return loaded_; }
    bool isEmpty() const { return robots_.isEmpty(); }

    QList<RobotProfile> robots() const { return robots_; }
    QStringList robotIds() const;
    std::optional<RobotProfile> findById(const QString& robot_id) const;

    /**
     * Cloud upload backend base URL (API Gateway invoke URL minted by the
     * BDR backend Lambda team). Read from one of:
     *  1. The optional top-level "cloud_api_base" key in robots.json.
     *  2. Sibling `cloud_config.json` (`{"cloud_api_base": "..."}`) — same
     *     search path as robots.json.
     *  3. The compiled-in fallback in `kDefaultCloudApiBase`.
     *
     * Per the design lock-in this is **global** (shared across robots /
     * customers); per-robot differentiation lives in
     * `RobotProfile::cloud_client_id` + `cloud_device_token`.
     */
    QString cloudApiBase() const { return cloud_api_base_; }

    /**
     * The robot this laptop operates: the optional top-level
     * "paired_robot_id" when set, else the only robot in the file.
     * Empty when several robots are listed and none is named (the caller
     * offers a picker). Also empty, with `error` set, when
     * "paired_robot_id" names a robot the file does not list — guessing
     * there would point every SSH call at the wrong robot.
     */
    QString pairedRobotId(QString* error = nullptr) const;

    QString sourcePath() const { return source_path_; }

    static QString slugifyRobotId(const QString& robot_id);
    static QString defaultUserRegistryPath();

private:
    QList<RobotProfile> robots_;
    QString source_path_;
    QString cloud_api_base_;
    QString paired_robot_id_;
    bool loaded_ = false;
};

/** SSH target for operator-initiated remote commands (preflight, exploration, tilt cal). */
struct ResolvedRobotSshTarget {
    QString host;
    QString ssh_user;
};

/**
 * Resolve robot SSH host/user from QSettings + robots.json.
 * A dev build honours a non-empty `robot_ip` ahead of `setup/robot_id`.
 * A Release build ignores that key. On success, `ssh_user` is never empty
 * (defaults to "roofus").
 */
bool resolveRobotSshTargetFromSettings(ResolvedRobotSshTarget* out, QString* error_out = nullptr);

/**
 * The decision `resolveRobotSshTargetFromSettings` applies.
 * `stored_override_ip` is used only when `allow_dev_override` is true, so a
 * Release build cannot be pointed at a leftover address.
 */
bool resolveRobotSshTarget(const RobotRegistry& registry,
                           const QString& robot_id,
                           const QString& stored_override_ip,
                           bool allow_dev_override,
                           ResolvedRobotSshTarget* out,
                           QString* error_out = nullptr);

/** Values removed from the [General] group. Empty means the key was absent. */
struct RetiredGeneralRobotKeys {
    QString robot_ip;
    QString robot_id;
};

/**
 * A Release build deletes leftover [General] `robot_ip` and `robot_id`.
 * `setup/robot_id` and any grouped `robot_id` (planner sessions) stay.
 * `settings` must not be inside a group. A dev build (`release_build` false)
 * changes nothing.
 */
RetiredGeneralRobotKeys retireLegacyGeneralRobotKeys(QSettings& settings,
                                                     bool release_build);

}  // namespace f2c_cpp
