/**
 * @file transfer_manager.cpp
 * @brief Implementation of TransferManager for rsync-based data transfer
 */

#include "transfer_manager.hpp"

#include <QRegularExpression>
#include <QDir>
#include <QFileInfo>
#include <QDebug>
#include <QCoreApplication>

namespace f2c_cpp {

// =============================================================================
// Utility Functions
// =============================================================================

static QString buildSshOptions(const QString& knownHostsFile, int connectTimeoutSec) {
    QStringList opts;
    opts << "-o" << QString("ConnectTimeout=%1").arg(connectTimeoutSec)
         << "-o" << "BatchMode=yes";

    if (!knownHostsFile.trimmed().isEmpty()) {
        opts << "-o" << "StrictHostKeyChecking=yes"
             << "-o" << QString("UserKnownHostsFile=%1").arg(knownHostsFile)
             << "-o" << "GlobalKnownHostsFile=/dev/null";
    } else {
        // Fallback (legacy behavior) if no pinning material is provided
        opts << "-o" << "StrictHostKeyChecking=no";
    }

    return opts.join(' ');
}

QString formatFileSize(qint64 bytes) {
    if (bytes < 0) return "Unknown";
    
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unitIndex = 0;
    double size = static_cast<double>(bytes);
    
    while (size >= 1024.0 && unitIndex < 4) {
        size /= 1024.0;
        unitIndex++;
    }
    
    if (unitIndex == 0) {
        return QString("%1 B").arg(static_cast<int>(size));
    }
    return QString("%1 %2").arg(size, 0, 'f', unitIndex > 2 ? 2 : 1).arg(units[unitIndex]);
}

QString formatSpeed(double bytesPerSec) {
    if (bytesPerSec < 0) return "-- MB/s";
    return QString("%1/s").arg(formatFileSize(static_cast<qint64>(bytesPerSec)));
}

QString formatDuration(int seconds) {
    if (seconds < 0) return "--:--";
    
    int hours = seconds / 3600;
    int mins = (seconds % 3600) / 60;
    int secs = seconds % 60;
    
    if (hours > 0) {
        return QString("%1:%2:%3")
            .arg(hours)
            .arg(mins, 2, 10, QChar('0'))
            .arg(secs, 2, 10, QChar('0'));
    }
    return QString("%1:%2")
        .arg(mins, 2, 10, QChar('0'))
        .arg(secs, 2, 10, QChar('0'));
}

// =============================================================================
// TransferManager Implementation
// =============================================================================

TransferManager& TransferManager::instance() {
    static TransferManager instance;
    return instance;
}

TransferManager::TransferManager(QObject* parent)
    : QObject(parent)
{
    // Move to the main thread's event loop if we're not already there
    if (QCoreApplication::instance()) {
        this->moveToThread(QCoreApplication::instance()->thread());
    }
    
    retryTimer_ = new QTimer(this);
    retryTimer_->setSingleShot(true);
    connect(retryTimer_, &QTimer::timeout, this, &TransferManager::onRetryTimer);
    
    // Add a process monitoring timer
    processMonitorTimer_ = new QTimer(this);
    processMonitorTimer_->setInterval(500); // Check every 500ms
    connect(processMonitorTimer_, &QTimer::timeout, this, &TransferManager::checkProcessStatus);
    
    loadDownloadHistory();
}

TransferManager::~TransferManager() {
    cancelAll();
    saveDownloadHistory();
}

// =============================================================================
// Job Management
// =============================================================================

int TransferManager::enqueueJob(const TransferJob& job) {
    qDebug() << "[TransferManager] enqueueJob() called for:" << job.sectionPath;
    
    QMutexLocker locker(&mutex_);
    
    TransferJob newJob = job;
    newJob.jobId = nextJobId_++;
    newJob.state = TransferState::Queued;
    
    jobQueue_.push_back(newJob);
    
    qDebug() << "[TransferManager] Job added to queue, ID:" << newJob.jobId << ", queue size:" << jobQueue_.size();
    
    locker.unlock();
    emit queueChanged();
    
    // Start processing if nothing is running
    bool hasActive = hasActiveTransfer();
    qDebug() << "[TransferManager] hasActiveTransfer:" << hasActive;
    
    if (!hasActive) {
        qDebug() << "[TransferManager] Scheduling startNextJob";
        QTimer::singleShot(0, this, &TransferManager::startNextJob);
    }
    
    return newJob.jobId;
}

void TransferManager::cancelJob(int jobId) {
    QMutexLocker locker(&mutex_);
    
    // Check if it's the current job
    if (currentJob_ && currentJob_->jobId == jobId) {
        locker.unlock();
        cancelCurrentJob();
        return;
    }
    
    // Remove from queue
    auto it = std::find_if(jobQueue_.begin(), jobQueue_.end(),
                           [jobId](const TransferJob& j) { return j.jobId == jobId; });
    if (it != jobQueue_.end()) {
        int id = it->jobId;
        it->state = TransferState::Cancelled;
        completedJobs_.push_back(*it);
        jobQueue_.erase(it);
        
        locker.unlock();
        emit jobStateChanged(id, TransferState::Cancelled);
        emit queueChanged();
    }
}

void TransferManager::cancelCurrentJob() {
    QMutexLocker locker(&mutex_);
    
    if (!currentJob_) return;
    
    if (rsyncProcess_ && rsyncProcess_->state() != QProcess::NotRunning) {
        rsyncProcess_->kill();
        rsyncProcess_->waitForFinished(3000);
    }
    
    currentJob_->state = TransferState::Cancelled;
    currentJob_->endTime = QDateTime::currentDateTime();
    completedJobs_.push_back(*currentJob_);
    
    int jobId = currentJob_->jobId;
    currentJob_.reset();
    
    locker.unlock();
    
    emit jobStateChanged(jobId, TransferState::Cancelled);
    emit jobCompleted(jobId, false, "Cancelled by user");
    emit queueChanged();
    
    // Start next job
    QTimer::singleShot(0, this, &TransferManager::startNextJob);
}

void TransferManager::cancelAll() {
    QMutexLocker locker(&mutex_);
    
    // Cancel current
    if (currentJob_) {
        if (rsyncProcess_ && rsyncProcess_->state() != QProcess::NotRunning) {
            rsyncProcess_->kill();
            rsyncProcess_->waitForFinished(3000);
        }
        currentJob_->state = TransferState::Cancelled;
        completedJobs_.push_back(*currentJob_);
        int jobId = currentJob_->jobId;
        currentJob_.reset();
        
        locker.unlock();
        emit jobStateChanged(jobId, TransferState::Cancelled);
        locker.relock();
    }
    
    // Cancel all queued
    for (auto& job : jobQueue_) {
        job.state = TransferState::Cancelled;
        completedJobs_.push_back(job);
    }
    jobQueue_.clear();
    
    locker.unlock();
    emit queueChanged();
}

void TransferManager::clearQueue() {
    QMutexLocker locker(&mutex_);
    
    for (auto& job : jobQueue_) {
        job.state = TransferState::Cancelled;
        emit jobStateChanged(job.jobId, TransferState::Cancelled);
    }
    jobQueue_.clear();
    
    locker.unlock();
    emit queueChanged();
}

bool TransferManager::hasActiveTransfer() const {
    QMutexLocker locker(&mutex_);
    return currentJob_.has_value() && 
           (currentJob_->state == TransferState::Running ||
            currentJob_->state == TransferState::Verifying);
}

bool TransferManager::hasQueuedJobs() const {
    QMutexLocker locker(&mutex_);
    return !jobQueue_.empty();
}

int TransferManager::queueSize() const {
    QMutexLocker locker(&mutex_);
    return static_cast<int>(jobQueue_.size());
}

std::optional<TransferJob> TransferManager::currentJob() const {
    QMutexLocker locker(&mutex_);
    return currentJob_;
}

std::vector<TransferJob> TransferManager::queuedJobs() const {
    QMutexLocker locker(&mutex_);
    return std::vector<TransferJob>(jobQueue_.begin(), jobQueue_.end());
}

std::vector<TransferJob> TransferManager::completedJobs() const {
    QMutexLocker locker(&mutex_);
    return completedJobs_;
}

// =============================================================================
// Transfer Execution
// =============================================================================

void TransferManager::startNextJob() {
    qDebug() << "[TransferManager] startNextJob() called";
    
    QMutexLocker locker(&mutex_);
    
    if (currentJob_) {
        qDebug() << "[TransferManager] Already have currentJob, returning";
        return;
    }
    if (jobQueue_.empty()) {
        qDebug() << "[TransferManager] Queue is empty, returning";
        return;
    }
    
    currentJob_ = jobQueue_.front();
    jobQueue_.pop_front();
    
    // Copy the job for modification (avoid reference to optional issues)
    TransferJob job = *currentJob_;
    
    qDebug() << "[TransferManager] Starting job ID:" << job.jobId << "path:" << job.sectionPath;
    
    locker.unlock();
    
    emit queueChanged();
    
    // Start the transfer (this will update currentJob_ internally)
    startTransferInternal(job);
}

void TransferManager::startTransferInternal(const TransferJob& job) {
    qDebug() << "[TransferManager] startTransferInternal() for job:" << job.jobId;
    
    // Update the current job state
    {
        QMutexLocker locker(&mutex_);
        if (currentJob_) {
            currentJob_->state = TransferState::Running;
            currentJob_->startTime = QDateTime::currentDateTime();
            currentJob_->errorMessage.clear();
        }
    }
    
    emit jobStateChanged(job.jobId, TransferState::Running);
    
    // Create destination directory
    bool dirCreated = QDir().mkpath(job.localDestination);
    qDebug() << "[TransferManager] Created destination dir:" << job.localDestination << "success:" << dirCreated;
    
    // Build and execute rsync command
    QString cmd = buildRsyncCommand(job);
    
    qDebug() << "[TransferManager] rsync command:" << cmd;
    
    if (!rsyncProcess_) {
        qDebug() << "[TransferManager] Creating new QProcess for rsync";
        rsyncProcess_ = new QProcess(this);
        connect(rsyncProcess_, &QProcess::readyReadStandardOutput,
                this, &TransferManager::onProcessReadyRead);
        connect(rsyncProcess_, &QProcess::readyReadStandardError,
                this, &TransferManager::onProcessReadyRead);
        connect(rsyncProcess_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &TransferManager::onProcessFinished);
        connect(rsyncProcess_, &QProcess::errorOccurred,
                this, &TransferManager::onProcessError);
    } else {
        qDebug() << "[TransferManager] Reusing existing QProcess, state:" << rsyncProcess_->state();
        // Make sure the old process is not running
        if (rsyncProcess_->state() != QProcess::NotRunning) {
            qDebug() << "[TransferManager] WARNING: Process still running, killing it";
            rsyncProcess_->kill();
            rsyncProcess_->waitForFinished(3000);
        }
    }
    
    progressBuffer_.clear();
    lastBytesReported_ = 0;
    lastProgressTime_ = QDateTime::currentDateTime();
    
    qDebug() << "[TransferManager] Starting bash with command";
    rsyncProcess_->start("/bin/bash", QStringList() << "-c" << cmd);
    
    bool started = rsyncProcess_->waitForStarted(5000);
    qDebug() << "[TransferManager] Process started:" << started << "state:" << rsyncProcess_->state();
    
    if (!started) {
        qDebug() << "[TransferManager] Failed to start rsync process, error:" << rsyncProcess_->errorString();
        // Trigger error handling
        onProcessError(QProcess::FailedToStart);
    } else {
        // Start monitoring timer as backup for signal delivery
        qDebug() << "[TransferManager] Starting process monitor timer";
        processMonitorTimer_->start();
    }
}

QString TransferManager::buildRsyncCommand(const TransferJob& job) const {
    // rsync with:
    // -a: archive mode (preserves permissions, timestamps, etc.)
    // -v: verbose
    // -z: compression during transfer
    // --checksum: verify file integrity
    // --partial: keep partially transferred files (for resume)
    // --info=progress2: show OVERALL progress (not per-file)
    // --stats: show transfer statistics at the end
    // -e: specify ssh options
    
    // Use single quotes for the outer shell, escape properly
    // The command will be: rsync -avz ... -e 'ssh ...' user@host:'path/' 'dest/'
    
    QString userHost = QString("%1@%2").arg(robotUser_, robotHost_);
    
    QStringList cmdParts;
    cmdParts << "rsync"
             << "-az"  // removed -v to reduce noise
             << "--checksum"
             << "--partial"
             << "--info=progress2"  // OVERALL progress instead of per-file
             << "--stats"
             << "-e" << QString("'%1'").arg(QString("ssh %1").arg(buildSshOptions(knownHostsFile_, 30)));
    
    // Build source path(s) - use single quotes and escape any single quotes in path
    auto escapePath = [](const QString& path) -> QString {
        QString escaped = path;
        escaped.replace("'", "'\\''");  // Escape single quotes
        return QString("'%1'").arg(escaped);
    };
    
    if (job.subFolders.isEmpty()) {
        // Transfer entire section
        QString remotePath = QString("%1:%2/").arg(userHost, job.sectionPath);
        cmdParts << escapePath(remotePath);
    } else {
        // Transfer specific sub-folders
        for (const QString& sf : job.subFolders) {
            QString remotePath = QString("%1:%2/%3/").arg(userHost, job.sectionPath, sf);
            cmdParts << escapePath(remotePath);
        }
    }
    
    // Destination - ensure trailing slash
    QString destPath = job.localDestination;
    if (!destPath.endsWith('/')) destPath += '/';
    cmdParts << escapePath(destPath);
    
    return cmdParts.join(" ");
}

void TransferManager::onProcessReadyRead() {
    qDebug() << "[TransferManager] onProcessReadyRead() called";
    
    if (!rsyncProcess_ || !currentJob_) {
        qDebug() << "[TransferManager] No process or no current job";
        return;
    }
    
    // Read both stdout and stderr
    QByteArray output = rsyncProcess_->readAllStandardOutput();
    QByteArray error = rsyncProcess_->readAllStandardError();
    
    qDebug() << "[TransferManager] Read stdout:" << output.size() << "bytes, stderr:" << error.size() << "bytes";
    
    QString data = QString::fromUtf8(output + error);
    progressBuffer_ += data;
    
    // Parse progress lines - handle both \n and \r (rsync uses \r for progress updates)
    // Replace \r with \n for uniform parsing
    progressBuffer_.replace('\r', '\n');
    
    QStringList lines = progressBuffer_.split('\n', Qt::SkipEmptyParts);
    if (!lines.isEmpty()) {
        progressBuffer_ = lines.takeLast(); // Keep potentially incomplete last line
    }
    
    for (const QString& line : lines) {
        QString trimmed = line.trimmed();
        if (!trimmed.isEmpty()) {
            parseProgressLine(trimmed);
        }
    }
}

void TransferManager::parseProgressLine(const QString& line) {
    if (line.isEmpty() || !currentJob_) return;
    
    // Progress line regex: captures bytes, percent, speed
    // Example: "  1,234,567  42%   12.34MB/s    0:01:23"
    static QRegularExpression progressRx(
        R"(^\s*([\d,]+)\s+(\d+)%\s+([\d.]+[kMG]?B/s))"
    );
    
    QRegularExpressionMatch match = progressRx.match(line);
    if (match.hasMatch()) {
        // Parse bytes transferred
        QString bytesStr = match.captured(1).remove(',');
        qint64 bytes = bytesStr.toLongLong();
        
        // Parse percent
        int percent = match.captured(2).toInt();
        
        // Parse speed
        QString speedStr = match.captured(3);
        double speedBytes = 0.0;
        
        static QRegularExpression speedRx(R"(([\d.]+)([kMG]?)B/s)");
        QRegularExpressionMatch speedMatch = speedRx.match(speedStr);
        if (speedMatch.hasMatch()) {
            speedBytes = speedMatch.captured(1).toDouble();
            QString unit = speedMatch.captured(2);
            if (unit == "k") speedBytes *= 1024;
            else if (unit == "M") speedBytes *= 1024 * 1024;
            else if (unit == "G") speedBytes *= 1024 * 1024 * 1024;
        }
        
        QMutexLocker locker(&mutex_);
        if (currentJob_) {
            currentJob_->transferredBytes = bytes;
            currentJob_->progressPercent = percent;
            currentJob_->speedBytesPerSec = speedBytes;
            
            int jobId = currentJob_->jobId;
            qint64 total = currentJob_->totalBytes;
            
            locker.unlock();
            
            emit progressUpdated(jobId, bytes, total, speedBytes / (1024 * 1024), 
                                 percent, currentJob_->currentFile);
        }
        return;
    }
    
    // Check for file being transferred
    // Files typically appear as lines starting without whitespace
    if (!line.startsWith(' ') && !line.startsWith('\t') && 
        !line.contains('%') && !line.contains("sending") && 
        !line.contains("receiving") && !line.contains("total size")) {
        QMutexLocker locker(&mutex_);
        if (currentJob_) {
            currentJob_->currentFile = line;
        }
    }
}

void TransferManager::onProcessFinished(int exitCode, QProcess::ExitStatus status) {
    qDebug() << "[TransferManager] onProcessFinished() exitCode:" << exitCode << "status:" << status;
    
    QMutexLocker locker(&mutex_);
    
    if (!currentJob_) {
        qDebug() << "[TransferManager] No current job in onProcessFinished";
        return;
    }
    
    currentJob_->endTime = QDateTime::currentDateTime();
    
    bool success = (status == QProcess::NormalExit && exitCode == 0);
    qDebug() << "[TransferManager] Transfer success:" << success;
    
    if (success) {
        // Transfer completed successfully
        currentJob_->state = TransferState::Completed;
        currentJob_->progressPercent = 100;
        
        completedJobs_.push_back(*currentJob_);
        
        int jobId = currentJob_->jobId;
        QString dest = currentJob_->localDestination;
        QString sectionPath = currentJob_->sectionPath;
        currentJob_.reset();
        
        // Stop the monitor timer
        processMonitorTimer_->stop();
        
        locker.unlock();
        
        // Mark as downloaded AFTER unlocking mutex (markAsDownloaded also uses mutex)
        markAsDownloaded(sectionPath);
        
        qDebug() << "[TransferManager] Job" << jobId << "completed, emitting signals";
        
        emit jobStateChanged(jobId, TransferState::Completed);
        emit jobCompleted(jobId, true, QString("Transfer complete: %1").arg(dest));
        emit queueChanged();
        
        qDebug() << "[TransferManager] Scheduling next job";
        // Start next job
        QTimer::singleShot(0, this, &TransferManager::startNextJob);
        
    } else {
        // Transfer failed
        currentJob_->retryCount++;
        
        if (currentJob_->retryCount <= TransferJob::MaxRetries) {
            // Schedule retry
            qDebug() << "[TransferManager] Transfer failed, retry" << currentJob_->retryCount 
                     << "of" << TransferJob::MaxRetries;
            
            currentJob_->errorMessage = QString("Retry %1/%2...")
                .arg(currentJob_->retryCount).arg(TransferJob::MaxRetries);
            
            // Stop monitor timer during retry delay
            processMonitorTimer_->stop();
            
            locker.unlock();
            
            retryTimer_->start(TransferJob::RetryDelayMs);
            
        } else {
            // Max retries exceeded
            currentJob_->state = TransferState::Failed;
            currentJob_->errorMessage = QString("Failed after %1 retries (exit code: %2)")
                .arg(TransferJob::MaxRetries).arg(exitCode);
            
            completedJobs_.push_back(*currentJob_);
            
            int jobId = currentJob_->jobId;
            QString error = currentJob_->errorMessage;
            currentJob_.reset();
            
            // Stop the monitor timer
            processMonitorTimer_->stop();
            
            locker.unlock();
            
            qDebug() << "[TransferManager] Job" << jobId << "failed, emitting signals";
            
            emit jobStateChanged(jobId, TransferState::Failed);
            emit jobCompleted(jobId, false, error);
            emit queueChanged();
            
            qDebug() << "[TransferManager] Scheduling next job after failure";
            // Start next job
            QTimer::singleShot(0, this, &TransferManager::startNextJob);
        }
    }
}

void TransferManager::onProcessError(QProcess::ProcessError error) {
    qDebug() << "[TransferManager] onProcessError() error:" << error;
    
    // Stop the monitor timer immediately
    if (processMonitorTimer_) {
        processMonitorTimer_->stop();
    }
    
    QMutexLocker locker(&mutex_);
    
    if (!currentJob_) {
        qDebug() << "[TransferManager] No current job in onProcessError";
        return;
    }
    
    QString errorMsg;
    switch (error) {
        case QProcess::FailedToStart:
            errorMsg = "rsync failed to start (not installed?)";
            break;
        case QProcess::Crashed:
            errorMsg = "rsync crashed";
            break;
        case QProcess::Timedout:
            errorMsg = "rsync timed out";
            break;
        case QProcess::WriteError:
            errorMsg = "Write error";
            break;
        case QProcess::ReadError:
            errorMsg = "Read error";
            break;
        default:
            errorMsg = "Unknown error";
            break;
    }
    
    qDebug() << "[TransferManager] Error message:" << errorMsg;
    currentJob_->errorMessage = errorMsg;
    currentJob_->endTime = QDateTime::currentDateTime();
    
    // Handle retry logic here since onProcessFinished may not be called after crash
    currentJob_->retryCount++;
    
    if (currentJob_->retryCount <= TransferJob::MaxRetries) {
        // Schedule retry
        qDebug() << "[TransferManager] Process error, retry" << currentJob_->retryCount 
                 << "of" << TransferJob::MaxRetries;
        
        currentJob_->errorMessage = QString("Retry %1/%2 after error: %3")
            .arg(currentJob_->retryCount).arg(TransferJob::MaxRetries).arg(errorMsg);
        
        locker.unlock();
        
        retryTimer_->start(TransferJob::RetryDelayMs);
        
    } else {
        // Max retries exceeded - mark as failed and move on
        qDebug() << "[TransferManager] Max retries exceeded after error";
        
        currentJob_->state = TransferState::Failed;
        currentJob_->errorMessage = QString("Failed after %1 retries: %2")
            .arg(TransferJob::MaxRetries).arg(errorMsg);
        
        completedJobs_.push_back(*currentJob_);
        
        int jobId = currentJob_->jobId;
        QString finalError = currentJob_->errorMessage;
        currentJob_.reset();
        
        locker.unlock();
        
        qDebug() << "[TransferManager] Job" << jobId << "failed due to error, emitting signals";
        
        emit jobStateChanged(jobId, TransferState::Failed);
        emit jobCompleted(jobId, false, finalError);
        emit queueChanged();
        
        // Check if there are more jobs in the queue
        qDebug() << "[TransferManager] Scheduling next job after error";
        QTimer::singleShot(0, this, &TransferManager::startNextJob);
    }
}

void TransferManager::onRetryTimer() {
    QMutexLocker locker(&mutex_);
    
    if (currentJob_ && currentJob_->retryCount <= TransferJob::MaxRetries) {
        TransferJob job = *currentJob_;
        locker.unlock();
        startTransferInternal(job);
    }
}

void TransferManager::checkProcessStatus() {
    if (!rsyncProcess_) {
        processMonitorTimer_->stop();
        return;
    }
    
    qDebug() << "[TransferManager] checkProcessStatus() - state:" << rsyncProcess_->state();
    
    // Try to read any available output
    QByteArray stdOut = rsyncProcess_->readAllStandardOutput();
    QByteArray stdErr = rsyncProcess_->readAllStandardError();
    
    if (!stdOut.isEmpty() || !stdErr.isEmpty()) {
        qDebug() << "[TransferManager] Polled output - stdout:" << stdOut.size() << "stderr:" << stdErr.size();
        
        QString data = QString::fromUtf8(stdOut + stdErr);
        progressBuffer_ += data;
        progressBuffer_.replace('\r', '\n');
        
        QStringList lines = progressBuffer_.split('\n', Qt::SkipEmptyParts);
        if (!lines.isEmpty()) {
            progressBuffer_ = lines.takeLast();
        }
        
        for (const QString& line : lines) {
            QString trimmed = line.trimmed();
            if (!trimmed.isEmpty()) {
                parseProgressLine(trimmed);
            }
        }
    }
    
    // Check if process has finished
    if (rsyncProcess_->state() == QProcess::NotRunning) {
        qDebug() << "[TransferManager] Process finished (detected by polling)";
        processMonitorTimer_->stop();
        
        int exitCode = rsyncProcess_->exitCode();
        QProcess::ExitStatus status = rsyncProcess_->exitStatus();
        
        qDebug() << "[TransferManager] Exit code:" << exitCode << "status:" << status;
        
        // Read any remaining output
        QByteArray finalOut = rsyncProcess_->readAllStandardOutput();
        QByteArray finalErr = rsyncProcess_->readAllStandardError();
        if (!finalOut.isEmpty() || !finalErr.isEmpty()) {
            qDebug() << "[TransferManager] Final output:" << QString::fromUtf8(finalOut + finalErr);
        }
        
        // Call the finished handler
        onProcessFinished(exitCode, status);
    }
}

// =============================================================================
// Download History
// =============================================================================

bool TransferManager::isDownloaded(const QString& sectionPath) const {
    QMutexLocker locker(&mutex_);
    if (downloadHistoryUsesHostPrefix_) {
        return downloadedSections_.contains(robotHost_ + "|" + sectionPath);
    }
    return downloadedSections_.contains(sectionPath);
}

void TransferManager::markAsDownloaded(const QString& sectionPath) {
    QMutexLocker locker(&mutex_);
    downloadedSections_.insert(robotHost_ + "|" + sectionPath);
    downloadHistoryUsesHostPrefix_ = true;
    locker.unlock();
    saveDownloadHistory();
}

void TransferManager::clearDownloadHistory() {
    QMutexLocker locker(&mutex_);
    downloadedSections_.clear();
    downloadHistoryUsesHostPrefix_ = false;
    locker.unlock();
    saveDownloadHistory();
}

QStringList TransferManager::downloadHistory() const {
    QMutexLocker locker(&mutex_);
    return downloadedSections_.values();
}

void TransferManager::loadDownloadHistory() {
    QSettings settings("PilotControl", "BDRCoveragePlanner");
    QStringList history = settings.value("data_transfer/downloaded_sections").toStringList();
    
    QMutexLocker locker(&mutex_);
    downloadedSections_ = QSet<QString>(history.begin(), history.end());
    downloadHistoryUsesHostPrefix_ = false;
    for (const auto& entry : downloadedSections_) {
        if (entry.contains('|')) {
            downloadHistoryUsesHostPrefix_ = true;
            break;
        }
    }
}

void TransferManager::saveDownloadHistory() {
    QSettings settings("PilotControl", "BDRCoveragePlanner");
    
    QMutexLocker locker(&mutex_);
    QStringList list = downloadedSections_.values();
    settings.setValue("data_transfer/downloaded_sections", QVariant(list));
}

// =============================================================================
// SSH Query Operations
// =============================================================================

void TransferManager::checkConnection() {
    if (sshQueryProcess_ && sshQueryProcess_->state() != QProcess::NotRunning) {
        return; // Query already in progress
    }
    
    if (!sshQueryProcess_) {
        sshQueryProcess_ = new QProcess(this);
    }
    
    // Simple connection test
    QString cmd = QString(
        "ssh %1 %2@%3 'echo connected'"
    ).arg(buildSshOptions(knownHostsFile_, 5), robotUser_, robotHost_);
    
    qDebug() << "Connection check:" << cmd;
    
    sshQueryProcess_->disconnect();
    connect(sshQueryProcess_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus status) {
        bool connected = (status == QProcess::NormalExit && exitCode == 0);
        QString msg = connected ? "Connected" : "Connection failed";
        qDebug() << "Connection result:" << connected << exitCode;
        emit connectionStatus(connected, msg);
    });
    
