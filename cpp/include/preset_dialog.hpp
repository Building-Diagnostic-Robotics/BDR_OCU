#ifndef PRESET_DIALOG_HPP
#define PRESET_DIALOG_HPP

#include <QDialog>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLineEdit>
#include <QInputDialog>
#include "components/bdr_message_box.hpp"
#include <QFileDialog>

#include "preset_manager.hpp"

namespace f2c_cpp {

/**
 * @brief Dialog for managing presets (list, rename, delete, import/export)
 */
class PresetManagerDialog : public QDialog {
    Q_OBJECT

public:
    explicit PresetManagerDialog(PresetManager* manager, QWidget* parent = nullptr);
    ~PresetManagerDialog() = default;
    
    QString selectedPreset() const;

signals:
    void presetSelected(const QString& name);
    void presetLoadRequested(const QString& name);

private slots:
    void onSelectionChanged();
    void onItemDoubleClicked(QListWidgetItem* item);
    void onRename();
    void onDuplicate();
    void onDelete();
    void onImport();
    void onExport();
    void onLoad();
    void refreshList();

private:
    void setupUI();
    void updatePresetInfo();
    
    PresetManager* manager_;
    
    QListWidget* list_presets_;
    QLabel* lbl_info_;
    
    QPushButton* btn_load_;
    QPushButton* btn_rename_;
    QPushButton* btn_duplicate_;
    QPushButton* btn_delete_;
    QPushButton* btn_import_;
    QPushButton* btn_export_;
    QPushButton* btn_close_;
};

/**
 * @brief Simple dialog for creating a new preset with a name
 */
class NewPresetDialog : public QDialog {
    Q_OBJECT

public:
    explicit NewPresetDialog(const QStringList& existingNames, QWidget* parent = nullptr);
    
    QString presetName() const { return name_; }

private slots:
    void validateAndAccept();

private:
    QLineEdit* edit_name_;
    QStringList existing_names_;
    QString name_;
};

} // namespace f2c_cpp

#endif // PRESET_DIALOG_HPP
