/**
 * @file network_monitor.cpp
 * @brief Implementation of NetworkMonitor for connectivity tracking
 */

#include "network_monitor.hpp"
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

namespace f2c_cpp {

NetworkMonitor::NetworkMonitor(QObject* parent)
    : QObject(parent)
{
    pingProcess_ = new QProcess(this);
    connect(pingProcess_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &NetworkMonitor::onPingFinished);
    
    monitorTimer_ = new QTimer(this);
    connect(monitorTimer_, &QTimer::timeout, this, &NetworkMonitor::performCheck);
}

NetworkMonitor::~NetworkMonitor() {
    stopMonitoring();
    if (pingProcess_->state() != QProcess::NotRunning) {
        pingProcess_->kill();
        pingProcess_->waitForFinished(2000);
    }
}

void NetworkMonitor::startMonitoring(int intervalMs) {
    if (intervalMs < 2000) intervalMs = 2000;  // Minimum 2s interval
    monitorTimer_->start(intervalMs);
    performCheck();  // Immediate first check
}

void NetworkMonitor::stopMonitoring() {
    if (monitorTimer_) {
        monitorTimer_->stop();
    }
}

void NetworkMonitor::checkNow() {
    performCheck();
}

void NetworkMonitor::performCheck() {
    if (checkInProgress_) return;
    
    if (pingProcess_->state() != QProcess::NotRunning) {
        pingProcess_->kill();
        pingProcess_->waitForFinished(1000);
    }
    
    checkInProgress_ = true;
    
    // Use ping with 3 packets, 2 second timeout
    QStringList args;
    args << "-c" << "3" << "-W" << "2" << "-q" << pingTarget_;
    
    pingProcess_->start("ping", args);
    
    if (!pingProcess_->waitForStarted(3000)) {
        checkInProgress_ = false;
        currentStatus_.internetAvailable = false;
        currentStatus_.statusMessage = "Failed to start ping process";
        currentStatus_.lastCheck = QDateTime::currentDateTime();
        
        PingSample sample;
        sample.success = false;
        sample.timestamp = QDateTime::currentDateTime();
    pingSamples_.append(sample);
    while (pingSamples_.size() > stabilityWindowSize_) pingSamples_.removeFirst();
    
    updateStability();
    emit networkStatusUpdated(currentStatus_);
    }
}

void NetworkMonitor::onPingFinished(int exitCode, QProcess::ExitStatus) {
    checkInProgress_ = false;
    
    QString output = QString::fromUtf8(pingProcess_->readAllStandardOutput());
    parsePingOutput(output, exitCode);
    
    currentStatus_.lastCheck = QDateTime::currentDateTime();
    
    // Detect state change
    bool wasOnline = lastOnlineState_;
    lastOnlineState_ = currentStatus_.internetAvailable;
    
    emit networkStatusUpdated(currentStatus_);
    
    if (wasOnline != currentStatus_.internetAvailable) {
        emit connectivityChanged(currentStatus_.internetAvailable);
    }
}

void NetworkMonitor::parsePingOutput(const QString& output, int exitCode) {
    PingSample sample;
    sample.timestamp = QDateTime::currentDateTime();
    
    if (exitCode != 0) {
        sample.success = false;
        currentStatus_.internetAvailable = false;
        currentStatus_.latencyMs = -1;
        currentStatus_.packetLossPercent = 100.0;
        currentStatus_.statusMessage = "No internet connection";
    } else {
        // Parse: "3 packets transmitted, 3 received, 0% packet loss, time 2003ms"
        static QRegularExpression lossRe(R"((\d+)% packet loss)");
        auto lossMatch = lossRe.match(output);
        double loss = lossMatch.hasMatch() ? lossMatch.captured(1).toDouble() : 100.0;
        
        // Parse: "rtt min/avg/max/mdev = 12.345/15.678/18.901/2.345 ms"
        static QRegularExpression rttRe(R"(rtt min/avg/max/mdev = [\d.]+/([\d.]+)/[\d.]+/[\d.]+ ms)");
        auto rttMatch = rttRe.match(output);
        int avgLatency = rttMatch.hasMatch() ? static_cast<int>(rttMatch.captured(1).toDouble()) : -1;
        
        currentStatus_.internetAvailable = (loss < 100.0);
        currentStatus_.latencyMs = avgLatency;
        currentStatus_.packetLossPercent = loss;
        
        sample.success = currentStatus_.internetAvailable;
        sample.latencyMs = avgLatency;
        
        if (currentStatus_.internetAvailable) {
            currentStatus_.statusMessage = QString("Connected (%1ms, %2% loss)")
                .arg(avgLatency).arg(loss, 0, 'f', 0);
        } else {
            currentStatus_.statusMessage = "100% packet loss";
        }
    }
    
    pingSamples_.append(sample);
    while (pingSamples_.size() > stabilityWindowSize_) pingSamples_.removeFirst();
    
    updateStability();
}

void NetworkMonitor::updateStability() {
    if (pingSamples_.size() < 3) {
        currentStatus_.isStable = false;
        return;
    }
    
    int successCount = 0;
    int totalLatency = 0;
    int latencySamples = 0;
    
    for (const auto& s : pingSamples_) {
        if (s.success) {
            successCount++;
            if (s.latencyMs > 0) {
                totalLatency += s.latencyMs;
                latencySamples++;
            }
        }
    }
    
    double lossRate = 1.0 - (double(successCount) / double(pingSamples_.size()));
    currentStatus_.isStable = (lossRate * 100.0 <= stabilityMaxLoss_) && successCount >= 2;
}

void NetworkMonitor::validateAwsConfig(const QString& bucketName) {
    // Run the entire validation in a background thread so the UI never blocks.
    // Results are delivered via the awsValidationComplete signal (queued connection).
    if (awsValidationRunning_) return;
    awsValidationRunning_ = true;
    
    QString bucket = bucketName;
    
    QThread* thread = QThread::create([this, bucket]() {
        AwsStatus status{};
        status.bucketName = bucket;
        
        // Step 1: Check if AWS CLI is installed
        QProcess checkCli;
        checkCli.start("which", QStringList() << "aws");
        checkCli.waitForFinished(5000);
        if (checkCli.exitCode() != 0) {
            status.cliInstalled = false;
            status.errorMessage = "CLI not installed";
            QMetaObject::invokeMethod(this, [this, status]() {
                awsStatus_ = status;
                awsValidationRunning_ = false;
                emit awsValidationComplete(status);
            }, Qt::QueuedConnection);
            return;
        }
        status.cliInstalled = true;
        
        // Step 2: Validate credentials
        QProcess checkCreds;
        checkCreds.start("aws", QStringList() << "sts" << "get-caller-identity" << "--output" << "json");
        checkCreds.waitForFinished(15000);
        if (checkCreds.exitCode() != 0) {
            status.credentialsValid = false;
            status.errorMessage = "Credentials invalid";
            QMetaObject::invokeMethod(this, [this, status]() {
                awsStatus_ = status;
                awsValidationRunning_ = false;
                emit awsValidationComplete(status);
            }, Qt::QueuedConnection);
            return;
        }
        
        QString credsOutput = QString::fromUtf8(checkCreds.readAllStandardOutput());
        QJsonDocument doc = QJsonDocument::fromJson(credsOutput.toUtf8());
        if (doc.isObject()) {
            QJsonObject obj = doc.object();
            status.accountId = obj["Account"].toString();
            QString arn = obj["Arn"].toString();
            int lastSlash = arn.lastIndexOf('/');
            if (lastSlash >= 0) {
                status.userName = arn.mid(lastSlash + 1);
            }
        }
        status.credentialsValid = true;
        
        // Step 3: Check bucket access (STS succeeded so internet is available)
        if (!bucket.isEmpty()) {
            QProcess checkBucket;
            checkBucket.start("aws", QStringList() << "s3" << "ls"
                             << QString("s3://%1/").arg(bucket));
            checkBucket.waitForFinished(15000);
            if (checkBucket.exitCode() != 0) {
                QString err = QString::fromUtf8(checkBucket.readAllStandardError());
                status.bucketAccessible = false;
                status.errorMessage = QString("Cannot access bucket: %1").arg(err.trimmed());
            } else {
                status.bucketAccessible = true;
            }
        }
        
        // Step 4: Get region from config
        QProcess getRegion;
        getRegion.start("aws", QStringList() << "configure" << "get" << "region");
        getRegion.waitForFinished(5000);
        if (getRegion.exitCode() == 0) {
            status.region = QString::fromUtf8(getRegion.readAllStandardOutput()).trimmed();
        }
        
        QMetaObject::invokeMethod(this, [this, status]() {
            awsStatus_ = status;
            awsValidationRunning_ = false;
            emit awsValidationComplete(status);
        }, Qt::QueuedConnection);
    });
    
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

} // namespace f2c_cpp
