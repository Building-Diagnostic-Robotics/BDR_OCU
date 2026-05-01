/**
 * @file cloud_upload_dialog.cpp
 * @brief Implementation of CloudUploadDialog and AwsSettingsDialog
 */

#include "cloud_upload_dialog.hpp"
#include <QMessageBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QApplication>
#include <QStyle>
#include <QDir>
#include <QDirIterator>
#include <QDebug>

namespace f2c_cpp {

// =============================================================================
// AwsSettingsDialog
// =============================================================================

AwsSettingsDialog::AwsSettingsDialog(const AwsConfig& config, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("AWS S3 Settings");
    setMinimumWidth(420);
    setupUI();
    
    // Populate from config
    txtBucket_->setText(config.bucketName);
    txtPrefix_->setText(config.prefix);
    txtOperator_->setText(config.operatorName);
    spinConcurrency_->setValue(config.maxConcurrentRequests);
    
    // Set region combo
    int regionIdx = comboRegion_->findText(config.region);
    if (regionIdx >= 0) comboRegion_->setCurrentIndex(regionIdx);
    
    // Set storage class combo
    int storageIdx = comboStorageClass_->findData(config.storageClass);
    if (storageIdx >= 0) comboStorageClass_->setCurrentIndex(storageIdx);
}

void AwsSettingsDialog::setupUI() {
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    
    QFormLayout* form = new QFormLayout();
    
    txtBucket_ = new QLineEdit();
    txtBucket_->setPlaceholderText("e.g., reportgen-data");
    form->addRow("S3 Bucket:", txtBucket_);
    
    txtPrefix_ = new QLineEdit();
    txtPrefix_->setPlaceholderText("e.g., V2.00_Roofus_Data");
    form->addRow("S3 Prefix:", txtPrefix_);
    
    txtOperator_ = new QLineEdit();
    txtOperator_->setPlaceholderText("Operator name for metadata");
    form->addRow("Operator:", txtOperator_);
    
    comboRegion_ = new QComboBox();
    comboRegion_->addItems({"us-east-1", "us-east-2", "us-west-1", "us-west-2",
                            "eu-west-1", "eu-central-1", "ap-southeast-1", 
                            "ap-northeast-1", "ap-south-1"});
    form->addRow("Region:", comboRegion_);
    
    comboStorageClass_ = new QComboBox();
    comboStorageClass_->addItem("Standard", "STANDARD");
    comboStorageClass_->addItem("Intelligent-Tiering", "INTELLIGENT_TIERING");
    comboStorageClass_->addItem("Standard-IA", "STANDARD_IA");
    comboStorageClass_->addItem("Glacier", "GLACIER");
    form->addRow("Storage Class:", comboStorageClass_);
    
    spinConcurrency_ = new QSpinBox();
    spinConcurrency_->setRange(1, 20);
    spinConcurrency_->setValue(10);
    spinConcurrency_->setToolTip("Max parallel upload requests");
    form->addRow("Concurrency:", spinConcurrency_);
    
    mainLayout->addLayout(form);
    
    // Test connection
    QHBoxLayout* testLayout = new QHBoxLayout();
    btnTest_ = new QPushButton("Test Connection");
    connect(btnTest_, &QPushButton::clicked, this, &AwsSettingsDialog::onTestConnection);
    testLayout->addWidget(btnTest_);
    
    lblTestResult_ = new QLabel();
    lblTestResult_->setWordWrap(true);
    testLayout->addWidget(lblTestResult_, 1);
    mainLayout->addLayout(testLayout);
    
    // Dialog buttons
    QDialogButtonBox* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttons);
}

AwsConfig AwsSettingsDialog::getConfig() const {
    AwsConfig cfg;
    cfg.bucketName = txtBucket_->text().trimmed();
    cfg.prefix = txtPrefix_->text().trimmed();
    cfg.operatorName = txtOperator_->text().trimmed();
    cfg.region = comboRegion_->currentText();
    cfg.storageClass = comboStorageClass_->currentData().toString();
    cfg.maxConcurrentRequests = spinConcurrency_->value();
    return cfg;
}

void AwsSettingsDialog::onTestConnection() {
    btnTest_->setEnabled(false);
    lblTestResult_->setText("Testing...");
    lblTestResult_->setStyleSheet("color: #666;");
    
    if (!testMonitor_) {
        testMonitor_ = new NetworkMonitor(this);
        connect(testMonitor_, &NetworkMonitor::awsValidationComplete,
                this, &AwsSettingsDialog::onTestComplete);
    }
    
    testMonitor_->validateAwsConfig(txtBucket_->text().trimmed());
}

