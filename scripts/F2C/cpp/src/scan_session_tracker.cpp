/**
 * @file scan_session_tracker.cpp
 * @brief Implementation of ScanSessionTracker
 */

#include "scan_session_tracker.hpp"
#include "coverage_gui.hpp"  // For CoverageStats

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QStandardPaths>
#include <QDebug>

namespace f2c_cpp {

// =============================================================================
// ScanSession JSON Serialization
// =============================================================================

QJsonObject ScanSession::toJson() const {
    QJsonObject obj;
    obj["session_id"] = sessionId;
    obj["section_name"] = sectionName;
    obj["robot_id"] = robotId;
    obj["robot_id_slug"] = robotIdSlug;
    obj["start_time"] = startTime.toString(Qt::ISODate);
    obj["end_time"] = endTime.toString(Qt::ISODate);
    
    // Stats
    obj["path_length_m"] = pathLengthM;
    obj["coverage_area_m2"] = coverageAreaM2;
    obj["field_area_m2"] = fieldAreaM2;
    obj["coverage_percent"] = coveragePercent;
    obj["num_swaths"] = numSwaths;
    obj["num_turns"] = numTurns;
    obj["num_waypoints"] = numWaypoints;
    obj["swath_width_m"] = swathWidthM;
    obj["overlap_percent"] = overlapPercent;
    obj["pattern_type"] = patternType;
    
    // Execution
    obj["actual_duration_sec"] = actualDurationSec;
    obj["avg_speed_mps"] = avgSpeedMps;
    obj["total_traveled_m"] = totalTraveledM;
    
    // GPS
    QJsonObject gpsObj;
    gpsObj["avg_lat"] = gps.avgLat();
    gpsObj["avg_lon"] = gps.avgLon();
    gpsObj["avg_alt"] = gps.avgAlt();
    gpsObj["fix_count"] = gps.count;
    gpsObj["has_data"] = gps.hasData();
    obj["gps"] = gpsObj;
    
    return obj;
}

ScanSession ScanSession::fromJson(const QJsonObject& obj) {
    ScanSession s;
    s.sessionId = obj["session_id"].toString();
    s.sectionName = obj["section_name"].toString();
    s.robotId = obj["robot_id"].toString();
    s.robotIdSlug = obj["robot_id_slug"].toString();
    s.startTime = QDateTime::fromString(obj["start_time"].toString(), Qt::ISODate);
    s.endTime = QDateTime::fromString(obj["end_time"].toString(), Qt::ISODate);
    
    s.pathLengthM = obj["path_length_m"].toDouble();
    s.coverageAreaM2 = obj["coverage_area_m2"].toDouble();
    s.fieldAreaM2 = obj["field_area_m2"].toDouble();
    s.coveragePercent = obj["coverage_percent"].toDouble();
    s.numSwaths = obj["num_swaths"].toInt();
    s.numTurns = obj["num_turns"].toInt();
    s.numWaypoints = obj["num_waypoints"].toInt();
    s.swathWidthM = obj["swath_width_m"].toDouble();
    s.overlapPercent = obj["overlap_percent"].toDouble();
    s.patternType = obj["pattern_type"].toString();
    
    s.actualDurationSec = obj["actual_duration_sec"].toDouble();
    s.avgSpeedMps = obj["avg_speed_mps"].toDouble();
    s.totalTraveledM = obj["total_traveled_m"].toDouble();
    
    QJsonObject gpsObj = obj["gps"].toObject();
    if (gpsObj["has_data"].toBool()) {
        double avgLat = gpsObj["avg_lat"].toDouble();
        double avgLon = gpsObj["avg_lon"].toDouble();
        double avgAlt = gpsObj["avg_alt"].toDouble();
        int count = gpsObj["fix_count"].toInt();
        // Reconstruct accumulator
        s.gps.sumLat = avgLat * count;
        s.gps.sumLon = avgLon * count;
        s.gps.sumAlt = avgAlt * count;
        s.gps.count = count;
    }
    
    return s;
}

// =============================================================================
// ScanSessionTracker
// =============================================================================

ScanSessionTracker::ScanSessionTracker(rclcpp::Node::SharedPtr node, QObject* parent)
    : QObject(parent)
    , node_(node)
{
    if (node_) {
        // Subscribe to GPS with SensorDataQoS to match gps_driver publisher
        auto sensor_qos = rclcpp::SensorDataQoS().keep_last(10);
        gps_sub_ = node_->create_subscription<sensor_msgs::msg::NavSatFix>(
            "/gps/fix", sensor_qos,
            [this](const sensor_msgs::msg::NavSatFix::SharedPtr msg) {
                onGpsFix(msg);
            });
        qDebug() << "[ScanSessionTracker] Subscribed to /gps/fix";
    }
    
    loadSessions();
}

ScanSessionTracker::~ScanSessionTracker() {
    if (sessionActive_) {
        // Force end if destroyed while active
        endSession(0, 0, 0);
    }
}

void ScanSessionTracker::onGpsFix(const sensor_msgs::msg::NavSatFix::SharedPtr msg) {
    if (!sessionActive_) return;
    
    // Only accept valid fixes (status >= 0 means fix available)
    if (msg->status.status < 0) return;
    
    // Validate coordinates (basic sanity check)
    double lat = msg->latitude;
    double lon = msg->longitude;
    double alt = msg->altitude;
    
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return;
    
    QMutexLocker locker(&mutex_);
    gpsAccumulator_.addFix(lat, lon, alt);
    locker.unlock();
    
    emit gpsFixReceived(lat, lon, alt);
}

void ScanSessionTracker::startSession(const QString& sectionName,
                                      const QString& robotId,
                                      const QString& robotIdSlug,
                                      const CoverageStats& plannedStats,
                                      double swathWidth,
                                      const QString& patternType) {
    QMutexLocker locker(&mutex_);
    
    if (sessionActive_) {
        qDebug() << "[ScanSessionTracker] WARNING: Starting new session while previous active";
    }
    
    sessionActive_ = true;
    gpsAccumulator_.reset();
    
    currentSession_ = ScanSession{};
    currentSession_.sessionId = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    currentSession_.sectionName = sectionName;
    currentSession_.robotId = robotId;
    currentSession_.robotIdSlug = robotIdSlug;
    currentSession_.startTime = QDateTime::currentDateTime();
    
    // Copy planned stats
    currentSession_.pathLengthM = plannedStats.path_length_m;
    currentSession_.coverageAreaM2 = plannedStats.coverage_area_m2;
    currentSession_.fieldAreaM2 = plannedStats.polygon_area_m2;
    currentSession_.coveragePercent = plannedStats.coverage_percent;
    currentSession_.numSwaths = plannedStats.num_swaths;
    currentSession_.numTurns = plannedStats.num_turns;
    currentSession_.numWaypoints = plannedStats.num_waypoints;
    currentSession_.swathWidthM = swathWidth;
    currentSession_.overlapPercent = plannedStats.overlap_percent;
    currentSession_.patternType = patternType;
    
    locker.unlock();
    
    qDebug() << "[ScanSessionTracker] Session started:" << sectionName;
    emit sessionStarted(sectionName);
}

void ScanSessionTracker::endSession(double actualDurationSec, double avgSpeedMps, 
                                     double totalTraveledM) {
    QMutexLocker locker(&mutex_);
    
    if (!sessionActive_) return;
    
    sessionActive_ = false;
    currentSession_.endTime = QDateTime::currentDateTime();
    currentSession_.actualDurationSec = actualDurationSec;
    currentSession_.avgSpeedMps = avgSpeedMps;
    currentSession_.totalTraveledM = totalTraveledM;
    currentSession_.gps = gpsAccumulator_;
    
    // Save to persistent storage
    ScanSession completedSession = currentSession_;
    savedSessions_[completedSession.sessionId] = completedSession;
    
    locker.unlock();
    
    saveSession(completedSession);
    
    qDebug() << "[ScanSessionTracker] Session ended:" << completedSession.sectionName
             << "Duration:" << actualDurationSec << "s"
             << "GPS fixes:" << completedSession.gps.count;
    
    emit sessionEnded(completedSession.sectionName);
}

GpsAccumulator ScanSessionTracker::currentGps() const {
    QMutexLocker locker(&mutex_);
    return gpsAccumulator_;
}

ScanMetadata ScanSessionTracker::getMetadataForSection(const QString& sectionName,
                                                         const QString& dateFolder,
                                                         const QString& localPath) const {
    ScanMetadata meta;
    meta.sectionName = sectionName;
    meta.dateFolder = dateFolder;
    
    // Try to find matching session
    auto session = findSession(sectionName);
    
    if (session) {
        meta.robotId = session->robotId;
        meta.robotIdSlug = session->robotIdSlug;
        meta.pathLengthM = session->pathLengthM;
        meta.coverageAreaM2 = session->coverageAreaM2;
        meta.fieldAreaM2 = session->fieldAreaM2;
        meta.coveragePercent = session->coveragePercent;
        meta.numSwaths = session->numSwaths;
        meta.numTurns = session->numTurns;
        meta.numWaypoints = session->numWaypoints;
        meta.swathWidthM = session->swathWidthM;
        meta.overlapPercent = session->overlapPercent;
        meta.coveragePattern = session->patternType;
        
        meta.actualDurationSec = session->actualDurationSec;
        meta.avgSpeedMps = session->avgSpeedMps;
        meta.totalTraveledM = session->totalTraveledM;
        
        if (session->gps.hasData()) {
            meta.hasGps = true;
            meta.latitude = session->gps.avgLat();
            meta.longitude = session->gps.avgLon();
            meta.altitude = session->gps.avgAlt();
            meta.gpsFixCount = session->gps.count;
        }
        
        meta.scanTimestamp = session->startTime;
    } else {
        meta.scanTimestamp = QDateTime::currentDateTime();
    }
    
    // Scan local directory for data composition
    if (!localPath.isEmpty()) {
        QDir secDir(localPath);
        QStringList subDirs = secDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        
        meta.totalSizeBytes = 0;
        for (const QString& sub : subDirs) {
            ScanMetadata::DataTypeInfo info;
            QDirIterator it(secDir.filePath(sub), QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                info.fileCount++;
                info.sizeBytes += it.fileInfo().size();
            }
            meta.dataTypes[sub] = info;
            meta.totalSizeBytes += info.sizeBytes;
        }
        
        // Also count files directly in the section root
        QDirIterator rootIt(localPath, QDir::Files);
        qint64 rootSize = 0;
        int rootCount = 0;
        while (rootIt.hasNext()) {
            rootIt.next();
            rootCount++;
            rootSize += rootIt.fileInfo().size();
        }
        if (rootCount > 0) {
            ScanMetadata::DataTypeInfo rootInfo;
            rootInfo.fileCount = rootCount;
            rootInfo.sizeBytes = rootSize;
            meta.dataTypes["root"] = rootInfo;
            meta.totalSizeBytes += rootSize;
        }
    }
    
    return meta;
}

std::optional<ScanSession> ScanSessionTracker::findSession(const QString& sectionName) const {
    QMutexLocker locker(&mutex_);
    
    // Try exact match first
    for (const auto& session : savedSessions_) {
        if (session.sectionName == sectionName) {
            return session;
        }
    }
    
    // Try partial match (section name might have slight variations)
    for (const auto& session : savedSessions_) {
        if (session.sectionName.contains(sectionName, Qt::CaseInsensitive) ||
            sectionName.contains(session.sectionName, Qt::CaseInsensitive)) {
            return session;
        }
    }
    
    return std::nullopt;
}

QList<ScanSession> ScanSessionTracker::savedSessions() const {
    QMutexLocker locker(&mutex_);
    return savedSessions_.values();
}

// =============================================================================
// Persistence
// =============================================================================

QString ScanSessionTracker::sessionsFilePath() const {
    QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(configDir);
    return configDir + "/scan_sessions.json";
}

void ScanSessionTracker::saveSession(const ScanSession& session) {
    // Load existing file, append new session, save
    QString path = sessionsFilePath();
    
    QJsonArray sessions;
    
    QFile readFile(path);
    if (readFile.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(readFile.readAll());
        if (doc.isArray()) {
            sessions = doc.array();
        }
        readFile.close();
    }
    
    // Check if session already exists (update if so)
    bool found = false;
    for (int i = 0; i < sessions.size(); ++i) {
        QJsonObject obj = sessions[i].toObject();
        if (obj["session_id"].toString() == session.sessionId) {
            sessions[i] = session.toJson();
            found = true;
            break;
        }
    }
    
    if (!found) {
        sessions.append(session.toJson());
    }
    
    // Keep only last 500 sessions to prevent unbounded growth
    while (sessions.size() > 500) {
        sessions.removeFirst();
    }
    
    QFile writeFile(path);
    if (writeFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QJsonDocument doc(sessions);
        writeFile.write(doc.toJson(QJsonDocument::Indented));
        writeFile.close();
    }
    
    qDebug() << "[ScanSessionTracker] Saved session to" << path;
}

void ScanSessionTracker::loadSessions() {
    QString path = sessionsFilePath();
    QFile file(path);
    
    if (!file.open(QIODevice::ReadOnly)) {
        qDebug() << "[ScanSessionTracker] No sessions file found at" << path;
        return;
    }
    
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    
    if (!doc.isArray()) return;
    
    QJsonArray arr = doc.array();
    for (const QJsonValue& val : arr) {
        ScanSession s = ScanSession::fromJson(val.toObject());
        if (!s.sessionId.isEmpty()) {
            savedSessions_[s.sessionId] = s;
        }
    }
    
    qDebug() << "[ScanSessionTracker] Loaded" << savedSessions_.size() << "saved sessions";
}

} // namespace f2c_cpp
