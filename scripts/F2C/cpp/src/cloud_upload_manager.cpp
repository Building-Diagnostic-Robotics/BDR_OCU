/**
 * @file cloud_upload_manager.cpp
 * @brief Implementation of CloudUploadManager for AWS S3 uploads
 */

#include "cloud_upload_manager.hpp"
#include <QThread>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QUrl>
#include <QUrlQuery>
#include <QNetworkRequest>
#include <QDebug>

namespace f2c_cpp {

// =============================================================================
// AwsConfig
// =============================================================================

QJsonObject AwsConfig::toJson() const {
    QJsonObject obj;
    obj["bucket_name"] = bucketName;
    obj["prefix"] = prefix;
    obj["region"] = region;
    obj["storage_class"] = storageClass;
    obj["max_concurrent_requests"] = maxConcurrentRequests;
    obj["operator_name"] = operatorName;
    return obj;
}

AwsConfig AwsConfig::fromJson(const QJsonObject& obj) {
    AwsConfig cfg;
    cfg.bucketName = obj["bucket_name"].toString(cfg.bucketName);
    cfg.prefix = obj["prefix"].toString(cfg.prefix);
    cfg.region = obj["region"].toString(cfg.region);
    cfg.storageClass = obj["storage_class"].toString(cfg.storageClass);
    cfg.maxConcurrentRequests = obj["max_concurrent_requests"].toInt(cfg.maxConcurrentRequests);
    cfg.operatorName = obj["operator_name"].toString(cfg.operatorName);
    return cfg;
}

void AwsConfig::save() const {
    QSettings settings("PilotControl", "BDRCoveragePlanner");
    settings.beginGroup("aws");
    settings.setValue("bucket_name", bucketName);
    settings.setValue("prefix", prefix);
    settings.setValue("region", region);
    settings.setValue("storage_class", storageClass);
    settings.setValue("max_concurrent_requests", maxConcurrentRequests);
    settings.setValue("operator_name", operatorName);
    settings.endGroup();
}

AwsConfig AwsConfig::load() {
    QSettings settings("PilotControl", "BDRCoveragePlanner");
    settings.beginGroup("aws");
    AwsConfig cfg;
    cfg.bucketName = settings.value("bucket_name", cfg.bucketName).toString();
    cfg.prefix = settings.value("prefix", cfg.prefix).toString();
    cfg.region = settings.value("region", cfg.region).toString();
    cfg.storageClass = settings.value("storage_class", cfg.storageClass).toString();
    cfg.maxConcurrentRequests = settings.value("max_concurrent_requests", cfg.maxConcurrentRequests).toInt();
    cfg.operatorName = settings.value("operator_name", cfg.operatorName).toString();
    settings.endGroup();
    return cfg;
}

// =============================================================================
// ScanMetadata
// =============================================================================

QJsonObject ScanMetadata::toJson() const {
    QJsonObject root;
    
    // Section info
    root["section"] = sectionName;
    root["date"] = dateFolder;
    root["operator"] = operatorName;
    // Backward-compatible key (previously hardcoded)
    root["robot"] = robotId.isEmpty() ? "Unknown" : robotId;
    if (!robotId.isEmpty()) {
        root["robot_id"] = robotId;
    }
    if (!robotIdSlug.isEmpty()) {
        root["robot_id_slug"] = robotIdSlug;
    }
    root["scan_timestamp"] = scanTimestamp.toString(Qt::ISODate);
    
    // Scan statistics
    QJsonObject stats;
    stats["path_length_m"] = pathLengthM;
    stats["coverage_area_sqm"] = coverageAreaM2;
    stats["field_area_sqm"] = fieldAreaM2;
    stats["coverage_percent"] = coveragePercent;
    stats["num_lines"] = numSwaths;
    stats["num_turns"] = numTurns;
    stats["num_waypoints"] = numWaypoints;
    stats["swath_width_m"] = swathWidthM;
    stats["overlap_percent"] = overlapPercent;
    stats["pattern"] = coveragePattern;
    root["scan_statistics"] = stats;
    
    // Execution statistics
    QJsonObject exec;
    exec["duration_sec"] = actualDurationSec;
    exec["avg_speed_mps"] = avgSpeedMps;
    exec["total_traveled_m"] = totalTraveledM;
    root["execution"] = exec;
    
    // Location
    QJsonObject location;
    if (hasGps) {
        location["latitude"] = latitude;
        location["longitude"] = longitude;
        location["altitude"] = altitude;
        location["gps_fix_count"] = gpsFixCount;
    }
    if (!resolvedAddress.isEmpty()) {
        location["address"] = resolvedAddress;
    }
    if (!siteName.isEmpty()) {
        location["site_name"] = siteName;
    }
    root["location"] = location;
    
    // Data composition
    QJsonObject data;
    for (auto it = dataTypes.constBegin(); it != dataTypes.constEnd(); ++it) {
        QJsonObject dtype;
        dtype["files"] = it.value().fileCount;
        dtype["size_bytes"] = it.value().sizeBytes;
        data[it.key()] = dtype;
    }
    root["data"] = data;
    root["total_size_bytes"] = totalSizeBytes;
    
    // Upload timestamp
    root["upload_timestamp"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    
    return root;
}

// =============================================================================
// CloudUploadManager - Singleton
// =============================================================================

CloudUploadManager& CloudUploadManager::instance() {
    static CloudUploadManager instance;
    return instance;
}

CloudUploadManager::CloudUploadManager(QObject* parent)
    : QObject(parent)
{
    // Move to main thread for signal/slot reliability
    if (QCoreApplication::instance()) {
        moveToThread(QCoreApplication::instance()->thread());
    }
    
    config_ = AwsConfig::load();
    
    uploadProcess_ = new QProcess(this);
    connect(uploadProcess_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &CloudUploadManager::onProcessFinished);
    connect(uploadProcess_, &QProcess::readyReadStandardOutput,
            this, &CloudUploadManager::onProcessReadyRead);
    connect(uploadProcess_, &QProcess::readyReadStandardError,
            this, &CloudUploadManager::onProcessReadyRead);
    connect(uploadProcess_, &QProcess::errorOccurred,
            this, &CloudUploadManager::onProcessError);
    
    retryTimer_ = new QTimer(this);
    retryTimer_->setSingleShot(true);
    connect(retryTimer_, &QTimer::timeout, this, &CloudUploadManager::onRetryTimer);
    
    // Poll for progress estimation every 3 seconds
    progressPollTimer_ = new QTimer(this);
    connect(progressPollTimer_, &QTimer::timeout, this, &CloudUploadManager::onProgressPollTimer);
    
    networkManager_ = new QNetworkAccessManager(this);
    
    loadUploadHistory();
}

CloudUploadManager::~CloudUploadManager() {
    if (uploadProcess_->state() != QProcess::NotRunning) {
        uploadProcess_->kill();
        uploadProcess_->waitForFinished(3000);
    }
}

void CloudUploadManager::setConfig(const AwsConfig& config) {
    config_ = config;
    config_.save();
}

// =============================================================================
// Job Management
// =============================================================================

int CloudUploadManager::enqueueUpload(const UploadJob& job) {
    QMutexLocker locker(&mutex_);
    
    UploadJob j = job;
    j.jobId = nextJobId_++;
    j.state = UploadState::Queued;
    
    qDebug() << "[CloudUpload] Enqueued job" << j.jobId << ":" << j.sectionName 
             << "->" << j.s3Destination;
    
    jobQueue_.push_back(j);
    locker.unlock();
    
    emit queueChanged();
    
    // Start if nothing active
    if (!hasActiveUpload()) {
        QMetaObject::invokeMethod(this, "startNextJob", Qt::QueuedConnection);
    }
    
    return j.jobId;
}

void CloudUploadManager::cancelJob(int jobId) {
    QMutexLocker locker(&mutex_);
    
    if (currentJob_ && currentJob_->jobId == jobId) {
        locker.unlock();
        cancelCurrentJob();
        return;
    }
    
    for (auto it = jobQueue_.begin(); it != jobQueue_.end(); ++it) {
        if (it->jobId == jobId) {
            it->state = UploadState::Cancelled;
            jobQueue_.erase(it);
            break;
        }
    }
    locker.unlock();
    emit queueChanged();
}

void CloudUploadManager::cancelCurrentJob() {
    QMutexLocker locker(&mutex_);
    if (!currentJob_) return;
    
    int jobId = currentJob_->jobId;
    QString s3Dest = currentJob_->s3Destination;
    QString localPath = currentJob_->localPath;
    bool wasAlreadyUploaded = uploadedSections_.contains(localPath);
    currentJob_->state = UploadState::Cancelled;
    locker.unlock();
    
    progressPollTimer_->stop();
    
    if (uploadProcess_->state() != QProcess::NotRunning) {
        uploadProcess_->kill();
        uploadProcess_->waitForFinished(3000);
    }
    
    emit uploadStateChanged(jobId, UploadState::Cancelled);
    emit uploadCompleted(jobId, false, "Cancelled by user");
    
    // Clean up partially uploaded files from S3
    // Only delete if this section wasn't already fully uploaded before
    if (!wasAlreadyUploaded && !s3Dest.isEmpty()) {
        qDebug() << "[CloudUpload] Cleaning up partial upload from S3:" << s3Dest;
        
        // Run cleanup as a child process parented to this manager
        // Use /bin/bash -c to ensure aws is found via PATH
        QString cmd = QString("/usr/bin/aws s3 rm --recursive \"%1\"").arg(s3Dest);
        QProcess* cleanup = new QProcess(this);
        connect(cleanup, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [cleanup, s3Dest](int exitCode, QProcess::ExitStatus) {
            if (exitCode == 0) {
                qDebug() << "[CloudUpload] S3 cleanup complete:" << s3Dest;
            } else {
                qDebug() << "[CloudUpload] S3 cleanup failed (exit" << exitCode << "):"
                         << QString::fromUtf8(cleanup->readAllStandardError()).trimmed();
            }
            cleanup->deleteLater();
        });
        connect(cleanup, &QProcess::errorOccurred, this, [cleanup, s3Dest](QProcess::ProcessError err) {
            qDebug() << "[CloudUpload] S3 cleanup process error:" << err << "for:" << s3Dest;
            cleanup->deleteLater();
        });
        cleanup->start("/bin/bash", QStringList() << "-c" << cmd);
        qDebug() << "[CloudUpload] Cleanup command launched:" << cmd;
    }
    
    QMutexLocker locker2(&mutex_);
    currentJob_.reset();
    locker2.unlock();
    
    emit queueChanged();
    QMetaObject::invokeMethod(this, "startNextJob", Qt::QueuedConnection);
}

void CloudUploadManager::pauseCurrentJob() {
    QMutexLocker locker(&mutex_);
    if (!currentJob_) return;
    
    int jobId = currentJob_->jobId;
    currentJob_->state = UploadState::Paused;
    
    // Re-queue at front so resume picks it up
    jobQueue_.push_front(*currentJob_);
    currentJob_.reset();
    locker.unlock();
    
    progressPollTimer_->stop();
    
    // Gracefully terminate (SIGTERM, then SIGKILL)
    if (uploadProcess_->state() != QProcess::NotRunning) {
        uploadProcess_->terminate();
        if (!uploadProcess_->waitForFinished(5000)) {
            uploadProcess_->kill();
            uploadProcess_->waitForFinished(2000);
        }
    }
    
    emit uploadStateChanged(jobId, UploadState::Paused);
    emit queueChanged();
    qDebug() << "[CloudUpload] Paused job" << jobId << "(re-queued for resume)";
}

void CloudUploadManager::resumeCurrentJob() {
    // Simply start the next job in queue — aws s3 sync is inherently resumable
    // (it only uploads files not yet in S3)
    QMutexLocker locker(&mutex_);
    if (currentJob_) return;  // Already running
    if (jobQueue_.empty()) return;
    
    // Change state from Paused back to Queued
    jobQueue_.front().state = UploadState::Queued;
    locker.unlock();
    
    startNextJob();
    qDebug() << "[CloudUpload] Resumed upload";
}

void CloudUploadManager::cancelAll() {
    QMutexLocker locker(&mutex_);
    jobQueue_.clear();
    locker.unlock();
    
    cancelCurrentJob();
}

bool CloudUploadManager::hasActiveUpload() const {
    QMutexLocker locker(&mutex_);
    return currentJob_.has_value() && currentJob_->state == UploadState::Uploading;
}

bool CloudUploadManager::hasQueuedJobs() const {
    QMutexLocker locker(&mutex_);
    return !jobQueue_.empty();
}

int CloudUploadManager::queueSize() const {
    QMutexLocker locker(&mutex_);
    return static_cast<int>(jobQueue_.size());
}

std::optional<UploadJob> CloudUploadManager::currentJob() const {
    QMutexLocker locker(&mutex_);
    return currentJob_;
}

std::vector<UploadJob> CloudUploadManager::queuedJobs() const {
    QMutexLocker locker(&mutex_);
    return std::vector<UploadJob>(jobQueue_.begin(), jobQueue_.end());
}

// =============================================================================
// Upload Execution
// =============================================================================

void CloudUploadManager::startNextJob() {
    QMutexLocker locker(&mutex_);
    
    if (currentJob_ || jobQueue_.empty()) return;
    
    currentJob_ = jobQueue_.front();
    jobQueue_.pop_front();
    currentJob_->state = UploadState::Uploading;
    currentJob_->startTime = QDateTime::currentDateTime();
    
    UploadJob jobCopy = *currentJob_;
    locker.unlock();
    
    emit uploadStateChanged(jobCopy.jobId, UploadState::Uploading);
    emit queueChanged();
    
    startUploadInternal(jobCopy);
}

void CloudUploadManager::startUploadInternal(const UploadJob& job) {
    qDebug() << "[CloudUpload] Starting upload:" << job.localPath << "->" << job.s3Destination;
    
    // First, generate metadata.json in the local folder
    generateAndUploadMetadata(job);
    
    // Build the aws s3 sync command
    QString cmd = buildAwsSyncCommand(job);
    qDebug() << "[CloudUpload] Command:" << cmd;
    
    uploadStartTime_ = QDateTime::currentDateTime();
    filesUploadedSoFar_ = 0;
    bytesUploadedSoFar_ = 0;
    
    // Non-blocking start: errorOccurred signal handles start failure
    uploadProcess_->start("/bin/bash", QStringList() << "-c" << cmd);
    
    // Start polling for progress
    progressPollTimer_->start(2000);
}

QString CloudUploadManager::buildAwsSyncCommand(const UploadJob& job) const {
    QStringList parts;
    parts << "aws" << "s3" << "sync";
    parts << QString("\"%1\"").arg(job.localPath);
    parts << QString("\"%1\"").arg(job.s3Destination);
    
    // Storage class
    if (!config_.storageClass.isEmpty() && config_.storageClass != "STANDARD") {
        parts << "--storage-class" << config_.storageClass;
    }
    
    // Show per-file output so we can parse progress (e.g. "upload: ./file to s3://...")
    // Do NOT use --only-show-errors — we need the output for progress tracking
    // --no-progress removes the per-byte progress bar but keeps "upload:" lines
    parts << "--no-progress";
    
    return parts.join(" ");
}

void CloudUploadManager::generateAndUploadMetadata(const UploadJob& job) {
    // Create metadata.json in the local folder
    QString metadataPath = job.localPath + "/metadata.json";
    
    QJsonObject metaJson = job.metadata.toJson();
    
    QFile file(metadataPath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QJsonDocument doc(metaJson);
        file.write(doc.toJson(QJsonDocument::Indented));
        file.close();
        qDebug() << "[CloudUpload] Generated metadata.json at" << metadataPath;
    } else {
        qDebug() << "[CloudUpload] WARNING: Could not write metadata.json:" << file.errorString();
    }
}

// =============================================================================
// Process Callbacks
// =============================================================================

void CloudUploadManager::onProcessReadyRead() {
    QByteArray stdOut = uploadProcess_->readAllStandardOutput();
    QByteArray stdErr = uploadProcess_->readAllStandardError();
    
    if (!stdErr.isEmpty()) {
        QString errStr = QString::fromUtf8(stdErr).trimmed();
        if (!errStr.isEmpty()) {
            qDebug() << "[CloudUpload] stderr:" << errStr;
        }
    }
    
    if (!stdOut.isEmpty()) {
        // Parse "upload: ./path/file to s3://..." lines
        QString output = QString::fromUtf8(stdOut);
        QStringList lines = output.split('\n', Qt::SkipEmptyParts);
        
        for (const QString& line : lines) {
            QString trimmed = line.trimmed();
            if (trimmed.startsWith("upload:")) {
                filesUploadedSoFar_++;
            }
        }
        
        // Emit progress based on file count
        QMutexLocker locker(&mutex_);
        if (currentJob_) {
            int jobId = currentJob_->jobId;
            int totalFiles = currentJob_->totalFiles;
            qint64 totalBytes = currentJob_->totalBytes;
            locker.unlock();
            
            int percent = -1;
            if (totalFiles > 0) {
                percent = qMin(99, static_cast<int>(
                    100.0 * filesUploadedSoFar_ / totalFiles));
            }
            
            // Estimate speed from elapsed time
            qint64 elapsedMs = uploadStartTime_.msecsTo(QDateTime::currentDateTime());
            double speedMBps = 0.0;
            qint64 estUploaded = 0;
            if (totalFiles > 0 && totalBytes > 0) {
                estUploaded = totalBytes * filesUploadedSoFar_ / totalFiles;
                if (elapsedMs > 1000) {
                    speedMBps = (estUploaded / (1024.0 * 1024.0)) / (elapsedMs / 1000.0);
                }
            }
            
            emit uploadProgress(jobId, estUploaded, totalBytes, speedMBps, percent);
        }
    }
}

void CloudUploadManager::onProcessFinished(int exitCode, QProcess::ExitStatus) {
    progressPollTimer_->stop();
    
    QMutexLocker locker(&mutex_);
    if (!currentJob_) return;
    
    int jobId = currentJob_->jobId;
    QString sectionName = currentJob_->sectionName;
    QString localPath = currentJob_->localPath;
    
    if (exitCode == 0) {
        // Success
        currentJob_->state = UploadState::Completed;
        currentJob_->endTime = QDateTime::currentDateTime();
        currentJob_->progressPercent = 100;
        completedJobs_.push_back(*currentJob_);
        currentJob_.reset();
        locker.unlock();
        
        // Mark as uploaded (outside mutex)
        markAsUploaded(localPath);
        
        qDebug() << "[CloudUpload] Upload completed successfully:" << sectionName;
        emit uploadProgress(jobId, 0, 0, 0.0, 100);
        emit uploadCompleted(jobId, true, "Upload complete");
        emit queueChanged();
        
        QMetaObject::invokeMethod(this, "startNextJob", Qt::QueuedConnection);
    } else {
        // Failure
        int retryCount = currentJob_->retryCount;
        
        if (retryCount < UploadJob::MaxRetries) {
            currentJob_->retryCount++;
            currentJob_->state = UploadState::Queued;
            qDebug() << "[CloudUpload] Upload failed (exit" << exitCode 
                     << "), retry" << currentJob_->retryCount << "/" << UploadJob::MaxRetries;
            
            // Push back to front of queue for retry
            jobQueue_.push_front(*currentJob_);
            currentJob_.reset();
            locker.unlock();
            
            retryTimer_->start(UploadJob::RetryDelayMs);
        } else {
            currentJob_->state = UploadState::Failed;
            currentJob_->errorMessage = QString("aws s3 sync failed with exit code %1").arg(exitCode);
            currentJob_->endTime = QDateTime::currentDateTime();
            completedJobs_.push_back(*currentJob_);
            currentJob_.reset();
            locker.unlock();
            
            qDebug() << "[CloudUpload] Upload failed permanently:" << sectionName;
            emit uploadCompleted(jobId, false, "Upload failed after retries");
            emit queueChanged();
            
            QMetaObject::invokeMethod(this, "startNextJob", Qt::QueuedConnection);
        }
    }
}

void CloudUploadManager::onProcessError(QProcess::ProcessError error) {
    progressPollTimer_->stop();
    
    QMutexLocker locker(&mutex_);
    if (!currentJob_) return;
    
    int jobId = currentJob_->jobId;
    int retryCount = currentJob_->retryCount;
    
    qDebug() << "[CloudUpload] Process error:" << error << "for job" << jobId;
    
    if (error == QProcess::Crashed) {
        if (retryCount < UploadJob::MaxRetries) {
            currentJob_->retryCount++;
            currentJob_->state = UploadState::Queued;
            jobQueue_.push_front(*currentJob_);
            currentJob_.reset();
            locker.unlock();
            
            retryTimer_->start(UploadJob::RetryDelayMs);
        } else {
            currentJob_->state = UploadState::Failed;
            currentJob_->errorMessage = "Upload process crashed";
            completedJobs_.push_back(*currentJob_);
            currentJob_.reset();
            locker.unlock();
            
            emit uploadCompleted(jobId, false, "Process crashed");
            emit queueChanged();
            QMetaObject::invokeMethod(this, "startNextJob", Qt::QueuedConnection);
        }
    } else {
        currentJob_->state = UploadState::Failed;
        currentJob_->errorMessage = "Failed to start upload process";
        completedJobs_.push_back(*currentJob_);
        currentJob_.reset();
        locker.unlock();
        
        emit uploadCompleted(jobId, false, "Process error");
        emit queueChanged();
        QMetaObject::invokeMethod(this, "startNextJob", Qt::QueuedConnection);
    }
}

void CloudUploadManager::onRetryTimer() {
    startNextJob();
}

// =============================================================================
// Progress Tracking
// =============================================================================

void CloudUploadManager::onProgressPollTimer() {
    estimateProgress();
}

void CloudUploadManager::estimateProgress() {
    QMutexLocker locker(&mutex_);
    if (!currentJob_) return;
    
    int jobId = currentJob_->jobId;
    qint64 totalBytes = currentJob_->totalBytes;
    int totalFiles = currentJob_->totalFiles;
    locker.unlock();
    
    if (uploadProcess_->state() != QProcess::Running) return;
    
    qint64 elapsedMs = uploadStartTime_.msecsTo(QDateTime::currentDateTime());
    if (elapsedMs < 500) return;
    
    int percent = -1;
    qint64 estUploaded = 0;
    double speedMBps = 0.0;
    
    if (totalFiles > 0) {
        percent = qMin(99, static_cast<int>(
            100.0 * filesUploadedSoFar_ / totalFiles));
        if (totalBytes > 0) {
            estUploaded = totalBytes * filesUploadedSoFar_ / totalFiles;
        }
    }
    
    if (estUploaded > 0 && elapsedMs > 1000) {
        speedMBps = (estUploaded / (1024.0 * 1024.0)) / (elapsedMs / 1000.0);
    }
    
    emit uploadProgress(jobId, estUploaded, totalBytes, speedMBps, percent);
}

// =============================================================================
// Upload History
// =============================================================================

bool CloudUploadManager::isUploaded(const QString& sectionPath) const {
    QMutexLocker locker(&mutex_);
    return uploadedSections_.contains(sectionPath);
}

void CloudUploadManager::markAsUploaded(const QString& sectionPath) {
    QMutexLocker locker(&mutex_);
    uploadedSections_.insert(sectionPath);
    locker.unlock();
    saveUploadHistory();
}

void CloudUploadManager::unmarkAsUploaded(const QString& sectionPath) {
    QMutexLocker locker(&mutex_);
    uploadedSections_.remove(sectionPath);
    locker.unlock();
    saveUploadHistory();
    qDebug() << "[CloudUpload] Unmarked as uploaded:" << sectionPath;
}

void CloudUploadManager::verifyUploadedSections(const QList<LocalSection>& sections) {
    // Collect sections that are marked as uploaded
    QList<QPair<QString, QString>> toVerify;  // (localPath, s3Dest)
    
    for (const auto& sec : sections) {
        if (!sec.alreadyUploaded) continue;
        
        QString s3Dest;
        if (sec.dateFolder.isEmpty()) {
            s3Dest = QString("s3://%1/%2/%3/")
                .arg(config_.bucketName, config_.prefix, sec.name);
        } else {
            s3Dest = QString("s3://%1/%2/%3/%4/")
                .arg(config_.bucketName, config_.prefix, sec.dateFolder, sec.name);
        }
        toVerify.append({sec.path, s3Dest});
    }
    
    if (toVerify.isEmpty()) return;
    
    // Run verification in background thread
    QThread* thread = QThread::create([this, toVerify]() {
        for (const auto& pair : toVerify) {
            const QString& localPath = pair.first;
            const QString& s3Dest = pair.second;
            
            QProcess check;
            check.start("aws", QStringList() << "s3" << "ls" << s3Dest);
            check.waitForFinished(10000);
            
            bool exists = (check.exitCode() == 0 &&
                          !QString::fromUtf8(check.readAllStandardOutput()).trimmed().isEmpty());
            
            QMetaObject::invokeMethod(this, [this, localPath, exists]() {
                emit uploadVerified(localPath, exists);
                if (!exists) {
                    unmarkAsUploaded(localPath);
                }
            }, Qt::QueuedConnection);
        }
    });
    
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void CloudUploadManager::loadUploadHistory() {
    QSettings settings("PilotControl", "BDRCoveragePlanner");
    QStringList list = settings.value("cloud_upload/uploaded_sections").toStringList();
    uploadedSections_ = QSet<QString>(list.begin(), list.end());
    qDebug() << "[CloudUpload] Loaded" << uploadedSections_.size() << "upload history entries";
}

void CloudUploadManager::saveUploadHistory() {
    QSettings settings("PilotControl", "BDRCoveragePlanner");
    QStringList list(uploadedSections_.begin(), uploadedSections_.end());
    settings.setValue("cloud_upload/uploaded_sections", QVariant(list));
}

// =============================================================================
// Local Data Scanning
// =============================================================================

QList<CloudUploadManager::LocalSection> CloudUploadManager::scanLocalData(const QString& basePath) const {
    QList<LocalSection> sections;
    QDir baseDir(basePath);
    
    if (!baseDir.exists()) return sections;
    
    // Supports two layouts:
    //   Flat:   basePath/{section_name}/          (e.g., Roofus/Section_1_130222/)
    //   Dated:  basePath/{date_folder}/{section_name}/
    //
    // Detect flat layout: if any top-level dir contains files or known sub-folders
    // (GPR_scan_data, Visual_data, rosbag, etc.), treat it as a section directly.
    
    QStringList topDirs = baseDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    
    for (const QString& topDir : topDirs) {
        QString topPath = baseDir.filePath(topDir);
        QDir topFolder(topPath);
        
        // Check if this directory is itself a section (has files or known data sub-folders)
        bool isSection = false;
        
        // Has files directly in it?
        QStringList files = topFolder.entryList(QDir::Files);
        if (!files.isEmpty()) {
            isSection = true;
        }
        
        // Has known data sub-folders?
        if (!isSection) {
            QStringList knownSubs = {"GPR_scan_data", "Visual_data", "rosbag", 
                                      "rosbag_data", "thermal_data", "map_data"};
            QStringList subs = topFolder.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            for (const QString& sub : subs) {
                if (knownSubs.contains(sub, Qt::CaseInsensitive) ||
                    sub.startsWith("GPR", Qt::CaseInsensitive) ||
                    sub.startsWith("Visual", Qt::CaseInsensitive) ||
                    sub.startsWith("rosbag", Qt::CaseInsensitive)) {
                    isSection = true;
                    break;
                }
            }
        }
        
        if (isSection) {
            // Flat layout: topDir IS a section
            LocalSection sec;
            sec.name = topDir;
            sec.path = topPath;
            sec.dateFolder = "";  // No date folder
            sec.alreadyUploaded = isUploaded(sec.path);
            
            QDirIterator it(sec.path, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                sec.fileCount++;
                sec.totalSize += it.fileInfo().size();
            }
            
            sec.subFolders = topFolder.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
            sections.append(sec);
        } else {
            // Dated layout: topDir is a date folder containing sections
            QStringList sectionDirs = topFolder.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
            
            for (const QString& sectionDir : sectionDirs) {
                LocalSection sec;
                sec.name = sectionDir;
                sec.path = topFolder.filePath(sectionDir);
                sec.dateFolder = topDir;
                sec.alreadyUploaded = isUploaded(sec.path);
                
                QDirIterator it(sec.path, QDir::Files, QDirIterator::Subdirectories);
                while (it.hasNext()) {
                    it.next();
                    sec.fileCount++;
                    sec.totalSize += it.fileInfo().size();
                }
                
                QDir secDir(sec.path);
                sec.subFolders = secDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
                sections.append(sec);
            }
        }
    }
    
    return sections;
}

void CloudUploadManager::countLocalFiles(const QString& path, int& fileCount, qint64& totalSize) const {
    fileCount = 0;
    totalSize = 0;
    QDirIterator it(path, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        fileCount++;
        totalSize += it.fileInfo().size();
    }
}

// =============================================================================
// Reverse Geocoding (OpenStreetMap Nominatim)
// =============================================================================

void CloudUploadManager::reverseGeocode(double latitude, double longitude) {
    // Nominatim API: https://nominatim.openstreetmap.org/reverse?format=json&lat=X&lon=Y
    QUrl url("https://nominatim.openstreetmap.org/reverse");
    QUrlQuery query;
    query.addQueryItem("format", "json");
    query.addQueryItem("lat", QString::number(latitude, 'f', 9));
    query.addQueryItem("lon", QString::number(longitude, 'f', 9));
    query.addQueryItem("zoom", "18");
    query.addQueryItem("addressdetails", "1");
    url.setQuery(query);
    
    QNetworkRequest request(url);
    // Nominatim requires a User-Agent per their usage policy
    request.setRawHeader("User-Agent", "BDRCoveragePlanner/1.0 (pilot_control)");
    request.setRawHeader("Accept-Language", "en");
    
    QNetworkReply* reply = networkManager_->get(request);
    
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        
        if (reply->error() != QNetworkReply::NoError) {
            qDebug() << "[CloudUpload] Geocoding failed:" << reply->errorString();
            emit geocodeResult("");
            return;
        }
        
        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        
        if (doc.isObject()) {
            QJsonObject obj = doc.object();
            QString displayName = obj["display_name"].toString();
            
            qDebug() << "[CloudUpload] Geocoded address:" << displayName;
            emit geocodeResult(displayName);
        } else {
            emit geocodeResult("");
        }
    });
}

} // namespace f2c_cpp