void AwsSettingsDialog::onTestComplete(const AwsStatus& status) {
    btnTest_->setEnabled(true);
    
    QStringList results;
    
    results << (status.cliInstalled ? "CLI: Installed" : "CLI: NOT INSTALLED");
    results << (status.credentialsValid ? 
                QString("Auth: %1 (%2)").arg(status.userName, status.accountId) :
                "Auth: INVALID");
    
    if (status.bucketAccessible) {
        results << QString("Bucket: Accessible");
    } else if (!status.bucketName.isEmpty()) {
        results << QString("Bucket: %1").arg(status.errorMessage);
    }
    
    if (!status.region.isEmpty()) {
        results << QString("Region: %1").arg(status.region);
    }
    
    bool allGood = status.cliInstalled && status.credentialsValid && status.bucketAccessible;
    
    lblTestResult_->setText(results.join("\n"));
    lblTestResult_->setStyleSheet(allGood ? "color: green;" : "color: #c00;");
}

// =============================================================================
// CloudUploadDialog
// =============================================================================

CloudUploadDialog::CloudUploadDialog(ScanSessionTracker* tracker, QWidget* parent)
    : QDialog(parent)
    , sessionTracker_(tracker)
{
    setWindowTitle("Cloud Upload");
    setMinimumSize(650, 700);
    setWindowFlags(windowFlags() | Qt::WindowMinimizeButtonHint);
    
    setupUI();
    setupConnections();
    loadSettings();
    
    // Network monitor
    networkMonitor_ = new NetworkMonitor(this);
    connect(networkMonitor_, &NetworkMonitor::networkStatusUpdated,
            this, &CloudUploadDialog::onNetworkStatusUpdated);
    connect(networkMonitor_, &NetworkMonitor::awsValidationComplete,
            this, &CloudUploadDialog::onAwsValidationComplete);
    
    // Refresh timer
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(5000);
    connect(refreshTimer_, &QTimer::timeout, this, &CloudUploadDialog::onRefreshTimer);
}

CloudUploadDialog::~CloudUploadDialog() {
    networkMonitor_->stopMonitoring();
    saveSettings();
}

void CloudUploadDialog::setupUI() {
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(10);
    mainLayout->setContentsMargins(15, 15, 15, 15);
    
    // ─── Network & AWS Status ─────────────────────────────────
    QGroupBox* statusGroup = new QGroupBox("Connection Status");
    QGridLayout* statusLayout = new QGridLayout(statusGroup);
    
    lblNetworkIcon_ = new QLabel();
    lblNetworkIcon_->setFixedWidth(20);
    statusLayout->addWidget(lblNetworkIcon_, 0, 0);
    
    lblNetworkStatus_ = new QLabel("Internet: Checking...");
    statusLayout->addWidget(lblNetworkStatus_, 0, 1);
    
    lblAwsIcon_ = new QLabel();
    lblAwsIcon_->setFixedWidth(20);
    statusLayout->addWidget(lblAwsIcon_, 1, 0);
    
    lblAwsStatus_ = new QLabel("Cloud: Checking...");
    statusLayout->addWidget(lblAwsStatus_, 1, 1);
    
    lblLocalData_ = new QLabel("Local data: Scanning...");
    statusLayout->addWidget(lblLocalData_, 2, 0, 1, 2);
    
    mainLayout->addWidget(statusGroup);
    
    // ─── Section Tree ─────────────────────────────────────────
    QGroupBox* dataGroup = new QGroupBox("Downloaded Sections (ready to upload)");
    QVBoxLayout* dataLayout = new QVBoxLayout(dataGroup);
    
    // Toolbar
    QHBoxLayout* toolbar = new QHBoxLayout();
    btnSelectAll_ = new QPushButton("Select All");
    btnDeselectAll_ = new QPushButton("Deselect All");
    btnRefresh_ = new QPushButton("Refresh");
    toolbar->addWidget(btnSelectAll_);
    toolbar->addWidget(btnDeselectAll_);
    toolbar->addStretch();
    toolbar->addWidget(btnRefresh_);
    dataLayout->addLayout(toolbar);
    
    treeWidget_ = new QTreeWidget();
    treeWidget_->setHeaderLabels({"Section", "Size", "Files", "Status"});
    treeWidget_->setColumnWidth(0, 240);
    treeWidget_->setColumnWidth(1, 80);
    treeWidget_->setColumnWidth(2, 60);
    treeWidget_->setColumnWidth(3, 120);
    treeWidget_->setAlternatingRowColors(true);
    treeWidget_->setRootIsDecorated(true);
    dataLayout->addWidget(treeWidget_);
    
    // Summary
    lblSelectionSummary_ = new QLabel("Select sections to upload");
    lblSelectionSummary_->setStyleSheet("font-weight: bold;");
    dataLayout->addWidget(lblSelectionSummary_);
    
    lblEstTime_ = new QLabel();
    lblEstTime_->setStyleSheet("color: #666;");
    dataLayout->addWidget(lblEstTime_);
    
    // GPS / Location (shown for selected section)
    QHBoxLayout* gpsRow = new QHBoxLayout();
    lblGpsCoords_ = new QLabel();
    lblGpsCoords_->setStyleSheet("color: #666; font-size: 11px;");
    gpsRow->addWidget(lblGpsCoords_);
    gpsRow->addStretch();
    lblAddress_ = new QLabel();
    lblAddress_->setWordWrap(true);
    lblAddress_->setStyleSheet("color: #666; font-size: 11px;");
    gpsRow->addWidget(lblAddress_);
    dataLayout->addLayout(gpsRow);
    
    mainLayout->addWidget(dataGroup, 1);
    
    // ─── Progress ─────────────────────────────────────────────
    groupProgress_ = new QGroupBox("Upload Progress");
    QVBoxLayout* progressLayout = new QVBoxLayout(groupProgress_);
    
    lblProgressSection_ = new QLabel();
    progressLayout->addWidget(lblProgressSection_);
    
    progressBar_ = new QProgressBar();
    progressBar_->setTextVisible(true);
    progressLayout->addWidget(progressBar_);
    
    QHBoxLayout* detailsLayout = new QHBoxLayout();
    lblProgressBytes_ = new QLabel();
    lblProgressSpeed_ = new QLabel();
    lblProgressEta_ = new QLabel();
    detailsLayout->addWidget(lblProgressBytes_);
    detailsLayout->addStretch();
    detailsLayout->addWidget(lblProgressSpeed_);
    detailsLayout->addStretch();
    detailsLayout->addWidget(lblProgressEta_);
    progressLayout->addLayout(detailsLayout);
    
    lblQueueStatus_ = new QLabel();
    lblQueueStatus_->setStyleSheet("color: #666;");
    progressLayout->addWidget(lblQueueStatus_);
    
    groupProgress_->setVisible(false);
    mainLayout->addWidget(groupProgress_);
    
    // ─── Action Buttons ───────────────────────────────────────
    QHBoxLayout* actionLayout = new QHBoxLayout();
    
    btnUpload_ = new QPushButton("Upload Selected");
    btnUpload_->setMinimumHeight(36);
    btnUpload_->setStyleSheet(
        "QPushButton { background-color: #2196F3; color: white; font-weight: bold; "
        "border-radius: 4px; padding: 6px 16px; }"
        "QPushButton:hover { background-color: #1976D2; }"
        "QPushButton:disabled { background-color: #ccc; color: #888; }");
    actionLayout->addWidget(btnUpload_);
    
    btnPause_ = new QPushButton("Pause");
    btnPause_->setEnabled(false);
    btnPause_->setMinimumHeight(36);
    actionLayout->addWidget(btnPause_);
    
    btnCancel_ = new QPushButton("Cancel");
    btnCancel_->setEnabled(false);
    btnCancel_->setMinimumHeight(36);
    actionLayout->addWidget(btnCancel_);
    
    actionLayout->addStretch();
    
    mainLayout->addLayout(actionLayout);
}