    sshQueryProcess_->start("/bin/bash", QStringList() << "-c" << cmd);
}

void TransferManager::fetchAvailableDates(const QString& dataPath) {
    if (sshQueryProcess_ && sshQueryProcess_->state() != QProcess::NotRunning) {
        sshQueryProcess_->kill();
        sshQueryProcess_->waitForFinished(1000);
    }
    
    if (!sshQueryProcess_) {
        sshQueryProcess_ = new QProcess(this);
    }
    
    // List directories in data path (day folders)
    // Use simpler command that's more portable
    QString cmd = QString(
        "ssh %1 %2@%3 \"ls -1 '%4' 2>/dev/null | sort -r\""
    ).arg(buildSshOptions(knownHostsFile_, 10), robotUser_, robotHost_, dataPath);
    
    qDebug() << "Fetching dates:" << cmd;
    
    sshQueryProcess_->disconnect();
    connect(sshQueryProcess_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus status) {
        QString output = QString::fromUtf8(sshQueryProcess_->readAllStandardOutput());
        QString errOutput = QString::fromUtf8(sshQueryProcess_->readAllStandardError());
        
        qDebug() << "Dates result - exit:" << exitCode << "output:" << output << "err:" << errOutput;
        
        if (status == QProcess::NormalExit && exitCode == 0) {
            QStringList dates = output.split('\n', Qt::SkipEmptyParts);
            // Filter to only include date-like folders (Month_Day_Year format)
            QStringList validDates;
            for (const QString& d : dates) {
                if (d.contains('_') && !d.startsWith('.')) {
                    validDates << d;
                }
            }
            emit datesAvailable(validDates);
        } else {
            emit queryError("fetchDates", QString("Failed: %1").arg(errOutput.isEmpty() ? "Connection error" : errOutput));
        }
    });
    
    sshQueryProcess_->start("/bin/bash", QStringList() << "-c" << cmd);
}

