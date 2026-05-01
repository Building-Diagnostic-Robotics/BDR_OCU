/**
 * @file cloud_upload_dialog.hpp
 * @brief Dialog for uploading data from laptop to AWS S3
 * 
 * Features:
 * - Network status indicator
 * - AWS connection validation
 * - Browse local downloaded sections
 * - Checkbox tree for section selection
 * - Upload progress with speed/ETA
 * - AWS settings configuration
 * - Reverse geocoding display
 */

#pragma once

#include <QDialog>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QProgressBar>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QComboBox>
#include <QGroupBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QTimer>
#include <QSpinBox>

#include <QThread>

#include "cloud_upload_manager.hpp"
#include "network_monitor.hpp"
#include "scan_session_tracker.hpp"

namespace f2c_cpp {

// =============================================================================
// AWS Settings Dialog
// =============================================================================

class AwsSettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit AwsSettingsDialog(const AwsConfig& config, QWidget* parent = nullptr);
    
    AwsConfig getConfig() const;

private slots:
    void onTestConnection();
    void onTestComplete(const AwsStatus& status);

private:
    void setupUI();
    
    QLineEdit* txtBucket_ = nullptr;
    QLineEdit* txtPrefix_ = nullptr;
    QLineEdit* txtOperator_ = nullptr;
    QComboBox* comboRegion_ = nullptr;
    QComboBox* comboStorageClass_ = nullptr;
    QSpinBox* spinConcurrency_ = nullptr;
    QPushButton* btnTest_ = nullptr;
    QLabel* lblTestResult_ = nullptr;
    
    NetworkMonitor* testMonitor_ = nullptr;
};

// =============================================================================
// Cloud Upload Dialog
// =============================================================================

// Data passed from background scan thread to UI thread
struct CloudSubInfo { QString name; qint64 size = 0; int count = 0; };
struct SectionUIData {
    CloudUploadManager::LocalSection sec;
    QList<CloudSubInfo> subs;
};

class CloudUploadDialog : public QDialog {
    Q_OBJECT

public:
    explicit CloudUploadDialog(ScanSessionTracker* tracker, QWidget* parent = nullptr);
    ~CloudUploadDialog() override;
    
    void setLocalDataPath(const QString& path);
    void bringToFront();
    
    // Call when this tab becomes visible/hidden
    void activate();
    void deactivate();
    
signals:
    void uploadActive(bool active);

public slots:
    void refreshSections();

protected:
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private slots:
    // UI actions
    void onSelectAll();
    void onDeselectAll();
    void onUploadClicked();
    void onPauseClicked();
    void onCancelClicked();
    void onItemChanged(QTreeWidgetItem* item, int column);
    void onCurrentItemChanged(QTreeWidgetItem* current, QTreeWidgetItem* previous);
    
    // Network monitor callbacks
    void onNetworkStatusUpdated(const NetworkStatus& status);
    void onAwsValidationComplete(const AwsStatus& status);
    
    // Upload manager callbacks
    void onUploadProgress(int jobId, qint64 uploaded, qint64 total,
                         double speedMBps, int percent);
    void onUploadStateChanged(int jobId, UploadState state);
    void onUploadCompleted(int jobId, bool success, const QString& message);
    void onQueueChanged();
    
    // Geocoding
    void onGeocodeResult(const QString& address);
    
    // Verification (cloud existence check)
    void onUploadVerified(const QString& sectionPath, bool existsInCloud);
    
    // Refresh timer
    void onRefreshTimer();

private:
    void setupUI();
    void setupConnections();
    void updateNetworkIndicator();
    void updateAwsIndicator();
    void updateSelectionSummary();
    void updateProgressUI();
    void populateTree(const QList<SectionUIData>& uiData);
    void loadSettings();
    void saveSettings();
    
    QString formatBytes(qint64 bytes) const;
    QString formatDuration(int seconds) const;
    QString formatSpeed(double bytesPerSec) const;
    
    // UI - Network/AWS Status
    QLabel* lblNetworkIcon_ = nullptr;
    QLabel* lblNetworkStatus_ = nullptr;
    QLabel* lblAwsIcon_ = nullptr;
    QLabel* lblAwsStatus_ = nullptr;
    QLabel* lblLocalData_ = nullptr;
    
    // UI - Section tree
    QTreeWidget* treeWidget_ = nullptr;
    QPushButton* btnSelectAll_ = nullptr;
    QPushButton* btnDeselectAll_ = nullptr;
    QPushButton* btnRefresh_ = nullptr;
    
    // UI - Selection summary
    QLabel* lblSelectionSummary_ = nullptr;
    QLabel* lblEstTime_ = nullptr;
    
    // UI - Progress
    QGroupBox* groupProgress_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QLabel* lblProgressSection_ = nullptr;
    QLabel* lblProgressBytes_ = nullptr;
    QLabel* lblProgressSpeed_ = nullptr;
    QLabel* lblProgressEta_ = nullptr;
    QLabel* lblQueueStatus_ = nullptr;
    
    // UI - Actions
    QPushButton* btnUpload_ = nullptr;
    QPushButton* btnPause_ = nullptr;
    QPushButton* btnCancel_ = nullptr;
    // (AWS Settings button removed from UI)
    
    // UI - Location display
    QLabel* lblGpsCoords_ = nullptr;
    QLabel* lblAddress_ = nullptr;
    
    // State
    NetworkMonitor* networkMonitor_ = nullptr;
    ScanSessionTracker* sessionTracker_ = nullptr;
    QString localDataPath_;
    QTimer* refreshTimer_ = nullptr;
    bool updatingItems_ = false;
    bool refreshRunning_ = false;
    
    // Track geocoding state to avoid duplicate requests
    bool geocodeRequested_ = false;
    double geocodedLat_ = 0.0;
    double geocodedLon_ = 0.0;
};

} // namespace f2c_cpp