void CloudUploadDialog::setupConnections() {
    connect(btnSelectAll_, &QPushButton::clicked, this, &CloudUploadDialog::onSelectAll);
    connect(btnDeselectAll_, &QPushButton::clicked, this, &CloudUploadDialog::onDeselectAll);
    connect(btnRefresh_, &QPushButton::clicked, this, &CloudUploadDialog::refreshSections);
    connect(btnUpload_, &QPushButton::clicked, this, &CloudUploadDialog::onUploadClicked);
    connect(btnPause_, &QPushButton::clicked, this, &CloudUploadDialog::onPauseClicked);
    connect(btnCancel_, &QPushButton::clicked, this, &CloudUploadDialog::onCancelClicked);
    connect(treeWidget_, &QTreeWidget::itemChanged, this, &CloudUploadDialog::onItemChanged);
    connect(treeWidget_, &QTreeWidget::currentItemChanged,
            this, &CloudUploadDialog::onCurrentItemChanged);
    
    // Upload manager signals
    auto& um = CloudUploadManager::instance();
    connect(&um, &CloudUploadManager::uploadProgress,
            this, &CloudUploadDialog::onUploadProgress);
    connect(&um, &CloudUploadManager::uploadStateChanged,
            this, &CloudUploadDialog::onUploadStateChanged);
    connect(&um, &CloudUploadManager::uploadCompleted,
            this, &CloudUploadDialog::onUploadCompleted);
    connect(&um, &CloudUploadManager::queueChanged,
            this, &CloudUploadDialog::onQueueChanged);
    connect(&um, &CloudUploadManager::geocodeResult,
            this, &CloudUploadDialog::onGeocodeResult);
    connect(&um, &CloudUploadManager::uploadVerified,
            this, &CloudUploadDialog::onUploadVerified);
}

void CloudUploadDialog::setLocalDataPath(const QString& path) {
    localDataPath_ = path;
}

void CloudUploadDialog::bringToFront() {
    show();
    raise();
    activateWindow();
    activate();
}

