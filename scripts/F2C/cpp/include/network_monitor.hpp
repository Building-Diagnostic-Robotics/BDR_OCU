/**
 * @file network_monitor.hpp
 * @brief Monitors internet connectivity and AWS availability
 * 
 * Features:
 * - Periodic ping-based connectivity checks
 * - Latency and stability tracking over a rolling window
 * - AWS CLI and credential validation
 * - Bandwidth estimation (optional, on-demand)
 */

#pragma once

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QThread>
#include <QDateTime>
#include <QList>

namespace f2c_cpp {

/**
 * @brief Snapshot of current network state
 */
struct NetworkStatus {
    bool internetAvailable = false;
    int latencyMs = -1;
    double packetLossPercent = 100.0;
    bool isStable = false;         // Low loss + consistent latency over window
    QDateTime lastCheck;
    QString statusMessage;
};

/**
 * @brief Snapshot of AWS configuration state
 */
struct AwsStatus {
    bool cliInstalled = false;
    bool credentialsValid = false;
    bool bucketAccessible = false;
    QString accountId;
    QString userName;
    QString region;
    QString bucketName;
    QString errorMessage;
};

/**
 * @brief Monitors internet and AWS connectivity
 * 
 * Runs periodic ping checks and provides signals for connectivity changes.
 * AWS validation is done on-demand (not periodic).
 */
class NetworkMonitor : public QObject {
    Q_OBJECT

public:
    explicit NetworkMonitor(QObject* parent = nullptr);
    ~NetworkMonitor() override;
    
    // Start/stop monitoring
    void startMonitoring(int intervalMs = 8000);
    void stopMonitoring();
    bool isMonitoring() const { return monitorTimer_ && monitorTimer_->isActive(); }
    
    // Current state
    NetworkStatus networkStatus() const { return currentStatus_; }
    AwsStatus awsStatus() const { return awsStatus_; }
    
    bool isOnline() const { return currentStatus_.internetAvailable; }
    bool isStable() const { return currentStatus_.isStable; }
    int latencyMs() const { return currentStatus_.latencyMs; }
    
    // On-demand checks
    void checkNow();
    void validateAwsConfig(const QString& bucketName);
    
    // Configuration
    void setPingTarget(const QString& target) { pingTarget_ = target; }
    void setStabilityWindow(int samples) { stabilityWindowSize_ = samples; }
    void setStabilityMaxLossPercent(double pct) { stabilityMaxLoss_ = pct; }
    
signals:
    void connectivityChanged(bool online);
    void networkStatusUpdated(const NetworkStatus& status);
    void awsValidationComplete(const AwsStatus& status);

private slots:
    void onPingFinished(int exitCode, QProcess::ExitStatus status);
    void performCheck();

private:
    void parsePingOutput(const QString& output, int exitCode);
    void updateStability();
    
    // Ping state
    QProcess* pingProcess_ = nullptr;
    QTimer* monitorTimer_ = nullptr;
    QString pingTarget_ = "1.1.1.1";
    bool checkInProgress_ = false;
    
    // Current status
    NetworkStatus currentStatus_;
    AwsStatus awsStatus_;
    bool lastOnlineState_ = false;
    bool awsValidationRunning_ = false;
    
    // Stability tracking (rolling window)
    struct PingSample {
        int latencyMs = -1;
        bool success = false;
        QDateTime timestamp;
    };
    QList<PingSample> pingSamples_;
    int stabilityWindowSize_ = 6;       // Number of samples for stability calc
    double stabilityMaxLoss_ = 20.0;    // Max loss % to be "stable"
};

} // namespace f2c_cpp
