/**
 * @file scan_session_tracker.hpp
 * @brief Tracks scan sessions: GPS accumulation, stats persistence, metadata
 * 
 * Features:
 * - Subscribes to /gps/fix to accumulate GPS coordinates during scan
 * - Saves CoverageStats + execution data per section to JSON
 * - Links scan sessions to downloaded data sections
 * - Provides metadata for cloud upload
 */

#pragma once

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QDateTime>
#include <QMap>
#include <QMutex>

#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/nav_sat_fix.hpp>

#include "cloud_upload_manager.hpp"  // For ScanMetadata

#include "coverage_stats.hpp"

namespace f2c_cpp {

/**
 * @brief GPS accumulator that averages fixes during a scan session
 */
struct GpsAccumulator {
    double sumLat = 0.0;
    double sumLon = 0.0;
    double sumAlt = 0.0;
    int count = 0;
    
    void addFix(double lat, double lon, double alt) {
        sumLat += lat;
        sumLon += lon;
        sumAlt += alt;
        count++;
    }
    
    double avgLat() const { return count > 0 ? sumLat / count : 0.0; }
    double avgLon() const { return count > 0 ? sumLon / count : 0.0; }
    double avgAlt() const { return count > 0 ? sumAlt / count : 0.0; }
    bool hasData() const { return count > 0; }
    
    void reset() {
        sumLat = sumLon = sumAlt = 0.0;
        count = 0;
    }
};

/**
 * @brief Saved scan session record
 */
struct ScanSession {
    QString sessionId;          // Unique ID (timestamp-based)
    QString sectionName;        // Expected section name on robot
    QDateTime startTime;
    QDateTime endTime;
    
    // Scan stats (from CoverageStats at time of execution)
    double pathLengthM = 0.0;
    double coverageAreaM2 = 0.0;
    double fieldAreaM2 = 0.0;
    double coveragePercent = 0.0;
    int numSwaths = 0;
    int numTurns = 0;
    int numWaypoints = 0;
    double swathWidthM = 0.0;
    double overlapPercent = 0.0;
    QString patternType;
    
    // Execution data
    double actualDurationSec = 0.0;
    double avgSpeedMps = 0.0;
    double totalTraveledM = 0.0;
    
    // GPS
    GpsAccumulator gps;
    
    QJsonObject toJson() const;
    static ScanSession fromJson(const QJsonObject& obj);
};

/**
 * @brief Tracks scan sessions, accumulates GPS, persists scan metadata
 * 
 * Usage:
 * 1. Call startSession() when a scan begins
 * 2. GPS fixes are accumulated automatically via ROS2 subscription
 * 3. Call endSession() with final stats when scan completes
 * 4. Sessions are persisted to JSON files
 * 5. getMetadataForSection() retrieves metadata for cloud upload
 */
class ScanSessionTracker : public QObject {
    Q_OBJECT

public:
    explicit ScanSessionTracker(rclcpp::Node::SharedPtr node, QObject* parent = nullptr);
    ~ScanSessionTracker() override;
    
    // Session lifecycle
    void startSession(const QString& sectionName, const CoverageStats& plannedStats,
                      double swathWidth, const QString& patternType);
    void endSession(double actualDurationSec, double avgSpeedMps, double totalTraveledM);
    bool isSessionActive() const { return sessionActive_; }
    
    // Get metadata for a section (for cloud upload)
    ScanMetadata getMetadataForSection(const QString& sectionName, 
                                        const QString& dateFolder,
                                        const QString& localPath) const;
    
    // Get all saved sessions
    QList<ScanSession> savedSessions() const;
    
    // Find session matching a section name (approximate)
    std::optional<ScanSession> findSession(const QString& sectionName) const;
    
    // GPS access
    GpsAccumulator currentGps() const;
    
signals:
    void sessionStarted(const QString& sectionName);
    void sessionEnded(const QString& sectionName);
    void gpsFixReceived(double lat, double lon, double alt);

private:
    void onGpsFix(const sensor_msgs::msg::NavSatFix::SharedPtr msg);
    void saveSession(const ScanSession& session);
    void loadSessions();
    QString sessionsFilePath() const;
    
    rclcpp::Node::SharedPtr node_;
    rclcpp::Subscription<sensor_msgs::msg::NavSatFix>::SharedPtr gps_sub_;
    
    bool sessionActive_ = false;
    ScanSession currentSession_;
    GpsAccumulator gpsAccumulator_;
    
    QMap<QString, ScanSession> savedSessions_;  // sessionId -> session
    mutable QMutex mutex_;
};

} // namespace f2c_cpp