void CloudUploadDialog::activate() {
    // Start background monitoring (non-blocking - just starts a timer)
    if (!networkMonitor_->isMonitoring()) {
        networkMonitor_->startMonitoring(8000);
    }
    if (!refreshTimer_->isActive()) {
        refreshTimer_->start();
    }
    
    // Validate cloud credentials in a background thread (non-blocking)
    auto& um = CloudUploadManager::instance();
    networkMonitor_->validateAwsConfig(um.config().bucketName);
    
    // Defer disk scan so tab paint is instant
    QTimer::singleShot(50, this, &CloudUploadDialog::refreshSections);
}

void CloudUploadDialog::deactivate() {
    // Keep monitoring in background but reduce frequency
    // Don't fully stop - uploads may be in progress
}

void CloudUploadDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    // All heavy work is deferred via activate() called from the tab switch
}

void CloudUploadDialog::closeEvent(QCloseEvent* event) {
    networkMonitor_->stopMonitoring();
    refreshTimer_->stop();
    saveSettings();
    QDialog::closeEvent(event);
}

// =============================================================================
// Section Tree
// =============================================================================

void CloudUploadDialog::refreshSections() {
    if (localDataPath_.isEmpty()) return;
    if (refreshRunning_) return;
    refreshRunning_ = true;
    
    lblLocalData_->setText("Local data: Scanning...");
    
    // Heavy disk I/O runs in a background thread
    QString dataPath = localDataPath_;
    QThread* thread = QThread::create([this, dataPath]() {
        auto& um = CloudUploadManager::instance();
        QList<CloudUploadManager::LocalSection> sections = um.scanLocalData(dataPath);
        
        // Compute sub-folder sizes in the thread too
        QList<SectionUIData> uiData;
        
        for (const auto& sec : sections) {
            SectionUIData su;
            su.sec = sec;
            for (const QString& sub : sec.subFolders) {
                CloudSubInfo si;
                si.name = sub;
                si.size = 0;
                si.count = 0;
                QDirIterator it(sec.path + "/" + sub, QDir::Files,
                               QDirIterator::Subdirectories);
                while (it.hasNext()) {
                    it.next();
                    si.count++;
                    si.size += it.fileInfo().size();
                }
                su.subs.append(si);
            }
            uiData.append(su);
        }
        
        // Post results back to the main thread for UI update
        QMetaObject::invokeMethod(this, [this, uiData]() {
            populateTree(uiData);
        }, Qt::QueuedConnection);
    });
    
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void CloudUploadDialog::populateTree(const QList<SectionUIData>& uiData) {
    refreshRunning_ = false;
    updatingItems_ = true;
    treeWidget_->clear();
    
    qint64 totalLocalSize = 0;
    int totalSections = 0;
    
    for (const auto& su : uiData) {
        const auto& sec = su.sec;
        QTreeWidgetItem* item = new QTreeWidgetItem(treeWidget_);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, Qt::Unchecked);
        
        // Section name (with optional date prefix)
        QString displayName = sec.name;
        if (!sec.dateFolder.isEmpty()) {
            displayName = QString("%1 / %2").arg(sec.dateFolder, sec.name);
        }
        item->setText(0, displayName);
        item->setText(1, formatBytes(sec.totalSize));
        item->setText(2, QString::number(sec.fileCount));
        
        // Store data for upload
        item->setData(0, Qt::UserRole, sec.path);
        item->setData(0, Qt::UserRole + 1, sec.dateFolder);
        item->setData(0, Qt::UserRole + 2, sec.name);
        item->setData(0, Qt::UserRole + 3, sec.totalSize);
        
        // Build data types indicator (like the download dialog)
        bool hasVisual = false, hasGpr = false, hasRosbag = false, hasMap = false;
        for (const auto& sub : su.subs) {
            QString lower = sub.name.toLower();
            if (lower.startsWith("visual")) hasVisual = true;
            else if (lower.startsWith("gpr")) hasGpr = true;
            else if (lower.startsWith("rosbag")) hasRosbag = true;
            else if (lower.startsWith("map")) hasMap = true;
        }
        // Check root files for maps (.pcd)
        for (const auto& sub : su.subs) {
            Q_UNUSED(sub);
        }
        // Also check if .pcd files exist at section level
        QDir secDir(sec.path);
        if (!secDir.entryList(QStringList() << "*.pcd", QDir::Files).isEmpty()) {
            hasMap = true;
        }
        
        QString types;
        types += hasVisual ? "V " : "";
        types += hasGpr ? "G " : "";
        types += hasMap ? "M " : "";
        types += hasRosbag ? "B " : "";
        if (!types.isEmpty()) {
            item->setText(3, types.trimmed());
            item->setForeground(3, QBrush(QColor("#666")));
        }
        
        // Upload status as column 4
        if (sec.alreadyUploaded) {
            item->setText(3, (types.isEmpty() ? "" : types) + "  Uploaded");
            item->setForeground(3, QBrush(QColor("#4CAF50")));
            item->setForeground(0, QBrush(QColor("#888")));
        } else {
            item->setText(3, (types.isEmpty() ? "" : types) + "  Pending");
            item->setForeground(3, QBrush(QColor("#ff9800")));
        }
        
        // Sub-folders as children (expandable, for info only)
        for (const auto& sub : su.subs) {
            QTreeWidgetItem* child = new QTreeWidgetItem(item);
            child->setText(0, sub.name);
            child->setText(1, formatBytes(sub.size));
            child->setText(2, QString::number(sub.count));
        }
        
        totalLocalSize += sec.totalSize;
        totalSections++;
    }
    
    updatingItems_ = false;
    
    lblLocalData_->setText(QString("Local data: %1 sections, %2 total")
        .arg(totalSections).arg(formatBytes(totalLocalSize)));
    
    updateSelectionSummary();
    
    // Verify "Uploaded" sections still exist in S3 (runs in background)
    if (networkMonitor_->isOnline()) {
        // Build LocalSection list from uiData for verification
        QList<CloudUploadManager::LocalSection> secs;
        for (const auto& su : uiData) {
            secs.append(su.sec);
        }
        CloudUploadManager::instance().verifyUploadedSections(secs);
    }
}

