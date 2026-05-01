/**
 * @file data_transfer_dialog.cpp
 * @brief Implementation of DataTransferDialog
 */

#include "data_transfer_dialog.hpp"

#include <QFileDialog>
#include "components/bdr_message_box.hpp"
#include <QHeaderView>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QDateTime>
#include <QApplication>
#include <QStyle>
#include <QDebug>

namespace f2c_cpp {

// =============================================================================
// SectionTreeItem Implementation
// =============================================================================

SectionTreeItem::SectionTreeItem(QTreeWidget* parent, const SectionInfo& section)
    : QTreeWidgetItem(parent)
    , itemType_(SectionItem)
    , sectionPath_(section.fullPath)
    , sizeBytes_(section.totalSizeBytes)
    , downloaded_(section.previouslyDownloaded)
    , inProgress_(section.isRecordingInProgress)
{
    // Column 0: Checkbox + Name
    setFlags(flags() | Qt::ItemIsUserCheckable);
    setCheckState(0, Qt::Unchecked);
    
    QString name = section.name;
    if (inProgress_) {
        name += " ⚠️";
    }
    setText(0, name);
    
    // Column 1: Size
    setText(1, formatFileSize(section.totalSizeBytes));
    
    // Column 2: Files
    setText(2, QString::number(section.fileCount));
    
    // Column 3: Timestamp
    setText(3, section.timestamp.toString("hh:mm:ss"));
    
    // Column 4: Data types
    QString types;
    if (section.hasVisualData) types += "✓V ";
    else types += "✗V ";
    if (section.hasGprData) types += "✓G ";
    else types += "✗G ";
    if (section.hasMap) types += "✓M ";
    else types += "✗M ";
    if (section.hasRosbag) types += "✓B ";
    else types += "✗B ";
    if (section.hasTiltCalib) types += "✓T";
    else types += "✗T";
    setText(4, types.trimmed());
    
    // Column 5: Status
    if (downloaded_) {
        setText(5, "Downloaded");
        setForeground(5, QBrush(QColor(40, 167, 69)));  // Green
    } else if (inProgress_) {
        setText(5, "Recording...");
        setForeground(5, QBrush(QColor(255, 193, 7)));  // Yellow/orange
    } else {
        setText(5, "");
    }
    
    // Make expandable (will populate sub-folders when expanded)
    setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    
    // Tooltip
    QString tooltip = QString("Path: %1\nSize: %2\nFiles: %3")
        .arg(section.fullPath)
        .arg(formatFileSize(section.totalSizeBytes))
        .arg(section.fileCount);
    if (inProgress_) {
        tooltip += "\n\n⚠️ Recording in progress - data may be incomplete";
    }
    setToolTip(0, tooltip);
}

SectionTreeItem::SectionTreeItem(QTreeWidgetItem* parent, const SubFolderInfo& subFolder,
                                 const QString& sectionPath)
    : QTreeWidgetItem(parent)
    , itemType_(SubFolderItem)
    , sectionPath_(sectionPath)
    , subFolderName_(subFolder.name)
    , sizeBytes_(subFolder.sizeBytes)
{
    setFlags(flags() | Qt::ItemIsUserCheckable);
    setCheckState(0, parent->checkState(0));  // Inherit parent state
    
    setText(0, subFolder.name);
    setText(1, formatFileSize(subFolder.sizeBytes));
    setText(2, QString::number(subFolder.fileCount));
}

void SectionTreeItem::setDownloaded(bool downloaded) {
    downloaded_ = downloaded;
    if (downloaded_) {
        setText(5, "Downloaded");
        setForeground(5, QBrush(QColor(40, 167, 69)));
    } else {
        setText(5, "");
        setForeground(5, QBrush());
    }
}

void SectionTreeItem::setInProgress(bool inProgress) {
    inProgress_ = inProgress;
    if (inProgress_) {
        setText(5, "Recording...");
        setForeground(5, QBrush(QColor(255, 193, 7)));
    } else if (!downloaded_) {
        setText(5, "");
        setForeground(5, QBrush());
    }
}

void SectionTreeItem::updateSelectionFromChildren() {
    if (childCount() == 0) return;
    
    int checkedCount = 0;
    int uncheckedCount = 0;
    
    for (int i = 0; i < childCount(); ++i) {
        if (child(i)->checkState(0) == Qt::Checked) {
            checkedCount++;
        } else {
            uncheckedCount++;
        }
    }
    
    if (checkedCount == childCount()) {
        setCheckState(0, Qt::Checked);
    } else if (uncheckedCount == childCount()) {
        setCheckState(0, Qt::Unchecked);
    } else {
        setCheckState(0, Qt::PartiallyChecked);
    }
}

// =============================================================================
// TransferProgressWidget Implementation
// =============================================================================

TransferProgressWidget::TransferProgressWidget(QWidget* parent)
    : QWidget(parent)
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 8, 6, 8);
    layout->setSpacing(4);
    
    // Section name label
    lblSection_ = new QLabel("Downloading...");
    lblSection_->setStyleSheet("font-weight: bold; font-size: 11px;");
    layout->addWidget(lblSection_);
    
    // Progress bar with percentage
    progressBar_ = new QProgressBar();
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    progressBar_->setTextVisible(true);
    progressBar_->setFormat("%p%");
    progressBar_->setFixedHeight(20);
    progressBar_->setStyleSheet(
        "QProgressBar { border: 1px solid #ccc; border-radius: 3px; text-align: center; }"
        "QProgressBar::chunk { background-color: #4CAF50; border-radius: 2px; }"
    );
    layout->addWidget(progressBar_);
    
    // Info row: bytes transferred / total, speed
    QHBoxLayout* infoLayout = new QHBoxLayout();
    infoLayout->setSpacing(8);
    
    lblBytes_ = new QLabel("0 B / 0 B");
    lblBytes_->setStyleSheet("font-size: 11px; color: #333;");
    infoLayout->addWidget(lblBytes_);
    
    lblSpeed_ = new QLabel("");
    lblSpeed_->setStyleSheet("font-size: 11px; color: #666;");
    infoLayout->addWidget(lblSpeed_);
    
    infoLayout->addStretch();
    layout->addLayout(infoLayout);
    
    // Button row
    QHBoxLayout* btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(6);
    
    btnShow_ = new QPushButton("📋 Details");
    btnShow_->setFixedHeight(26);
    btnShow_->setMinimumWidth(70);
    btnShow_->setToolTip("Show download details window");
    connect(btnShow_, &QPushButton::clicked, this, &TransferProgressWidget::showDialogRequested);
    btnLayout->addWidget(btnShow_);
    
    btnCancel_ = new QPushButton("✗ Cancel");
    btnCancel_->setFixedHeight(26);
    btnCancel_->setMinimumWidth(70);
    btnCancel_->setToolTip("Cancel current transfer");
    btnCancel_->setStyleSheet("QPushButton { color: #c00; }");
    connect(btnCancel_, &QPushButton::clicked, this, &TransferProgressWidget::cancelRequested);
    btnLayout->addWidget(btnCancel_);
    
    btnLayout->addStretch();
    layout->addLayout(btnLayout);
    
    setVisible(false);
}