void TransferManager::fetchSectionsForDate(const QString& dataPath, const QString& date) {
    if (sshQueryProcess_ && sshQueryProcess_->state() != QProcess::NotRunning) {
        sshQueryProcess_->kill();
        sshQueryProcess_->waitForFinished(1000);
    }
    
    if (!sshQueryProcess_) {
        sshQueryProcess_ = new QProcess(this);
    }
    
    QString dayPath = QString("%1/%2").arg(dataPath, date);
    
    // Get detailed listing of sections with sizes
    // Use a simpler, more robust script
    QString remoteScript = QString(
        "cd '%1' 2>/dev/null || exit 1; "
        "for d in Section_*/; do "
        "  [ -d \"$d\" ] || continue; "
        "  name=$(basename \"$d\"); "
        "  size=$(du -sb \"$d\" 2>/dev/null | cut -f1 || echo 0); "
        "  count=$(find \"$d\" -type f 2>/dev/null | wc -l || echo 0); "
        "  mtime=$(stat -c %%Y \"$d\" 2>/dev/null || echo 0); "
        "  hasVisual=0; [ -d \"${d}Visual_data\" ] && hasVisual=1; "
        "  hasGpr=0; [ -d \"${d}GPR_scan_data\" ] && hasGpr=1; "
        "  hasMap=0; ls \"$d\"*_map_*.pcd >/dev/null 2>&1 && hasMap=1; "
        "  hasBag=0; ls -d \"${d}\"rosbag_*/ >/dev/null 2>&1 && hasBag=1; "
        "  hasTilt=0; ls \"$d\"tilt_correction_*.npz >/dev/null 2>&1 && hasTilt=1; "
        "  inProgress=0; "
        "  echo \"$name|$size|$count|$mtime|$hasVisual|$hasGpr|$hasMap|$hasBag|$hasTilt|$inProgress\"; "
        "done"
    ).arg(dayPath);
    
    QString cmd = QString(
        "ssh %1 %2@%3 '%4'"
    ).arg(buildSshOptions(knownHostsFile_, 10), robotUser_, robotHost_, remoteScript.replace("'", "'\"'\"'"));
    
    qDebug() << "Fetching sections for" << date;
    
    sshQueryProcess_->disconnect();
    connect(sshQueryProcess_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, dataPath, date](int exitCode, QProcess::ExitStatus status) {
        QString output = QString::fromUtf8(sshQueryProcess_->readAllStandardOutput());
        QString errOutput = QString::fromUtf8(sshQueryProcess_->readAllStandardError());
        
        qDebug() << "Sections result - exit:" << exitCode << "lines:" << output.split('\n').count();
        
        if (status == QProcess::NormalExit && (exitCode == 0 || !output.isEmpty())) {
            QStringList lines = output.split('\n', Qt::SkipEmptyParts);
            
            QList<SectionInfo> sections;
            for (const QString& line : lines) {
                QStringList parts = line.split('|');
                if (parts.size() >= 10) {
                    SectionInfo info;
                    info.name = parts[0];
                    info.fullPath = QString("%1/%2/%3").arg(dataPath, date, info.name);
                    info.dayFolder = date;
                    info.totalSizeBytes = parts[1].toLongLong();
                    info.fileCount = parts[2].toInt();
                    info.timestamp = QDateTime::fromSecsSinceEpoch(parts[3].toLongLong());
                    info.hasVisualData = (parts[4] == "1");
                    info.hasGprData = (parts[5] == "1");
                    info.hasMap = (parts[6] == "1");
                    info.hasRosbag = (parts[7] == "1");
                    info.hasTiltCalib = (parts[8] == "1");
                    info.isRecordingInProgress = (parts[9] == "1");
                    info.previouslyDownloaded = isDownloaded(info.fullPath);
                    
                    sections.append(info);
                }
            }
            
            emit sectionsAvailable(date, sections);
        } else {
            emit queryError("fetchSections", 
                           QString("Failed to list sections for %1: %2").arg(date, errOutput));
        }
    });
    
    sshQueryProcess_->start("/bin/bash", QStringList() << "-c" << cmd);
}