void CloudUploadDialog::onSelectAll() {
    updatingItems_ = true;
    for (int i = 0; i < treeWidget_->topLevelItemCount(); ++i) {
        treeWidget_->topLevelItem(i)->setCheckState(0, Qt::Checked);
    }
    updatingItems_ = false;
    updateSelectionSummary();
}

void CloudUploadDialog::onDeselectAll() {
    updatingItems_ = true;
    for (int i = 0; i < treeWidget_->topLevelItemCount(); ++i) {
        treeWidget_->topLevelItem(i)->setCheckState(0, Qt::Unchecked);
    }
    updatingItems_ = false;
    updateSelectionSummary();
}

void CloudUploadDialog::onItemChanged(QTreeWidgetItem*, int) {
    if (updatingItems_) return;
    updateSelectionSummary();
}

void CloudUploadDialog::onCurrentItemChanged(QTreeWidgetItem* current, QTreeWidgetItem*) {
    if (!current || !sessionTracker_) {
        lblGpsCoords_->clear();
        lblAddress_->clear();
        return;
    }
    
    // If it's a child item, use the parent
    QTreeWidgetItem* sectionItem = current->parent() ? current->parent() : current;
    
    QString sectionName = sectionItem->data(0, Qt::UserRole + 2).toString();
    QString dateFolder = sectionItem->data(0, Qt::UserRole + 1).toString();
    QString localPath = sectionItem->data(0, Qt::UserRole).toString();
    
    if (sectionName.isEmpty()) {
        lblGpsCoords_->clear();
        lblAddress_->clear();
        return;
    }
    
    // Get metadata for this specific section from the session tracker
    ScanMetadata meta = sessionTracker_->getMetadataForSection(
        sectionName, dateFolder, localPath);
    
    if (meta.latitude != 0.0 || meta.longitude != 0.0) {
        lblGpsCoords_->setText(QString("GPS: %1, %2")
            .arg(meta.latitude, 0, 'f', 6)
            .arg(meta.longitude, 0, 'f', 6));
        lblGpsCoords_->setStyleSheet("color: #333; font-size: 11px;");
        
        // Trigger geocoding for this section's coords if online
        if (networkMonitor_->isOnline()) {
            // Only re-geocode if coords differ from last request
            if (qAbs(meta.latitude - geocodedLat_) > 0.0001 ||
                qAbs(meta.longitude - geocodedLon_) > 0.0001) {
                geocodedLat_ = meta.latitude;
                geocodedLon_ = meta.longitude;
                lblAddress_->setText("Resolving address...");
                lblAddress_->setStyleSheet("color: #999; font-size: 11px;");
                CloudUploadManager::instance().reverseGeocode(
                    meta.latitude, meta.longitude);
            }
        } else {
            lblAddress_->setText("Address: Connect to internet to resolve");
            lblAddress_->setStyleSheet("color: #999; font-size: 11px;");
        }
    } else {
        lblGpsCoords_->setText("GPS: No data for this section");
        lblGpsCoords_->setStyleSheet("color: #999; font-size: 11px;");
        lblAddress_->clear();
    }
}