QString TransferProgressWidget::formatBytes(qint64 bytes) const {
    if (bytes < 1024) return QString("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    if (bytes < 1024 * 1024 * 1024) return QString("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
    return QString("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}

void TransferProgressWidget::setProgress(int percent, double speedMBps, qint64 bytesTransferred, 
                                          qint64 totalBytes, const QString& sectionName) {
    progressBar_->setValue(percent);
    
    // Section name (truncated if long)
    QString name = sectionName;
    if (name.length() > 30) {
        name = name.left(27) + "...";
    }
    lblSection_->setText(name.isEmpty() ? "Downloading..." : name);
    
    // Bytes transferred / total
    if (totalBytes > 0) {
        lblBytes_->setText(QString("%1 / %2").arg(formatBytes(bytesTransferred), formatBytes(totalBytes)));
    } else {
        lblBytes_->setText(formatBytes(bytesTransferred));
    }
    
    // Speed
    if (speedMBps > 0) {
        lblSpeed_->setText(QString("@ %1 MB/s").arg(speedMBps, 0, 'f', 1));
    } else {
        lblSpeed_->setText("");
    }
}

// =============================================================================
// DataTransferDialog Implementation
// =============================================================================

DataTransferDialog::DataTransferDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("Download Robot Data");
    setMinimumSize(700, 550);
    setModal(false);
    
    setupUI();
    setupConnections();
    loadSettings();
    
    // Auto-refresh timer (every 30 seconds when visible)
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(30000);
    connect(refreshTimer_, &QTimer::timeout, this, &DataTransferDialog::onRefreshTimer);
}

DataTransferDialog::~DataTransferDialog() {
    saveSettings();
}

void DataTransferDialog::setupUI() {
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(8);
    
    // === Header Section ===
    QHBoxLayout* headerLayout = new QHBoxLayout();
    
    QLabel* lblRobot = new QLabel("Robot:");
    headerLayout->addWidget(lblRobot);
    
    lblConnectionStatus_ = new QLabel("Checking...");
    lblConnectionStatus_->setStyleSheet("font-weight: bold;");
    headerLayout->addWidget(lblConnectionStatus_);
    
    headerLayout->addStretch();
    
    btnRefresh_ = new QPushButton("⟳ Refresh");
    btnRefresh_->setToolTip("Refresh section list from robot");
    headerLayout->addWidget(btnRefresh_);
    
    mainLayout->addLayout(headerLayout);
    
    // === Date Selection ===
    QHBoxLayout* dateLayout = new QHBoxLayout();
    dateLayout->addWidget(new QLabel("Date:"));
    
    comboDate_ = new QComboBox();
    comboDate_->setMinimumWidth(200);
    comboDate_->addItem("Loading...");
    dateLayout->addWidget(comboDate_);
    
    dateLayout->addStretch();
    mainLayout->addLayout(dateLayout);
    
    // === Selection Buttons ===
    QHBoxLayout* selectLayout = new QHBoxLayout();
    
    btnSelectAll_ = new QPushButton("Select All");
    btnSelectAll_->setFixedWidth(80);
    selectLayout->addWidget(btnSelectAll_);
    
    btnDeselectAll_ = new QPushButton("Deselect All");
    btnDeselectAll_->setFixedWidth(90);
    selectLayout->addWidget(btnDeselectAll_);
    
    selectLayout->addStretch();
    mainLayout->addLayout(selectLayout);
    
    // === Section Tree ===
    treeWidget_ = new QTreeWidget();
    treeWidget_->setHeaderLabels({"Section", "Size", "Files", "Time", "Data Types", "Status"});
    treeWidget_->setRootIsDecorated(true);
    treeWidget_->setAlternatingRowColors(true);
    treeWidget_->setSelectionMode(QAbstractItemView::NoSelection);
    
    // Column widths
    treeWidget_->header()->setStretchLastSection(false);
    treeWidget_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    treeWidget_->setColumnWidth(1, 80);
    treeWidget_->setColumnWidth(2, 50);
    treeWidget_->setColumnWidth(3, 65);
    treeWidget_->setColumnWidth(4, 100);
    treeWidget_->setColumnWidth(5, 80);
    
    mainLayout->addWidget(treeWidget_, 1);
    
    // === Selection Summary ===
    QHBoxLayout* summaryLayout = new QHBoxLayout();
    
    lblSelectionSummary_ = new QLabel("Selected: 0 items (0 B)");
    summaryLayout->addWidget(lblSelectionSummary_);
    
    summaryLayout->addStretch();
    mainLayout->addLayout(summaryLayout);
    
    // === Destination ===
    QHBoxLayout* destLayout = new QHBoxLayout();
    destLayout->addWidget(new QLabel("Destination:"));
    
    txtDestination_ = new QLineEdit();
    txtDestination_->setPlaceholderText("Select download location...");
    destLayout->addWidget(txtDestination_, 1);
    
    btnBrowse_ = new QPushButton("Browse...");
    destLayout->addWidget(btnBrowse_);
    
    mainLayout->addLayout(destLayout);
    
    // === Progress Section ===
    groupProgress_ = new QGroupBox("Transfer Progress");
    QVBoxLayout* progressLayout = new QVBoxLayout(groupProgress_);
    
    lblCurrentFile_ = new QLabel("No active transfer");
    lblCurrentFile_->setStyleSheet("color: #666;");
    progressLayout->addWidget(lblCurrentFile_);
    
    progressBar_ = new QProgressBar();
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    progressBar_->setTextVisible(true);
    progressLayout->addWidget(progressBar_);
    
    QHBoxLayout* progressDetailLayout = new QHBoxLayout();
    lblProgressDetail_ = new QLabel("0 B / 0 B");
    progressDetailLayout->addWidget(lblProgressDetail_);
    
    progressDetailLayout->addStretch();
    
    lblQueueStatus_ = new QLabel("");
    progressDetailLayout->addWidget(lblQueueStatus_);
    
    btnCancel_ = new QPushButton("Cancel");
    btnCancel_->setEnabled(false);
    progressDetailLayout->addWidget(btnCancel_);
    
    progressLayout->addLayout(progressDetailLayout);
    mainLayout->addWidget(groupProgress_);
    
    // === Action Buttons ===
    QHBoxLayout* actionLayout = new QHBoxLayout();
    actionLayout->addStretch();
    
    btnDownload_ = new QPushButton("📥 Download Selected");
    btnDownload_->setMinimumWidth(140);
    btnDownload_->setEnabled(false);
    actionLayout->addWidget(btnDownload_);
    
    // (Close button removed - dialog is now a tab in the Data Transfer window)
    
    mainLayout->addLayout(actionLayout);
}

void DataTransferDialog::setupConnections() {
    // UI connections
    connect(btnRefresh_, &QPushButton::clicked, this, &DataTransferDialog::refreshData);
    connect(comboDate_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DataTransferDialog::onDateChanged);
    connect(btnSelectAll_, &QPushButton::clicked, this, &DataTransferDialog::onSelectAll);
    connect(btnDeselectAll_, &QPushButton::clicked, this, &DataTransferDialog::onDeselectAll);
    connect(btnBrowse_, &QPushButton::clicked, this, &DataTransferDialog::onBrowseDestination);
    connect(btnDownload_, &QPushButton::clicked, this, &DataTransferDialog::onDownloadClicked);
    connect(btnCancel_, &QPushButton::clicked, this, &DataTransferDialog::onCancelClicked);
    // (Close button removed - dialog is now a tab)
    
    connect(treeWidget_, &QTreeWidget::itemChanged, this, &DataTransferDialog::onItemChanged);
    connect(treeWidget_, &QTreeWidget::itemExpanded, this, &DataTransferDialog::onItemExpanded);
    
    // Transfer manager connections
    TransferManager& tm = TransferManager::instance();
    
    connect(&tm, &TransferManager::connectionStatus,
            this, &DataTransferDialog::onConnectionStatus);
    connect(&tm, &TransferManager::datesAvailable,
            this, &DataTransferDialog::onDatesAvailable);
    connect(&tm, &TransferManager::sectionsAvailable,
            this, &DataTransferDialog::onSectionsAvailable);
    connect(&tm, &TransferManager::sectionDetailsReady,
            this, &DataTransferDialog::onSectionDetailsReady);
    connect(&tm, &TransferManager::queryError,
            this, &DataTransferDialog::onQueryError);
    connect(&tm, &TransferManager::progressUpdated,
            this, &DataTransferDialog::onProgressUpdated);
    connect(&tm, &TransferManager::jobStateChanged,
            this, &DataTransferDialog::onJobStateChanged);
    connect(&tm, &TransferManager::jobCompleted,
            this, &DataTransferDialog::onJobCompleted);
    connect(&tm, &TransferManager::queueChanged,
            this, &DataTransferDialog::onQueueChanged);
}

void DataTransferDialog::setRobotHost(const QString& host) {
    robotHost_ = host;
    TransferManager::instance().setRobotHost(host);
}

void DataTransferDialog::setRobotUser(const QString& user) {
    robotUser_ = user;
    TransferManager::instance().setRobotUser(user);
}

void DataTransferDialog::setDataPath(const QString& path) {
    dataPath_ = path;
}

void DataTransferDialog::setDefaultDestination(const QString& path) {
    defaultDestination_ = path;
    txtDestination_->setText(path);
}

void DataTransferDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    
    // Start connection check and data refresh
    TransferManager::instance().setRobotHost(robotHost_);
    TransferManager::instance().setRobotUser(robotUser_);
    
    refreshData();
    refreshTimer_->start();
}

void DataTransferDialog::closeEvent(QCloseEvent* event) {
    refreshTimer_->stop();
    saveSettings();
    QDialog::closeEvent(event);
}

void DataTransferDialog::refreshData() {
    qDebug() << "[DataTransfer] refreshData() called, dataPath:" << dataPath_;
    qDebug() << "[DataTransfer] robotHost:" << robotHost_ << "robotUser:" << robotUser_;
    
    updateConnectionStatus(false, "Connecting...");
    
    // Only fetch dates - if successful, we're connected
    // (checkConnection and fetchDates use the same process, can't run simultaneously)
    TransferManager& tm = TransferManager::instance();
    tm.fetchAvailableDates(dataPath_);
}

void DataTransferDialog::bringToFront() {
    show();
    raise();
    activateWindow();
}

void DataTransferDialog::onDateChanged() {
    if (comboDate_->currentIndex() < 0) return;
    
    currentDate_ = comboDate_->currentText();
    if (currentDate_.isEmpty() || currentDate_ == "Loading..." || 
        currentDate_ == "No data found") {
        return;
    }
    
    treeWidget_->clear();
    expandedSections_.clear();
    
    TransferManager::instance().fetchSectionsForDate(dataPath_, currentDate_);
}

void DataTransferDialog::onSelectAll() {
    updatingItems_ = true;
    
    for (int i = 0; i < treeWidget_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = treeWidget_->topLevelItem(i);
        item->setCheckState(0, Qt::Checked);
        
        // Also check all children
        for (int j = 0; j < item->childCount(); ++j) {
            item->child(j)->setCheckState(0, Qt::Checked);
        }
    }
    
    updatingItems_ = false;
    updateSelectionSummary();
}

void DataTransferDialog::onDeselectAll() {
    updatingItems_ = true;
    
    for (int i = 0; i < treeWidget_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = treeWidget_->topLevelItem(i);
        item->setCheckState(0, Qt::Unchecked);
        
        for (int j = 0; j < item->childCount(); ++j) {
            item->child(j)->setCheckState(0, Qt::Unchecked);
        }
    }
    
    updatingItems_ = false;
    updateSelectionSummary();
}

void DataTransferDialog::onBrowseDestination() {
    QString startDir = txtDestination_->text();
    if (startDir.isEmpty()) {
        startDir = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    }
    
    QString dir = QFileDialog::getExistingDirectory(
        this, "Select Download Destination", startDir,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
    );
    
    if (!dir.isEmpty()) {
        // Append date folder to destination
        QString fullPath = dir;
        if (!currentDate_.isEmpty()) {
            fullPath = QString("%1/%2").arg(dir, currentDate_);
        }
        txtDestination_->setText(fullPath);
        defaultDestination_ = dir;
        saveSettings();
    }
}

void DataTransferDialog::onDownloadClicked() {
    QList<DownloadSelection> selections = getSelectedDownloads();
    
    if (selections.isEmpty()) {
        BdrMessageBox::information(this, "No Selection", 
                                 "Please select at least one section to download.");
        return;
    }
    
    QString destination = txtDestination_->text();
    if (destination.isEmpty()) {
        onBrowseDestination();
        destination = txtDestination_->text();
        if (destination.isEmpty()) return;
    }
    
    // Check if there's an active transfer
    TransferManager& tm = TransferManager::instance();
    
    if (tm.hasActiveTransfer()) {
        showQueueConflictDialog();
        return;
    }
    
    startDownload(false);
}

void DataTransferDialog::onCancelClicked() {
    TransferManager::instance().cancelCurrentJob();
}

void DataTransferDialog::showQueueConflictDialog() {
    const int result = BdrMessageBox::custom(
        this,
        "Download in Progress",
        "A download is currently in progress.",
        {"Queue", "Cancel Current & Start New", "Cancel"},
        -1,
        "What would you like to do?");
    if (result == 0) {
        startDownload(false);
    } else if (result == 1) {
        startDownload(true);
    }
}

void DataTransferDialog::startDownload(bool cancelCurrent) {
    qDebug() << "[DataTransfer] startDownload() called, cancelCurrent:" << cancelCurrent;
    
    TransferManager& tm = TransferManager::instance();
    
    if (cancelCurrent) {
        tm.cancelCurrentJob();
    }
    
    QList<DownloadSelection> selections = getSelectedDownloads();
    QString baseDestination = txtDestination_->text();
    
    qDebug() << "[DataTransfer] Selections count:" << selections.size();
    qDebug() << "[DataTransfer] Base destination:" << baseDestination;
    
    for (const DownloadSelection& sel : selections) {
        TransferJob job;
        job.sectionPath = sel.sectionPath;
        job.sectionName = sel.sectionName;
        job.subFolders = sel.subFolders;
        job.totalBytes = sel.totalSize;
        job.localDestination = QString("%1/%2").arg(baseDestination, sel.sectionName);
        
        qDebug() << "[DataTransfer] Enqueuing job:" << job.sectionPath << "->" << job.localDestination;
        
        int jobId = tm.enqueueJob(job);
        qDebug() << "[DataTransfer] Job enqueued with ID:" << jobId;
    }
    
    emit transferActive(true);
    qDebug() << "[DataTransfer] transferActive(true) emitted";
}

void DataTransferDialog::onItemChanged(QTreeWidgetItem* item, int column) {
    if (updatingItems_ || column != 0) return;
    
    updatingItems_ = true;
    
    SectionTreeItem* sectionItem = dynamic_cast<SectionTreeItem*>(item);
    if (!sectionItem) {
        updatingItems_ = false;
        return;
    }
    
    if (sectionItem->itemType() == SectionTreeItem::SectionItem) {
        // Parent item changed - propagate to children
        Qt::CheckState state = item->checkState(0);
        for (int i = 0; i < item->childCount(); ++i) {
            item->child(i)->setCheckState(0, state);
        }
    } else {
        // Child item changed - update parent
        QTreeWidgetItem* parent = item->parent();
        if (parent) {
            SectionTreeItem* parentSection = dynamic_cast<SectionTreeItem*>(parent);
            if (parentSection) {
                parentSection->updateSelectionFromChildren();
            }
        }
    }
    
    updatingItems_ = false;
    updateSelectionSummary();
}

void DataTransferDialog::onItemExpanded(QTreeWidgetItem* item) {
    SectionTreeItem* sectionItem = dynamic_cast<SectionTreeItem*>(item);
    if (!sectionItem || sectionItem->itemType() != SectionTreeItem::SectionItem) {
        return;
    }
    
    QString path = sectionItem->sectionPath();
    
    // Only fetch details once per section
    if (expandedSections_.contains(path)) {
        return;
    }
    expandedSections_.insert(path);
    
    // Fetch detailed sub-folder info
    TransferManager::instance().fetchSectionDetails(path);
}

// =============================================================================
// Transfer Manager Callbacks
// =============================================================================

void DataTransferDialog::onConnectionStatus(bool connected, const QString& message) {
    connected_ = connected;
    updateConnectionStatus(connected, message);
}

void DataTransferDialog::onDatesAvailable(const QStringList& dates) {
    // If we received dates, we're connected
    connected_ = true;
    updateConnectionStatus(true, "Connected");
    populateDateCombo(dates);
}

void DataTransferDialog::onSectionsAvailable(const QString& date, const QList<SectionInfo>& sections) {
    if (date != currentDate_) return;
    
    // If we received sections, we're connected
    connected_ = true;
    
    currentSections_ = sections;
    populateSectionTree(sections);
    updateConnectionStatus(true, QString("Connected (%1 sections)").arg(sections.size()));
}

void DataTransferDialog::onSectionDetailsReady(const SectionInfo& section) {
    // Find the corresponding tree item and populate children
    for (int i = 0; i < treeWidget_->topLevelItemCount(); ++i) {
        SectionTreeItem* item = dynamic_cast<SectionTreeItem*>(treeWidget_->topLevelItem(i));
        if (item && item->sectionPath() == section.fullPath) {
            // Remove placeholder children if any
            while (item->childCount() > 0) {
                delete item->takeChild(0);
            }
            
            // Add sub-folder items
            updatingItems_ = true;
            for (const SubFolderInfo& sf : section.subFolders) {
                new SectionTreeItem(item, sf, section.fullPath);
            }
            updatingItems_ = false;
            
            break;
        }
    }
}

void DataTransferDialog::onQueryError(const QString& operation, const QString& error) {
    qDebug() << "Query error:" << operation << "-" << error;
    
    if (operation == "fetchDates") {
        connected_ = false;
        comboDate_->clear();
        comboDate_->addItem("No data found");
        updateConnectionStatus(false, "Error: " + error);
    } else if (operation == "fetchSections") {
        // Connection might still be OK, just this query failed
        updateConnectionStatus(connected_, "Error loading sections: " + error);
    }
}

void DataTransferDialog::onProgressUpdated(int /*jobId*/, qint64 transferred, qint64 total,
                                           double speedMBps, int percent, 
                                           const QString& currentFile) {
    progressBar_->setValue(percent);
    
    QString progressText = QString("%1 / %2   %3 MB/s")
        .arg(formatFileSize(transferred))
        .arg(formatFileSize(total))
        .arg(speedMBps, 0, 'f', 1);
    lblProgressDetail_->setText(progressText);
    
    if (!currentFile.isEmpty()) {
        QString fileName = currentFile;
        if (fileName.length() > 50) {
            fileName = "..." + fileName.right(47);
        }
        lblCurrentFile_->setText(fileName);
    }
    
    emit transferProgress(percent, speedMBps);
}

void DataTransferDialog::onJobStateChanged(int /*jobId*/, TransferState newState) {
    btnCancel_->setEnabled(newState == TransferState::Running);
    
    QString stateText;
    switch (newState) {
        case TransferState::Queued: stateText = "Queued"; break;
        case TransferState::Running: stateText = "Transferring..."; break;
        case TransferState::Paused: stateText = "Paused"; break;
        case TransferState::Completed: stateText = "Completed"; break;
        case TransferState::Failed: stateText = "Failed"; break;
        case TransferState::Cancelled: stateText = "Cancelled"; break;
        case TransferState::Verifying: stateText = "Verifying..."; break;
    }
    
    lblCurrentFile_->setText(stateText);
}

void DataTransferDialog::onJobCompleted(int /*jobId*/, bool success, const QString& message) {
    if (success) {
        // Refresh to update downloaded status
        onDateChanged();
    }
    
    // Update UI
    updateProgressUI();
    
    // Check if any more transfers
    TransferManager& tm = TransferManager::instance();
    bool hasActive = tm.hasActiveTransfer() || tm.hasQueuedJobs();
    emit transferActive(hasActive);
    
    if (!hasActive) {
        if (success) {
            lblCurrentFile_->setText("All transfers complete");
        } else {
            lblCurrentFile_->setText("Transfer failed: " + message);
        }
        progressBar_->setValue(0);
        btnCancel_->setEnabled(false);
    }
}

void DataTransferDialog::onQueueChanged() {
    TransferManager& tm = TransferManager::instance();
    int queueSize = tm.queueSize();
    
    if (queueSize > 0) {
        lblQueueStatus_->setText(QString("Queue: %1 pending").arg(queueSize));
    } else {
        lblQueueStatus_->setText("");
    }
}

void DataTransferDialog::onRefreshTimer() {
    if (isVisible() && connected_) {
        // Soft refresh - just update connection status
        TransferManager::instance().checkConnection();
    }
}

// =============================================================================
// Helper Methods
// =============================================================================

void DataTransferDialog::updateConnectionStatus(bool connected, const QString& msg) {
    if (connected) {
        lblConnectionStatus_->setText(QString("%1 ● %2").arg(robotHost_, msg));
        lblConnectionStatus_->setStyleSheet("font-weight: bold; color: #28a745;");
    } else {
        lblConnectionStatus_->setText(QString("%1 ○ %2").arg(robotHost_, msg));
        lblConnectionStatus_->setStyleSheet("font-weight: bold; color: #dc3545;");
    }
    
    btnDownload_->setEnabled(connected);
}

void DataTransferDialog::updateSelectionSummary() {
    QList<DownloadSelection> selections = getSelectedDownloads();
    
    qint64 totalSize = 0;
    int itemCount = 0;
    
    for (const DownloadSelection& sel : selections) {
        totalSize += sel.totalSize;
        itemCount++;
    }
    
    lblSelectionSummary_->setText(QString("Selected: %1 items (%2)")
        .arg(itemCount)
        .arg(formatFileSize(totalSize)));
    
    btnDownload_->setEnabled(itemCount > 0 && connected_);
}

void DataTransferDialog::populateDateCombo(const QStringList& dates) {
    comboDate_->blockSignals(true);
    comboDate_->clear();
    
    if (dates.isEmpty()) {
        comboDate_->addItem("No data found");
    } else {
        for (const QString& date : dates) {
            comboDate_->addItem(date);
        }
        
        // Select today's date if available
        QString today = QDate::currentDate().toString("MMMM_d_yyyy");
        int todayIndex = comboDate_->findText(today);
        if (todayIndex >= 0) {
            comboDate_->setCurrentIndex(todayIndex);
        }
    }
    
    comboDate_->blockSignals(false);
    
    // Trigger date change to load sections
    onDateChanged();
}

void DataTransferDialog::populateSectionTree(const QList<SectionInfo>& sections) {
    treeWidget_->clear();
    
    updatingItems_ = true;
    
    for (const SectionInfo& section : sections) {
        new SectionTreeItem(treeWidget_, section);
    }
    
    updatingItems_ = false;
    
    updateSelectionSummary();
}

void DataTransferDialog::updateProgressUI() {
    TransferManager& tm = TransferManager::instance();
    auto job = tm.currentJob();
    
    if (job) {
        progressBar_->setValue(job->progressPercent);
        lblProgressDetail_->setText(QString("%1 / %2")
            .arg(formatFileSize(job->transferredBytes))
            .arg(formatFileSize(job->totalBytes)));
    }
}

QList<DataTransferDialog::DownloadSelection> DataTransferDialog::getSelectedDownloads() const {
    QList<DownloadSelection> result;
    
    for (int i = 0; i < treeWidget_->topLevelItemCount(); ++i) {
        SectionTreeItem* item = dynamic_cast<SectionTreeItem*>(treeWidget_->topLevelItem(i));
        if (!item) continue;
        
        if (item->checkState(0) == Qt::Unchecked) {
            continue;
        }
        
        DownloadSelection sel;
        sel.sectionPath = item->sectionPath();
        sel.sectionName = item->text(0).remove(" ⚠️");
        
        if (item->checkState(0) == Qt::Checked || item->childCount() == 0) {
            // Full section selected or no sub-folders loaded yet
            sel.totalSize = item->sizeBytes();
        } else {
            // Partially selected - get selected children
            for (int j = 0; j < item->childCount(); ++j) {
                SectionTreeItem* child = dynamic_cast<SectionTreeItem*>(item->child(j));
                if (child && child->checkState(0) == Qt::Checked) {
                    sel.subFolders << child->subFolderName();
                    sel.totalSize += child->sizeBytes();
                }
            }
        }
        
        if (sel.totalSize > 0 || sel.subFolders.isEmpty()) {
            result.append(sel);
        }
    }
    
    return result;
}

void DataTransferDialog::loadSettings() {
    QSettings settings("PilotControl", "BDRCoveragePlanner");
    
    defaultDestination_ = settings.value("data_transfer/default_destination",
        QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + "/robot_data"
    ).toString();
    
    txtDestination_->setText(defaultDestination_);
}

void DataTransferDialog::saveSettings() {
    QSettings settings("PilotControl", "BDRCoveragePlanner");
    settings.setValue("data_transfer/default_destination", defaultDestination_);
}

} // namespace f2c_cpp
