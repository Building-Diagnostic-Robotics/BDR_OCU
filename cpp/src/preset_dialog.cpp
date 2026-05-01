#include "preset_dialog.hpp"
#include <QDateTime>

namespace f2c_cpp {

// =============================================================================
// PresetManagerDialog Implementation
// =============================================================================

PresetManagerDialog::PresetManagerDialog(PresetManager* manager, QWidget* parent)
    : QDialog(parent)
    , manager_(manager)
{
    setWindowTitle("Manage Presets");
    setMinimumSize(500, 400);
    setModal(true);
    
    setupUI();
    refreshList();
    
    // Connect to manager signals
    connect(manager_, &PresetManager::presetsChanged, this, &PresetManagerDialog::refreshList);
}

void PresetManagerDialog::setupUI() {
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(10);
    mainLayout->setContentsMargins(15, 15, 15, 15);
    
    // Title
    QLabel* title = new QLabel("Saved Presets");
    title->setStyleSheet("font-size: 14px; font-weight: bold;");
    mainLayout->addWidget(title);
    
    // Content area
    QHBoxLayout* contentLayout = new QHBoxLayout();
    contentLayout->setSpacing(10);
    
    // Left side: preset list
    QVBoxLayout* leftLayout = new QVBoxLayout();
    
    list_presets_ = new QListWidget();
    list_presets_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_presets_->setMinimumWidth(250);
    connect(list_presets_, &QListWidget::itemSelectionChanged, 
            this, &PresetManagerDialog::onSelectionChanged);
    connect(list_presets_, &QListWidget::itemDoubleClicked,
            this, &PresetManagerDialog::onItemDoubleClicked);
    leftLayout->addWidget(list_presets_);
    
    // Preset info label
    lbl_info_ = new QLabel();
    lbl_info_->setStyleSheet("color: #666; font-size: 10px;");
    lbl_info_->setWordWrap(true);
    leftLayout->addWidget(lbl_info_);
    
    contentLayout->addLayout(leftLayout, 1);
    
    // Right side: action buttons
    QVBoxLayout* btnLayout = new QVBoxLayout();
    btnLayout->setSpacing(8);
    
    btn_load_ = new QPushButton("📂 Load");
    btn_load_->setToolTip("Load selected preset");
    btn_load_->setMinimumHeight(32);
    connect(btn_load_, &QPushButton::clicked, this, &PresetManagerDialog::onLoad);
    btnLayout->addWidget(btn_load_);
    
    btnLayout->addSpacing(10);
    
    btn_rename_ = new QPushButton("✏️ Rename");
    btn_rename_->setToolTip("Rename selected preset");
    connect(btn_rename_, &QPushButton::clicked, this, &PresetManagerDialog::onRename);
    btnLayout->addWidget(btn_rename_);
    
    btn_duplicate_ = new QPushButton("📋 Duplicate");
    btn_duplicate_->setToolTip("Create a copy of the selected preset");
    connect(btn_duplicate_, &QPushButton::clicked, this, &PresetManagerDialog::onDuplicate);
    btnLayout->addWidget(btn_duplicate_);
    
    btn_delete_ = new QPushButton("🗑️ Delete");
    btn_delete_->setToolTip("Delete selected preset");
    btn_delete_->setStyleSheet("QPushButton { color: #c00; }");
    connect(btn_delete_, &QPushButton::clicked, this, &PresetManagerDialog::onDelete);
    btnLayout->addWidget(btn_delete_);
    
    btnLayout->addSpacing(20);
    
    btn_import_ = new QPushButton("📥 Import...");
    btn_import_->setToolTip("Import preset from file");
    connect(btn_import_, &QPushButton::clicked, this, &PresetManagerDialog::onImport);
    btnLayout->addWidget(btn_import_);
    
    btn_export_ = new QPushButton("📤 Export...");
    btn_export_->setToolTip("Export selected preset to file");
    connect(btn_export_, &QPushButton::clicked, this, &PresetManagerDialog::onExport);
    btnLayout->addWidget(btn_export_);
    
    btnLayout->addStretch();
    
    contentLayout->addLayout(btnLayout);
    mainLayout->addLayout(contentLayout, 1);
    
    // Bottom buttons
    QHBoxLayout* bottomLayout = new QHBoxLayout();
    bottomLayout->addStretch();
    
    btn_close_ = new QPushButton("Close");
    btn_close_->setMinimumWidth(80);
    connect(btn_close_, &QPushButton::clicked, this, &QDialog::accept);
    bottomLayout->addWidget(btn_close_);
    
    mainLayout->addLayout(bottomLayout);
    
    // Initial state
    onSelectionChanged();
}

void PresetManagerDialog::refreshList() {
    QString currentSelection;
    if (list_presets_->currentItem()) {
        currentSelection = list_presets_->currentItem()->text();
    }
    
    list_presets_->clear();
    
    QStringList presets = manager_->availablePresets();
    for (const QString& name : presets) {
        QListWidgetItem* item = new QListWidgetItem(name);
        list_presets_->addItem(item);
        
        if (name == currentSelection) {
            list_presets_->setCurrentItem(item);
        }
    }
    
    if (presets.isEmpty()) {
        lbl_info_->setText("No presets saved yet.\nUse 'Save Preset' from the main window to create one.");
    }
    
    onSelectionChanged();
}

void PresetManagerDialog::onSelectionChanged() {
    bool hasSelection = list_presets_->currentItem() != nullptr;
    
    btn_load_->setEnabled(hasSelection);
    btn_rename_->setEnabled(hasSelection);
    btn_duplicate_->setEnabled(hasSelection);
    btn_delete_->setEnabled(hasSelection);
    btn_export_->setEnabled(hasSelection);
    
    updatePresetInfo();
}

void PresetManagerDialog::updatePresetInfo() {
    if (!list_presets_->currentItem()) {
        lbl_info_->setText("");
        return;
    }
    
    QString name = list_presets_->currentItem()->text();
    PlanningPreset preset = manager_->loadPreset(name);
    
    if (preset.isValid()) {
        QString info = QString("Created: %1\nModified: %2\nSwath: %3m, Headland: %4m")
            .arg(preset.created.toString("yyyy-MM-dd hh:mm"))
            .arg(preset.modified.toString("yyyy-MM-dd hh:mm"))
            .arg(preset.swath_width)
            .arg(preset.headland_width);
        lbl_info_->setText(info);
    }
}

void PresetManagerDialog::onItemDoubleClicked(QListWidgetItem* item) {
    if (item) {
        emit presetLoadRequested(item->text());
        accept();
    }
}

void PresetManagerDialog::onLoad() {
    if (list_presets_->currentItem()) {
        emit presetLoadRequested(list_presets_->currentItem()->text());
        accept();
    }
}

void PresetManagerDialog::onRename() {
    QListWidgetItem* item = list_presets_->currentItem();
    if (!item) return;
    
    QString oldName = item->text();
    
    bool ok;
    QString newName = QInputDialog::getText(this, "Rename Preset",
        "Enter new name:", QLineEdit::Normal, oldName, &ok);
    
    if (ok && !newName.isEmpty() && newName != oldName) {
        if (manager_->presetExists(newName)) {
            BdrMessageBox::warning(this, "Rename Failed",
                QString("A preset named '%1' already exists.").arg(newName));
            return;
        }
        
        if (!manager_->renamePreset(oldName, newName)) {
            BdrMessageBox::warning(this, "Rename Failed",
                "Failed to rename preset.");
        }
    }
}

void PresetManagerDialog::onDuplicate() {
    QListWidgetItem* item = list_presets_->currentItem();
    if (!item) return;
    
    QString baseName = item->text();
    QString newName = baseName + "_copy";
    
    // Find unique name
    int counter = 1;
    while (manager_->presetExists(newName)) {
        newName = QString("%1_copy%2").arg(baseName).arg(counter++);
    }
    
    bool ok;
    newName = QInputDialog::getText(this, "Duplicate Preset",
        "Enter name for copy:", QLineEdit::Normal, newName, &ok);
    
    if (ok && !newName.isEmpty()) {
        if (manager_->presetExists(newName)) {
            BdrMessageBox::warning(this, "Duplicate Failed",
                QString("A preset named '%1' already exists.").arg(newName));
            return;
        }
        
        if (!manager_->duplicatePreset(baseName, newName)) {
            BdrMessageBox::warning(this, "Duplicate Failed",
                "Failed to duplicate preset.");
        }
    }
}

void PresetManagerDialog::onDelete() {
    QListWidgetItem* item = list_presets_->currentItem();
    if (!item) return;
    
    QString name = item->text();
    
    int result = BdrMessageBox::question(this, "Delete Preset",
        QString("Are you sure you want to delete '%1'?\n\nThis cannot be undone.").arg(name),
        BdrMessageBox::No);
    
    if (result == BdrMessageBox::Yes) {
        if (!manager_->deletePreset(name)) {
            BdrMessageBox::warning(this, "Delete Failed",
                "Failed to delete preset.");
        }
    }
}

void PresetManagerDialog::onImport() {
    QString filePath = QFileDialog::getOpenFileName(this, "Import Preset",
        QDir::homePath(), "Preset Files (*.json);;All Files (*)");
    
    if (filePath.isEmpty()) return;
    
    QString importedName;
    if (manager_->importPreset(filePath, &importedName)) {
        BdrMessageBox::information(this, "Import Successful",
            QString("Preset imported as '%1'").arg(importedName));
    } else {
        BdrMessageBox::warning(this, "Import Failed",
            "Failed to import preset. The file may be invalid.");
    }
}

void PresetManagerDialog::onExport() {
    QListWidgetItem* item = list_presets_->currentItem();
    if (!item) return;
    
    QString name = item->text();
    QString suggestedPath = QDir::homePath() + "/" + name + ".json";
    
    QString filePath = QFileDialog::getSaveFileName(this, "Export Preset",
        suggestedPath, "Preset Files (*.json);;All Files (*)");
    
    if (filePath.isEmpty()) return;
    
    // Ensure .json extension
    if (!filePath.endsWith(".json", Qt::CaseInsensitive)) {
        filePath += ".json";
    }
    
    if (manager_->exportPreset(name, filePath)) {
        BdrMessageBox::information(this, "Export Successful",
            QString("Preset exported to:\n%1").arg(filePath));
    } else {
        BdrMessageBox::warning(this, "Export Failed",
            "Failed to export preset.");
    }
}

QString PresetManagerDialog::selectedPreset() const {
    if (list_presets_->currentItem()) {
        return list_presets_->currentItem()->text();
    }
    return QString();
}

// =============================================================================
// NewPresetDialog Implementation
// =============================================================================

NewPresetDialog::NewPresetDialog(const QStringList& existingNames, QWidget* parent)
    : QDialog(parent)
    , existing_names_(existingNames)
{
    setWindowTitle("Create New Preset");
    setFixedSize(350, 120);
    setModal(true);
    
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setSpacing(10);
    layout->setContentsMargins(15, 15, 15, 15);
    
    QLabel* label = new QLabel("Enter preset name:");
    layout->addWidget(label);
    
    edit_name_ = new QLineEdit();
    edit_name_->setPlaceholderText("e.g., Roof_Survey_Standard");
    edit_name_->setMaxLength(50);
    connect(edit_name_, &QLineEdit::returnPressed, this, &NewPresetDialog::validateAndAccept);
    layout->addWidget(edit_name_);
    
    QHBoxLayout* btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    
    QPushButton* btnCancel = new QPushButton("Cancel");
    connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);
    btnLayout->addWidget(btnCancel);
    
    QPushButton* btnCreate = new QPushButton("Create");
    btnCreate->setDefault(true);
    connect(btnCreate, &QPushButton::clicked, this, &NewPresetDialog::validateAndAccept);
    btnLayout->addWidget(btnCreate);
    
    layout->addLayout(btnLayout);
    
    edit_name_->setFocus();
}

void NewPresetDialog::validateAndAccept() {
    QString name = edit_name_->text().trimmed();
    
    if (name.isEmpty()) {
        BdrMessageBox::warning(this, "Invalid Name", "Please enter a preset name.");
        return;
    }
    
    if (existing_names_.contains(name, Qt::CaseInsensitive)) {
        BdrMessageBox::warning(this, "Name Exists",
            QString("A preset named '%1' already exists.\nPlease choose a different name.").arg(name));
        return;
    }
    
    name_ = name;
    accept();
}

} // namespace f2c_cpp