void CloudUploadDialog::updateSelectionSummary() {
    qint64 selectedSize = 0;
    int selectedCount = 0;
    
    for (int i = 0; i < treeWidget_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = treeWidget_->topLevelItem(i);
        if (item->checkState(0) == Qt::Checked) {
            selectedSize += item->data(0, Qt::UserRole + 3).toLongLong();
            selectedCount++;
        }
    }
    
    if (selectedCount > 0) {
        lblSelectionSummary_->setText(QString("Selected: %1 sections, %2")
            .arg(selectedCount).arg(formatBytes(selectedSize)));
        btnUpload_->setEnabled(networkMonitor_->isOnline());
    } else {
        lblSelectionSummary_->setText("Select sections to upload");
        btnUpload_->setEnabled(false);
    }
    
    // Estimate time (rough: assume 2 MB/s average WiFi upload)
    if (selectedSize > 0) {
        double estSec = double(selectedSize) / (2.0 * 1024 * 1024);
        lblEstTime_->setText(QString("Estimated time: ~%1 at 2 MB/s")
            .arg(formatDuration(static_cast<int>(estSec))));
    } else {
        lblEstTime_->clear();
    }
}

// =============================================================================
// Upload Actions
// =============================================================================

void CloudUploadDialog::onUploadClicked() {
    if (!networkMonitor_->isOnline()) {
        QMessageBox::warning(this, "No Internet", 
            "Internet connection is required for uploading.\n"
            "Please connect to WiFi first.");
        return;
    }
    
    auto& um = CloudUploadManager::instance();
    
    // Check if already uploading
    if (um.hasActiveUpload()) {
        auto result = QMessageBox::question(this, "Upload Active",
            "An upload is already in progress.\n"
            "Do you want to queue the new sections?",
            QMessageBox::Yes | QMessageBox::No);
        if (result != QMessageBox::Yes) return;
    }
    
    // Collect selected items data (lightweight, no disk I/O)
    struct SelectedItem {
        QString localPath, dateFolder, sectionName;
        qint64 totalSize;
        int fileCount;
    };
    QList<SelectedItem> selected;
    
    for (int i = 0; i < treeWidget_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = treeWidget_->topLevelItem(i);
        if (item->checkState(0) != Qt::Checked) continue;
        
        QString localPath = item->data(0, Qt::UserRole).toString();
        if (um.isUploaded(localPath)) continue;
        
        SelectedItem si;
        si.localPath = localPath;
        si.dateFolder = item->data(0, Qt::UserRole + 1).toString();
        si.sectionName = item->data(0, Qt::UserRole + 2).toString();
        si.totalSize = item->data(0, Qt::UserRole + 3).toLongLong();
        si.fileCount = item->text(2).toInt();
        selected.append(si);
    }
    
    if (selected.isEmpty()) return;
    
    // Show progress immediately
    groupProgress_->setVisible(true);
    btnPause_->setEnabled(true);
    btnPause_->setText("Pause");
    btnCancel_->setEnabled(true);
    btnUpload_->setEnabled(false);
    lblProgressSection_->setText("Preparing upload...");
    emit uploadActive(true);
    
    // Gather resolved address from label (already available)
    QString resolvedAddr;
    if (lblAddress_ && !lblAddress_->text().isEmpty() &&
        !lblAddress_->text().startsWith("Resolv")) {
        resolvedAddr = lblAddress_->text();
    }
    
    // Run metadata gathering + enqueue in background thread (disk I/O in getMetadataForSection)
    ScanSessionTracker* tracker = sessionTracker_;
    QThread* thread = QThread::create([this, selected, tracker, resolvedAddr]() {
        auto& um = CloudUploadManager::instance();
        AwsConfig cfg = um.config();
        int enqueued = 0;
        
        for (const auto& si : selected) {
            UploadJob job;
            job.localPath = si.localPath;
            // Build S3 path: s3://bucket/prefix/[date/]section/
            if (si.dateFolder.isEmpty()) {
                job.s3Destination = QString("s3://%1/%2/%3/")
                    .arg(cfg.bucketName, cfg.prefix, si.sectionName);
            } else {
                job.s3Destination = QString("s3://%1/%2/%3/%4/")
                    .arg(cfg.bucketName, cfg.prefix, si.dateFolder, si.sectionName);
            }
            job.sectionName = si.sectionName;
            job.dateFolder = si.dateFolder;
            job.totalBytes = si.totalSize;
            job.totalFiles = si.fileCount;
            
            // Attach metadata (this does disk I/O for data composition)
            if (tracker) {
                job.metadata = tracker->getMetadataForSection(
                    si.sectionName, si.dateFolder, si.localPath);
                job.metadata.operatorName = cfg.operatorName;
                if (!resolvedAddr.isEmpty()) {
                    job.metadata.resolvedAddress = resolvedAddr;
                }
            }
            
            um.enqueueUpload(job);
            enqueued++;
        }
        
        QMetaObject::invokeMethod(this, [this, enqueued]() {
            if (enqueued > 0) {
                lblProgressSection_->setText(QString("Queued %1 section(s) for upload").arg(enqueued));
            } else {
                groupProgress_->setVisible(false);
                btnPause_->setEnabled(false);
                btnCancel_->setEnabled(false);
                emit uploadActive(false);
            }
            btnUpload_->setEnabled(true);
        }, Qt::QueuedConnection);
    });
    
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void CloudUploadDialog::onPauseClicked() {
    auto& um = CloudUploadManager::instance();
    
    if (um.hasActiveUpload()) {
        // Pause: stop current upload, re-queue for resume
        um.pauseCurrentJob();
        btnPause_->setText("Resume");
        lblProgressSection_->setText("Upload paused (will resume where it left off)");
    } else {
        // Resume: restart the paused job
        um.resumeCurrentJob();
        btnPause_->setText("Pause");
    }
}

void CloudUploadDialog::onCancelClicked() {
    auto result = QMessageBox::question(this, "Cancel Upload",
        "Cancel all pending uploads?\n\n"
        "Partially uploaded files will be removed from the cloud.",
        QMessageBox::Yes | QMessageBox::No);
    if (result == QMessageBox::Yes) {
        lblProgressSection_->setText("Cancelling and cleaning up...");
        CloudUploadManager::instance().cancelAll();
        groupProgress_->setVisible(false);
        btnPause_->setEnabled(false);
        btnPause_->setText("Pause");
        btnCancel_->setEnabled(false);
        emit uploadActive(false);
    }
}

// =============================================================================
// Network & Cloud Status
// =============================================================================

void CloudUploadDialog::onNetworkStatusUpdated(const NetworkStatus& status) {
    if (status.internetAvailable) {
        lblNetworkIcon_->setText("🟢");
        lblNetworkStatus_->setText(QString("Internet: %1").arg(status.statusMessage));
        lblNetworkStatus_->setStyleSheet("color: #2E7D32;");
    } else {
        lblNetworkIcon_->setText("🔴");
        lblNetworkStatus_->setText(QString("Internet: %1").arg(status.statusMessage));
        lblNetworkStatus_->setStyleSheet("color: #c62828;");
    }
    
    updateSelectionSummary();  // Enable/disable upload button
}

void CloudUploadDialog::onAwsValidationComplete(const AwsStatus& status) {
    if (status.cliInstalled && status.credentialsValid && status.bucketAccessible) {
        lblAwsIcon_->setText("🟢");
        lblAwsStatus_->setText(QString("Cloud: Ready (%1)").arg(status.bucketName));
        lblAwsStatus_->setStyleSheet("color: #2E7D32;");
    } else if (status.cliInstalled && status.credentialsValid) {
        lblAwsIcon_->setText("🟡");
        QString msg = "Cloud: Authenticated";
        if (!status.bucketAccessible && !status.bucketName.isEmpty()) {
            msg += " (bucket issue)";
        }
        lblAwsStatus_->setText(msg);
        lblAwsStatus_->setStyleSheet("color: #F57F17;");
    } else if (!status.cliInstalled) {
        lblAwsIcon_->setText("🔴");
        lblAwsStatus_->setText("Cloud: CLI not installed");
        lblAwsStatus_->setStyleSheet("color: #c62828;");
    } else {
        lblAwsIcon_->setText("🔴");
        lblAwsStatus_->setText("Cloud: Not authenticated");
        lblAwsStatus_->setStyleSheet("color: #c62828;");
    }
}

void CloudUploadDialog::onGeocodeResult(const QString& address) {
    if (!address.isEmpty()) {
        lblAddress_->setText("Address: " + address);
        lblAddress_->setStyleSheet("color: #333;");
    } else {
        lblAddress_->setText("Address: Could not resolve");
        lblAddress_->setStyleSheet("color: #c00;");
    }
}

void CloudUploadDialog::onUploadVerified(const QString& sectionPath, bool existsInCloud) {
    if (existsInCloud) return;  // Still in cloud, nothing to update
    
    // Section was marked "Uploaded" locally but no longer exists in S3
    // Find the tree item and update its status to "Pending"
    for (int i = 0; i < treeWidget_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = treeWidget_->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString() == sectionPath) {
            // Update status text — preserve data types prefix
            QString current = item->text(3);
            QString types;
            if (current.contains("V ")) types += "V ";
            if (current.contains("G ")) types += "G ";
            if (current.contains("M ")) types += "M ";
            if (current.contains("B ")) types += "B ";
            
            item->setText(3, (types.isEmpty() ? "" : types) + "  Pending");
            item->setForeground(3, QBrush(QColor("#ff9800")));
            item->setForeground(0, QBrush(QColor()));  // Reset to default
            
            qDebug() << "[CloudUploadDialog] Section no longer in cloud, reverted to Pending:" 
                     << sectionPath;
            break;
        }
    }
}