void TransferManager::fetchSectionDetails(const QString& sectionPath) {
    if (sshQueryProcess_ && sshQueryProcess_->state() != QProcess::NotRunning) {
        sshQueryProcess_->kill();
        sshQueryProcess_->waitForFinished(1000);
    }
    
    if (!sshQueryProcess_) {
        sshQueryProcess_ = new QProcess(this);
    }
    
    // Get detailed info about sub-folders
    QString remoteScript = QString(
        "cd '%1' 2>/dev/null || exit 1; "
        "for d in */; do "
        "  [ -d \"$d\" ] || continue; "
        "  name=$(basename \"$d\"); "
        "  size=$(du -sb \"$d\" 2>/dev/null | cut -f1 || echo 0); "
        "  count=$(find \"$d\" -type f 2>/dev/null | wc -l || echo 0); "
        "  echo \"$name|$size|$count\"; "
        "done; "
        "for f in *.pcd *.npz *.csv; do "
        "  [ -f \"$f\" ] || continue; "
        "  name=$(basename \"$f\"); "
        "  size=$(stat -c %%s \"$f\" 2>/dev/null || echo 0); "
        "  echo \"$name|$size|1\"; "
        "done"
    ).arg(sectionPath);
    
    QString cmd = QString(
        "ssh %1 %2@%3 '%4'"
    ).arg(buildSshOptions(knownHostsFile_, 10), robotUser_, robotHost_, remoteScript.replace("'", "'\"'\"'"));
    
    sshQueryProcess_->disconnect();
    connect(sshQueryProcess_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, sectionPath](int exitCode, QProcess::ExitStatus status) {
        if (status == QProcess::NormalExit && exitCode == 0) {
            QString output = QString::fromUtf8(sshQueryProcess_->readAllStandardOutput());
            SectionInfo info = parseSectionListing(output, sectionPath, 
                                                    QFileInfo(sectionPath).fileName());
            emit sectionDetailsReady(info);
        } else {
            emit queryError("fetchDetails", 
                           QString("Failed to get details for %1").arg(sectionPath));
        }
    });
    
    sshQueryProcess_->start("/bin/bash", QStringList() << "-c" << cmd);
}

