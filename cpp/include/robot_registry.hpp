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

    QString sourcePath() const { return source_path_; }

    static QString slugifyRobotId(const QString& robot_id);
    static QString defaultUserRegistryPath();

private:
    QList<RobotProfile> robots_;
    QString source_path_;
    bool loaded_ = false;
};

}  // namespace f2c_cpp