// =============================================================================
// Upload Progress Callbacks
// =============================================================================

void CloudUploadDialog::onUploadProgress(int jobId, qint64 uploaded, qint64 total,
                                          double speedMBps, int percent) {
    auto job = CloudUploadManager::instance().currentJob();
    if (!job) return;
    
    lblProgressSection_->setText(QString("Uploading: %1").arg(job->sectionName));
    
    if (percent >= 0) {
        progressBar_->setRange(0, 100);
        progressBar_->setValue(percent);
    } else {
        // Indeterminate
        progressBar_->setRange(0, 0);
    }
    
    if (total > 0 && uploaded > 0) {
        lblProgressBytes_->setText(QString("%1 / %2")
            .arg(formatBytes(uploaded), formatBytes(total)));
    }
    
    if (speedMBps > 0) {
        lblProgressSpeed_->setText(formatSpeed(speedMBps * 1024 * 1024));
        
        // ETA
        qint64 remaining = total - uploaded;
        if (remaining > 0) {
            double etaSec = remaining / (speedMBps * 1024 * 1024);
            lblProgressEta_->setText(QString("ETA: ~%1").arg(formatDuration(static_cast<int>(etaSec))));
        }
    } else {
        lblProgressSpeed_->setText("Calculating...");
        lblProgressEta_->clear();
    }
}

