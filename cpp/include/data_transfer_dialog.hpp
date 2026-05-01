/**
 * @file data_transfer_dialog.hpp
 * @brief Dialog for downloading data from robot
 * 
 * Features:
 * - Browse available dates and sections
 * - Tree view with section/sub-folder selection
 * - Progress tracking with speed display
 * - Download queue management (uTorrent-style)
 */

#pragma once

#include <QDialog>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QComboBox>
#include <QCalendarWidget>
#include <QProgressBar>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QGroupBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QTimer>
#include <QSet>

#include "transfer_manager.hpp"

namespace f2c_cpp {

// =============================================================================
// Section Tree Item (custom tree widget item with metadata)
// =============================================================================

class SectionTreeItem : public QTreeWidgetItem {
public:
    enum ItemType {
        SectionItem,
        SubFolderItem
    };
    
    explicit SectionTreeItem(QTreeWidget* parent, const SectionInfo& section);
    explicit SectionTreeItem(QTreeWidgetItem* parent, const SubFolderInfo& subFolder,
                            const QString& sectionPath);
    
    ItemType itemType() const { return itemType_; }
    QString sectionPath() const { return sectionPath_; }
    QString subFolderName() const { return subFolderName_; }
    qint64 sizeBytes() const { return sizeBytes_; }
    
    void setDownloaded(bool downloaded);
    bool isDownloaded() const { return downloaded_; }
    
    void setInProgress(bool inProgress);
    bool isInProgress() const { return inProgress_; }
    
    // Update selection state based on children
    void updateSelectionFromChildren();
    
private:
    ItemType itemType_;
    QString sectionPath_;
    QString subFolderName_;
    qint64 sizeBytes_ = 0;
    bool downloaded_ = false;
    bool inProgress_ = false;
};

// =============================================================================
// Data Transfer Dialog
// =============================================================================

class DataTransferDialog : public QDialog {
    Q_OBJECT

public:
    explicit DataTransferDialog(QWidget* parent = nullptr);
    ~DataTransferDialog() override;
    
    // Configuration
    void setRobotHost(const QString& host);
    void setRobotUser(const QString& user);
    void setDataPath(const QString& path);
    void setDefaultDestination(const QString& path);
    
signals:
    // Emitted when there's an active transfer (for main window badge)
    void transferActive(bool active);
    void transferProgress(int percent, double speedMBps);

public slots:
    void refreshData();
    void bringToFront();

protected:
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private slots:
    // UI actions
    void onDateChanged();
    void onSelectAll();
    void onDeselectAll();
    void onBrowseDestination();
    void onDownloadClicked();
    void onCancelClicked();
    void onItemChanged(QTreeWidgetItem* item, int column);
    void onItemExpanded(QTreeWidgetItem* item);
    
    // Transfer manager callbacks
    void onConnectionStatus(bool connected, const QString& message);
    void onDatesAvailable(const QStringList& dates);
    void onSectionsAvailable(const QString& date, const QList<SectionInfo>& sections);
    void onSectionDetailsReady(const SectionInfo& section);
    void onQueryError(const QString& operation, const QString& error);
    
    void onProgressUpdated(int jobId, qint64 transferred, qint64 total,
                           double speedMBps, int percent, const QString& currentFile);
    void onJobStateChanged(int jobId, TransferState newState);
    void onJobCompleted(int jobId, bool success, const QString& message);
    void onQueueChanged();
    
    // Timer callbacks
    void onRefreshTimer();

private:
    void setupUI();
    void setupConnections();
    void updateConnectionStatus(bool connected, const QString& msg);
    void updateSelectionSummary();
    void populateDateCombo(const QStringList& dates);
    void populateSectionTree(const QList<SectionInfo>& sections);
    void updateProgressUI();
    void showQueueConflictDialog();
    void startDownload(bool cancelCurrent);
    void loadSettings();
    void saveSettings();
    
    // Get currently selected items for download
    struct DownloadSelection {
        QString sectionPath;
        QString sectionName;
        QStringList subFolders;
        qint64 totalSize;
    };
    QList<DownloadSelection> getSelectedDownloads() const;
    
    // UI Elements - Header
    QLabel* lblConnectionStatus_ = nullptr;
    QPushButton* btnRefresh_ = nullptr;
    QComboBox* comboDate_ = nullptr;
    
    // UI Elements - Tree
    QTreeWidget* treeWidget_ = nullptr;
    QPushButton* btnSelectAll_ = nullptr;
    QPushButton* btnDeselectAll_ = nullptr;
    
    // UI Elements - Summary
    QLabel* lblSelectionSummary_ = nullptr;
    QLineEdit* txtDestination_ = nullptr;
    QPushButton* btnBrowse_ = nullptr;
    
    // UI Elements - Progress
    QGroupBox* groupProgress_ = nullptr;
    QLabel* lblCurrentFile_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QLabel* lblProgressDetail_ = nullptr;
    QLabel* lblQueueStatus_ = nullptr;
    QPushButton* btnCancel_ = nullptr;
    
    // UI Elements - Actions
    QPushButton* btnDownload_ = nullptr;
    QPushButton* btnClose_ = nullptr;
    
    // State
    QString robotHost_;
    QString robotUser_;
    QString dataPath_ = "/R_DATA";
    QString defaultDestination_;
    QString currentDate_;
    
    QList<SectionInfo> currentSections_;
    QSet<QString> expandedSections_;  // Track which sections have been expanded
    
    bool connected_ = false;
    QTimer* refreshTimer_ = nullptr;
    
    // Prevent re-entrancy during programmatic checkbox changes
    bool updatingItems_ = false;
};

// =============================================================================
// Mini Progress Widget (for embedding in main window)
// =============================================================================

class TransferProgressWidget : public QWidget {
    Q_OBJECT

public:
    explicit TransferProgressWidget(QWidget* parent = nullptr);
    
    void setProgress(int percent, double speedMBps, qint64 bytesTransferred, 
                     qint64 totalBytes, const QString& sectionName);
    
signals:
    void showDialogRequested();
    void cancelRequested();

private:
    QString formatBytes(qint64 bytes) const;
    
    QProgressBar* progressBar_ = nullptr;
    QLabel* lblBytes_ = nullptr;      // Shows "1.5 GB / 3.0 GB"
    QLabel* lblSpeed_ = nullptr;      // Shows speed
    QLabel* lblSection_ = nullptr;    // Shows section name
    QPushButton* btnShow_ = nullptr;
    QPushButton* btnCancel_ = nullptr;
};

} // namespace f2c_cpp
