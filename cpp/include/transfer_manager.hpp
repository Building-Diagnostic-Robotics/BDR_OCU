/**
 * @file transfer_manager.hpp
 * @brief Manages data transfers from robot to laptop via rsync over SSH
 * 
 * Features:
 * - Queue-based transfer management (uTorrent-style)
 * - Resume support via rsync --partial
 * - Progress tracking with speed calculation
 * - Automatic retry on failure
 * - Checksum verification
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

#include <memory>
#include <deque>
#include <optional>

namespace f2c_cpp {

// =============================================================================
// Data Structures
// =============================================================================

/**
 * @brief Information about a sub-folder within a section
 */
struct SubFolderInfo {
    QString name;           // e.g., "Visual_data", "GPR_scan_data"
    QString relativePath;   // Relative path within section
    qint64 sizeBytes = 0;
    int fileCount = 0;
    bool selected = false;
};

/**
 * @brief Metadata about a data collection section
 */
struct SectionInfo {
    QString name;               // e.g., "Section_1_093045"
    QString fullPath;           // e.g., "/R_DATA/January_27_2026/Acme_HQ/Section_1_093045"
    QString dayFolder;          // e.g., "January_27_2026"
    QString buildingSlug;       // e.g., "Acme_HQ" — empty for legacy
                                // (pre-modal) layout where sections lived
                                // at /R_DATA/<day>/Section_*.
    qint64 totalSizeBytes = 0;
    int fileCount = 0;
    QDateTime timestamp;
    
    // Data type presence flags
    bool hasVisualData = false;
    bool hasGprData = false;
    bool hasMap = false;
    bool hasRosbag = false;
    bool hasTiltCalib = false;
    
    // Status flags
    bool isRecordingInProgress = false;
    bool previouslyDownloaded = false;
    
    // Sub-folders
    QList<SubFolderInfo> subFolders;
    
    // Helper to get selected size
    qint64 selectedSize() const {
        qint64 total = 0;
        for (const auto& sf : subFolders) {
            if (sf.selected) total += sf.sizeBytes;
        }
        return total > 0 ? total : totalSizeBytes;
    }
    
    // Helper to get selected sub-folder paths
    QStringList selectedSubFolders() const {
        QStringList result;
        for (const auto& sf : subFolders) {
            if (sf.selected) result << sf.relativePath;
        }
        return result;
    }
};

/**
 * @brief State of a transfer job
 */
enum class TransferState {
    Queued,
    Running,
    Paused,
    Completed,
    Failed,
    Cancelled,
    Verifying
};

/**
 * @brief A single transfer job (section or sub-folder)
 */
struct TransferJob {
    int jobId = 0;
    QString sectionPath;        // Remote path
    QString sectionName;        // Display name
    QStringList subFolders;     // Empty = entire section
    QString localDestination;   // Local destination path
    
    qint64 totalBytes = 0;
    qint64 transferredBytes = 0;
    double speedBytesPerSec = 0.0;
    int progressPercent = 0;
    
    TransferState state = TransferState::Queued;
    int retryCount = 0;
    QString errorMessage;
    QString currentFile;        // Currently transferring file
    
    QDateTime startTime;
    QDateTime endTime;
    
    static constexpr int MaxRetries = 2;
    static constexpr int RetryDelayMs = 5000;
};

// =============================================================================
// Transfer Manager (Singleton)
// =============================================================================

class TransferManager : public QObject {
    Q_OBJECT

public:
    static TransferManager& instance();
    
    // Prevent copying
    TransferManager(const TransferManager&) = delete;
    TransferManager& operator=(const TransferManager&) = delete;
    
    // Configuration
    void setRobotHost(const QString& host) { robotHost_ = host; }
    void setRobotUser(const QString& user) { robotUser_ = user; }
    QString robotHost() const { return robotHost_; }
    QString robotUser() const { return robotUser_; }
    
    // Job management
    int enqueueJob(const TransferJob& job);
    void cancelJob(int jobId);
    void cancelCurrentJob();
    void cancelAll();
    void pauseCurrentJob();
    void resumeCurrentJob();
    
    // Queue control
    void clearQueue();
    bool hasActiveTransfer() const;
    bool hasQueuedJobs() const;
    int queueSize() const;
    
    // Job queries
    std::optional<TransferJob> currentJob() const;
    std::vector<TransferJob> queuedJobs() const;
    std::vector<TransferJob> completedJobs() const;
    
    // Download history (persisted)
    bool isDownloaded(const QString& sectionPath) const;
    void markAsDownloaded(const QString& sectionPath);
    void clearDownloadHistory();
    QStringList downloadHistory() const;
    
    // SSH operations for querying robot
    void fetchAvailableDates(const QString& dataPath);
    void fetchSectionsForDate(const QString& dataPath, const QString& date);
    void fetchSectionDetails(const QString& sectionPath);
    void checkConnection();
    
signals:
    // Transfer progress
    void progressUpdated(int jobId, qint64 transferred, qint64 total, 
                         double speedMBps, int percent, const QString& currentFile);
    void jobStateChanged(int jobId, TransferState newState);
    void jobCompleted(int jobId, bool success, const QString& message);
    void queueChanged();
    
    // SSH query results
    void connectionStatus(bool connected, const QString& message);
    void datesAvailable(const QStringList& dates);
    void sectionsAvailable(const QString& date, const QList<SectionInfo>& sections);
    void sectionDetailsReady(const SectionInfo& section);
    void queryError(const QString& operation, const QString& error);

private slots:
    void onProcessReadyRead();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessError(QProcess::ProcessError error);
    void onRetryTimer();
    void startNextJob();
    void checkProcessStatus();

private:
    explicit TransferManager(QObject* parent = nullptr);
    ~TransferManager() override;
    
    // Internal helpers
    void startTransferInternal(const TransferJob& job);
    void parseProgressLine(const QString& line);
    QString buildRsyncCommand(const TransferJob& job) const;
    void loadDownloadHistory();
    void saveDownloadHistory();
    SectionInfo parseSectionListing(const QString& output, const QString& basePath, 
                                    const QString& sectionName) const;
    
    // State
    QString robotHost_ = "192.168.168.101";
    QString robotUser_ = "roofus";
    
    int nextJobId_ = 1;
    std::optional<TransferJob> currentJob_;
    std::deque<TransferJob> jobQueue_;
    std::vector<TransferJob> completedJobs_;
    
    QProcess* rsyncProcess_ = nullptr;
    QProcess* sshQueryProcess_ = nullptr;
    QTimer* retryTimer_ = nullptr;
    QTimer* processMonitorTimer_ = nullptr;
    
    QSet<QString> downloadedSections_;
    mutable QMutex mutex_;
    
    // Progress parsing state
    QString progressBuffer_;
    qint64 lastBytesReported_ = 0;
    QDateTime lastProgressTime_;
    
    static constexpr int ProgressParseIntervalMs = 100;
};

// =============================================================================
// Utility Functions
// =============================================================================

QString formatFileSize(qint64 bytes);
QString formatSpeed(double bytesPerSec);
QString formatDuration(int seconds);

} // namespace f2c_cpp
