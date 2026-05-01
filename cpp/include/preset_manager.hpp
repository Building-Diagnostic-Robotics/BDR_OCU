#ifndef PRESET_MANAGER_HPP
#define PRESET_MANAGER_HPP

#include <QObject>
#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QDateTime>
#include <QDir>

namespace f2c_cpp {

/**
 * @brief Preset data structure containing all planning parameters
 */
struct PlanningPreset {
    QString name;
    QDateTime created;
    QDateTime modified;
    
    // Filtering parameters
    double z_min = -1.0;
    double z_max = 3.0;
    QString downsample_method = "voxel";
    int max_points = 50000;
    double voxel_size = 0.05;
    int mean_k = 50;
    double std_ratio = 1.0;
    
    // Hull parameters
    QString hull_method = "alphashape";
    double alpha = 1.0;
    double simplify_tolerance = 0.1;
    
    // Coverage parameters
    double swath_width = 0.6;
    double headland_width = 0.3;
    double turn_radius = 0.5;
    bool auto_align = true;
    QString direction = "longest";  // "longest" or "perpendicular"
    QString route_pattern = "boustrophedon";
    QString path_planner = "dubins";
    bool decomposition = false;
    QString decomp_type = "trapezoidal";
    bool axial_turns = false;
    double waypoint_spacing = 0.5;
    
    // Execution parameters
    double robot_speed = 0.3;
    
    // Convert to/from JSON
    QJsonObject toJson() const;
    static PlanningPreset fromJson(const QJsonObject& json);
    
    // Validation
    bool isValid() const { return !name.isEmpty(); }
};

/**
 * @brief Manages planning presets with JSON file storage
 */
class PresetManager : public QObject {
    Q_OBJECT

public:
    explicit PresetManager(QObject* parent = nullptr);
    ~PresetManager() = default;
    
    // Preset directory management
    QString presetsDir() const { return presets_dir_; }
    void setPresetsDir(const QString& dir);
    
    // Preset operations
    QStringList availablePresets() const;
    bool presetExists(const QString& name) const;
    
    PlanningPreset loadPreset(const QString& name) const;
    bool savePreset(const PlanningPreset& preset);
    bool deletePreset(const QString& name);
    bool renamePreset(const QString& oldName, const QString& newName);
    bool duplicatePreset(const QString& name, const QString& newName);
    
    // Import/Export
    bool exportPreset(const QString& name, const QString& filePath) const;
    bool importPreset(const QString& filePath, QString* importedName = nullptr);
    
    // Default preset
    static PlanningPreset defaultPreset();
    
signals:
    void presetsChanged();
    void presetLoaded(const QString& name);
    void presetSaved(const QString& name);
    void presetDeleted(const QString& name);
    void error(const QString& message);

private:
    QString presetFilePath(const QString& name) const;
    QString sanitizeName(const QString& name) const;
    void ensurePresetsDir();
    
    QString presets_dir_;
};

} // namespace f2c_cpp

#endif // PRESET_MANAGER_HPP