void CloudUploadDialog::onUploadStateChanged(int jobId, UploadState state) {
    // Update tree item status
    refreshSections();
}

void CloudUploadDialog::onUploadCompleted(int jobId, bool success, const QString& message) {
    if (success) {
        qDebug() << "[CloudUploadDialog] Upload completed successfully for job" << jobId;
    } else {
        qDebug() << "[CloudUploadDialog] Upload failed for job" << jobId << ":" << message;
    }
    
    auto& um = CloudUploadManager::instance();
    
    if (!um.hasActiveUpload() && !um.hasQueuedJobs()) {
        // All done
        groupProgress_->setVisible(false);
        btnPause_->setEnabled(false);
        btnCancel_->setEnabled(false);
        emit uploadActive(false);
        
        if (success) {
            lblQueueStatus_->setText("All uploads completed successfully");
        }
    }
    
    refreshSections();
}

void CloudUploadDialog::onQueueChanged() {
    auto& um = CloudUploadManager::instance();
    int queued = um.queueSize();
    
    if (queued > 0) {
        lblQueueStatus_->setText(QString("%1 section(s) queued").arg(queued));
    } else if (um.hasActiveUpload()) {
        lblQueueStatus_->setText("Uploading...");
    } else {
        lblQueueStatus_->clear();
    }
}

void CloudUploadDialog::onRefreshTimer() {
    updateProgressUI();
}

void CloudUploadDialog::updateProgressUI() {
    auto& um = CloudUploadManager::instance();
    if (!um.hasActiveUpload()) return;
    
    auto job = um.currentJob();
    if (!job) return;
    
    // Estimate time elapsed since upload started
    if (job->startTime.isValid()) {
        int elapsed = job->startTime.secsTo(QDateTime::currentDateTime());
        if (elapsed > 0 && job->totalBytes > 0) {
            // We can't know exact progress with --only-show-errors
            // Show elapsed time and let the user know it's working
            lblProgressEta_->setText(QString("Elapsed: %1").arg(formatDuration(elapsed)));
        }
    }
}

// =============================================================================
// Settings
// =============================================================================

void CloudUploadDialog::loadSettings() {
    QSettings settings("PilotControl", "BDRCoveragePlanner");
    localDataPath_ = settings.value("cloud_upload/local_data_path",
        QDir::homePath() + "/robot_data").toString();
}

void CloudUploadDialog::saveSettings() {
    QSettings settings("PilotControl", "BDRCoveragePlanner");
    settings.setValue("cloud_upload/local_data_path", localDataPath_);
}

// =============================================================================
// Formatting Helpers
// =============================================================================

QString CloudUploadDialog::formatBytes(qint64 bytes) const {
    if (bytes < 1024) return QString("%1 B").arg(bytes);
    if (bytes < 1024LL * 1024) return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    if (bytes < 1024LL * 1024 * 1024) return QString("%1 MB").arg(bytes / (1024.0 * 1024), 0, 'f', 1);
    return QString("%1 GB").arg(bytes / (1024.0 * 1024 * 1024), 0, 'f', 2);
}

QString CloudUploadDialog::formatDuration(int seconds) const {
    if (seconds < 60) return QString("%1s").arg(seconds);
    if (seconds < 3600) return QString("%1m %2s").arg(seconds / 60).arg(seconds % 60);
    return QString("%1h %2m").arg(seconds / 3600).arg((seconds % 3600) / 60);
}

QString CloudUploadDialog::formatSpeed(double bytesPerSec) const {
    if (bytesPerSec < 1024) return QString("%1 B/s").arg(bytesPerSec, 0, 'f', 0);
    if (bytesPerSec < 1024 * 1024) return QString("%1 KB/s").arg(bytesPerSec / 1024, 0, 'f', 1);
    return QString("%1 MB/s").arg(bytesPerSec / (1024 * 1024), 0, 'f', 1);
}

} // namespace f2c_cpp