SectionInfo TransferManager::parseSectionListing(const QString& output, 
                                                  const QString& basePath,
                                                  const QString& sectionName) const {
    SectionInfo info;
    info.name = sectionName;
    info.fullPath = basePath;
    info.previouslyDownloaded = isDownloaded(basePath);
    
    QStringList lines = output.split('\n', Qt::SkipEmptyParts);
    
    for (const QString& line : lines) {
        QStringList parts = line.split('|');
        if (parts.size() >= 3) {
            SubFolderInfo sf;
            sf.name = parts[0];
            sf.relativePath = sf.name;
            sf.sizeBytes = parts[1].toLongLong();
            sf.fileCount = parts[2].toInt();
            sf.selected = true;  // Default to selected
            
            info.subFolders.append(sf);
            info.totalSizeBytes += sf.sizeBytes;
            info.fileCount += sf.fileCount;
            
            // Set type flags based on name
            if (sf.name == "Visual_data") info.hasVisualData = true;
            else if (sf.name == "GPR_scan_data") info.hasGprData = true;
            else if (sf.name.contains("_map_") && sf.name.endsWith(".pcd")) info.hasMap = true;
            else if (sf.name.startsWith("rosbag_")) info.hasRosbag = true;
            else if (sf.name.startsWith("tilt_correction_")) info.hasTiltCalib = true;
        }
    }
    
    return info;
}

} // namespace f2c_cpp
