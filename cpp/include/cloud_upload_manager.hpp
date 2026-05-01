/**
 * @file cloud_upload_manager.hpp
 * @brief Manages data uploads from laptop to AWS S3 via aws s3 sync
 * 
 * Features:
 * - Queue-based upload management
 * - Uses 'aws s3 sync' for folder-level resume
 * - Progress tracking via file count and size
 * - Metadata JSON generation and upload
 * - Reverse geocoding via OpenStreetMap Nominatim
 * - Persistent upload history
 * - Configurable S3 bucket/prefix/storage class
 */

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QProcess>
#include <QTimer>
#include <QDateTime>
#include <QMutex>
#include <QSettings>
#include <QSet>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>

#include <memory>
#include <deque>
#include <optional>

namespace f2c_cpp {

// Forward declare
struct CoverageStats;

// =============================================================================
// Data Structures
// =============================================================================

/**
 * @brief AWS S3 configuration
 */
struct AwsConfig {
    QString bucketName = "reportgen-data";
    QString prefix = "V2.00_Roofus_Data";
    QString region = "us-east-1";
    QString storageClass = "STANDARD";  // STANDARD, INTELLIGENT_TIERING, GLACIER
    int maxConcurrentRequests = 10;
    QString operatorName = "Operator";
    
    QJsonObject toJson() const;
    static AwsConfig fromJson(const QJsonObject& obj);
    void save() const;
    static AwsConfig load();
};

/**
 * @brief Scan session metadata (collected during operation)
 */
struct ScanMetadata {
    // Scan statistics (from CoverageStats)
    double pathLengthM = 0.0;
    double coverageAreaM2 = 0.0;
    double fieldAreaM2 = 0.0;
    double coveragePercent = 0.0;
    int numSwaths = 0;
    int numTurns = 0;
    int numWaypoints = 0;
    double swathWidthM = 0.0;
    double overlapPercent = 0.0;
    QString coveragePattern;        // e.g., "boustrophedon"
    
    // Execution stats
    double actualDurationSec = 0.0;
    double avgSpeedMps = 0.0;
    double totalTraveledM = 0.0;
    
    // GPS data (averaged during scan)
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
    int gpsFixCount = 0;
    bool hasGps = false;
    
    // Resolved location (after internet available)
    QString resolvedAddress;
    QString siteName;
    
    // Session info
    QString sectionName;
    QString dateFolder;
    QDateTime scanTimestamp;
    QString operatorName;
    
    // Data composition
    struct DataTypeInfo {
        int fileCount = 0;
        qint64 sizeBytes = 0;
    };
    QMap<QString, DataTypeInfo> dataTypes;  // "Visual" -> {count, size}
    qint64 totalSizeBytes = 0;
    
    QJsonObject toJson() const;
    bool isValid() const { return !sectionName.isEmpty(); }
};

/**
 * @brief State of an upload job
 */
enum class UploadState {
    Queued,
    Uploading,
    Completed,
    Failed,
    Cancelled,
    Paused
};

/**
 * @brief A single S3 upload job
 */
struct UploadJob {
    int jobId = 0;
    QString localPath;          // Local folder path
    QString s3Destination;      // s3://bucket/prefix/date/section/
    QString sectionName;        // Display name
    QString dateFolder;         // Date folder name
    
    qint64 totalBytes = 0;
    qint64 uploadedBytes = 0;
    int totalFiles = 0;
    int uploadedFiles = 0;
    double speedBytesPerSec = 0.0;
    int progressPercent = 0;
    
    UploadState state = UploadState::Queued;
    int retryCount = 0;
    QString errorMessage;
    
    QDateTime startTime;
    QDateTime endTime;
    
    ScanMetadata metadata;
    
    static constexpr int MaxRetries = 2;
    static constexpr int RetryDelayMs = 5000;
};

// =============================================================================
// Cloud Upload Manager
// =============================================================================

class CloudUploadManager : public QObject {
    Q_OBJECT

public:
    static CloudUploadManager& instance();
    
    // Prevent copying
    CloudUploadManager(const CloudUploadManager&) = delete;
    CloudUploadManager& operator=(const CloudUploadManager&) = delete;
    
    // Configuration
    AwsConfig config() const { return config_; }
    void setConfig(const AwsConfig& config);
    
    // Job management
    int enqueueUpload(const UploadJob& job);
    void cancelJob(int jobId);
    void cancelCurrentJob();
    void cancelAll();
    void pauseCurrentJob();
    void resumeCurrentJob();
    
    // Queue queries
    bool hasActiveUpload() const;
    bool hasQueuedJobs() const;
    int queueSize() const;
    std::optional<UploadJob> currentJob() const;
    std::vector<UploadJob> queuedJobs() const;
    
    // Upload history (persisted)
    bool isUploaded(const QString& sectionPath) const;
    void markAsUploaded(const QString& sectionPath);
    void unmarkAsUploaded(const QString& sectionPath);
    QSet<QString> uploadedSections() const { return uploadedSections_; }
    
    // Reverse geocoding (OpenStreetMap Nominatim)
    void reverseGeocode(double latitude, double longitude);
    
    // Scan local data directory for uploadable sections
    struct LocalSection {
        QString name;
        QString path;
        QString dateFolder;
        qint64 totalSize = 0;
        int fileCount = 0;
        bool alreadyUploaded = false;
        QStringList subFolders;
    };
    QList<LocalSection> scanLocalData(const QString& basePath) const;
    
    // Verify uploaded sections still exist in S3 (runs in background thread)
    void verifyUploadedSections(const QList<LocalSection>& sections);
    
signals:
    void uploadProgress(int jobId, qint64 uploaded, qint64 total, 
                       double speedMBps, int percent);
    void uploadStateChanged(int jobId, UploadState newState);
    void uploadCompleted(int jobId, bool success, const QString& message);
    void queueChanged();
    void geocodeResult(const QString& address);
    void uploadVerified(const QString& sectionPath, bool existsInCloud);
    
private slots:
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessReadyRead();
    void onProcessError(QProcess::ProcessError error);
    void onRetryTimer();
    void startNextJob();
    void onProgressPollTimer();

private:
    explicit CloudUploadManager(QObject* parent = nullptr);
    ~CloudUploadManager() override;
    
    // Internal
    void startUploadInternal(const UploadJob& job);
    QString buildAwsSyncCommand(const UploadJob& job) const;
    void generateAndUploadMetadata(const UploadJob& job);
    void countLocalFiles(const QString& path, int& fileCount, qint64& totalSize) const;
    void loadUploadHistory();
    void saveUploadHistory();
    
    // Progress tracking via periodic S3 listing
    void estimateProgress();
    
    // Configuration
    AwsConfig config_;
    
    // State
    int nextJobId_ = 1;
    std::optional<UploadJob> currentJob_;
    std::deque<UploadJob> jobQueue_;
    std::vector<UploadJob> completedJobs_;
    
    QProcess* uploadProcess_ = nullptr;
    QTimer* retryTimer_ = nullptr;
    QTimer* progressPollTimer_ = nullptr;
    
    QSet<QString> uploadedSections_;
    mutable QMutex mutex_;
    
    // Progress tracking
    int filesUploadedSoFar_ = 0;
    qint64 bytesUploadedSoFar_ = 0;
    QDateTime uploadStartTime_;
    
    // Network manager for geocoding
    QNetworkAccessManager* networkManager_ = nullptr;
};

} // namespace f2c_cpp
