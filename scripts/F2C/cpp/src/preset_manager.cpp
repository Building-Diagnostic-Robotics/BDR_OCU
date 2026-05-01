#include "preset_manager.hpp"
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QStandardPaths>
#include <QFileInfo>
#include <QDebug>

namespace f2c_cpp {

// =============================================================================
// PlanningPreset JSON Serialization
// =============================================================================

QJsonObject PlanningPreset::toJson() const {
    QJsonObject json;
    
    // Metadata
    json["name"] = name;
    json["created"] = created.toString(Qt::ISODate);
    json["modified"] = modified.toString(Qt::ISODate);
    
    // Filtering
    QJsonObject filtering;
    filtering["z_min"] = z_min;
    filtering["z_max"] = z_max;
    filtering["downsample_method"] = downsample_method;
    filtering["max_points"] = max_points;
    filtering["voxel_size"] = voxel_size;
    filtering["mean_k"] = mean_k;
    filtering["std_ratio"] = std_ratio;
    json["filtering"] = filtering;
    
    // Hull
    QJsonObject hull;
    hull["method"] = hull_method;
    hull["alpha"] = alpha;
    hull["simplify_tolerance"] = simplify_tolerance;
    json["hull"] = hull;
    
    // Coverage
    QJsonObject coverage;
    coverage["swath_width"] = swath_width;
    coverage["headland_width"] = headland_width;
    coverage["turn_radius"] = turn_radius;
    coverage["auto_align"] = auto_align;
    coverage["direction"] = direction;
    coverage["route_pattern"] = route_pattern;
    coverage["path_planner"] = path_planner;
    coverage["decomposition"] = decomposition;
    coverage["decomp_type"] = decomp_type;
    coverage["axial_turns"] = axial_turns;
    coverage["waypoint_spacing"] = waypoint_spacing;
    json["coverage"] = coverage;
    
    // Execution
    QJsonObject execution;
    execution["robot_speed"] = robot_speed;
    json["execution"] = execution;
    
    return json;
}

PlanningPreset PlanningPreset::fromJson(const QJsonObject& json) {
    PlanningPreset preset;
    
    // Metadata
    preset.name = json["name"].toString();
    preset.created = QDateTime::fromString(json["created"].toString(), Qt::ISODate);
    preset.modified = QDateTime::fromString(json["modified"].toString(), Qt::ISODate);
    
    // Filtering
    QJsonObject filtering = json["filtering"].toObject();
    preset.z_min = filtering["z_min"].toDouble(-1.0);
    preset.z_max = filtering["z_max"].toDouble(3.0);
    preset.downsample_method = filtering["downsample_method"].toString("voxel");
    preset.max_points = filtering["max_points"].toInt(50000);
    preset.voxel_size = filtering["voxel_size"].toDouble(0.05);
    preset.mean_k = filtering["mean_k"].toInt(50);
    preset.std_ratio = filtering["std_ratio"].toDouble(1.0);
    
    // Hull
    QJsonObject hull = json["hull"].toObject();
    preset.hull_method = hull["method"].toString("alphashape");
    preset.alpha = hull["alpha"].toDouble(1.0);
    preset.simplify_tolerance = hull["simplify_tolerance"].toDouble(0.1);
    
    // Coverage
    QJsonObject coverage = json["coverage"].toObject();
    preset.swath_width = coverage["swath_width"].toDouble(0.6);
    preset.headland_width = coverage["headland_width"].toDouble(0.3);
    preset.turn_radius = coverage["turn_radius"].toDouble(0.5);
    preset.auto_align = coverage["auto_align"].toBool(true);
    preset.direction = coverage["direction"].toString("longest");
    preset.route_pattern = coverage["route_pattern"].toString("boustrophedon");
    preset.path_planner = coverage["path_planner"].toString("dubins");
    preset.decomposition = coverage["decomposition"].toBool(false);
    preset.decomp_type = coverage["decomp_type"].toString("trapezoidal");
    preset.axial_turns = coverage["axial_turns"].toBool(false);
    preset.waypoint_spacing = coverage["waypoint_spacing"].toDouble(0.5);
    
    // Execution
    QJsonObject execution = json["execution"].toObject();
    preset.robot_speed = execution["robot_speed"].toDouble(0.3);
    
    return preset;
}

// =============================================================================
// PresetManager Implementation
// =============================================================================

PresetManager::PresetManager(QObject* parent)
    : QObject(parent)
{
    // Default presets directory
    QString configPath = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    presets_dir_ = configPath + "/PilotControl/presets";
    ensurePresetsDir();
}

void PresetManager::setPresetsDir(const QString& dir) {
    presets_dir_ = dir;
    ensurePresetsDir();
}

void PresetManager::ensurePresetsDir() {
    QDir dir(presets_dir_);
    if (!dir.exists()) {
        dir.mkpath(".");
        qDebug() << "[PresetManager] Created presets directory:" << presets_dir_;
    }
}

QString PresetManager::presetFilePath(const QString& name) const {
    return presets_dir_ + "/" + sanitizeName(name) + ".json";
}

QString PresetManager::sanitizeName(const QString& name) const {
    QString sanitized = name;
    // Replace characters that are problematic in filenames
    sanitized.replace('/', '_');
    sanitized.replace('\\', '_');
    sanitized.replace(':', '_');
    sanitized.replace('*', '_');
    sanitized.replace('?', '_');
    sanitized.replace('"', '_');
    sanitized.replace('<', '_');
    sanitized.replace('>', '_');
    sanitized.replace('|', '_');
    return sanitized.trimmed();
}

QStringList PresetManager::availablePresets() const {
    QDir dir(presets_dir_);
    QStringList filters;
    filters << "*.json";
    
    QStringList files = dir.entryList(filters, QDir::Files, QDir::Name);
    QStringList presets;
    
    for (const QString& file : files) {
        QString name = file;
        name.chop(5);  // Remove ".json"
        presets << name;
    }
    
    return presets;
}

bool PresetManager::presetExists(const QString& name) const {
    return QFile::exists(presetFilePath(name));
}

PlanningPreset PresetManager::loadPreset(const QString& name) const {
    QString filePath = presetFilePath(name);
    QFile file(filePath);
    
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "[PresetManager] Failed to open preset file:" << filePath;
        return PlanningPreset();
    }
    
    QByteArray data = file.readAll();
    file.close();
    
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    
    if (parseError.error != QJsonParseError::NoError) {
        qWarning() << "[PresetManager] JSON parse error:" << parseError.errorString();
        return PlanningPreset();
    }
    
    PlanningPreset preset = PlanningPreset::fromJson(doc.object());
    qDebug() << "[PresetManager] Loaded preset:" << name;
    
    return preset;
}

bool PresetManager::savePreset(const PlanningPreset& preset) {
    if (preset.name.isEmpty()) {
        emit error("Preset name cannot be empty");
        return false;
    }
    
    PlanningPreset toSave = preset;
    
    // Set timestamps
    if (!toSave.created.isValid()) {
        toSave.created = QDateTime::currentDateTime();
    }
    toSave.modified = QDateTime::currentDateTime();
    
    QString filePath = presetFilePath(toSave.name);
    QFile file(filePath);
    
    if (!file.open(QIODevice::WriteOnly)) {
        QString msg = QString("Failed to save preset: %1").arg(file.errorString());
        qWarning() << "[PresetManager]" << msg;
        emit error(msg);
        return false;
    }
    
    QJsonDocument doc(toSave.toJson());
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();
    
    qDebug() << "[PresetManager] Saved preset:" << toSave.name;
    emit presetSaved(toSave.name);
    emit presetsChanged();
    
    return true;
}

bool PresetManager::deletePreset(const QString& name) {
    QString filePath = presetFilePath(name);
    
    if (!QFile::exists(filePath)) {
        emit error(QString("Preset '%1' does not exist").arg(name));
        return false;
    }
    
    if (!QFile::remove(filePath)) {
        emit error(QString("Failed to delete preset '%1'").arg(name));
        return false;
    }
    
    qDebug() << "[PresetManager] Deleted preset:" << name;
    emit presetDeleted(name);
    emit presetsChanged();
    
    return true;
}

bool PresetManager::renamePreset(const QString& oldName, const QString& newName) {
    if (oldName == newName) return true;
    
    if (newName.isEmpty()) {
        emit error("New preset name cannot be empty");
        return false;
    }
    
    if (presetExists(newName)) {
        emit error(QString("Preset '%1' already exists").arg(newName));
        return false;
    }
    
    // Load, rename, save, delete old
    PlanningPreset preset = loadPreset(oldName);
    if (!preset.isValid()) {
        emit error(QString("Failed to load preset '%1'").arg(oldName));
        return false;
    }
    
    preset.name = newName;
    
    if (!savePreset(preset)) {
        return false;
    }
    
    // Delete old file
    QString oldPath = presetFilePath(oldName);
    QFile::remove(oldPath);
    
    qDebug() << "[PresetManager] Renamed preset:" << oldName << "->" << newName;
    emit presetsChanged();
    
    return true;
}

bool PresetManager::duplicatePreset(const QString& name, const QString& newName) {
    if (newName.isEmpty()) {
        emit error("New preset name cannot be empty");
        return false;
    }
    
    if (presetExists(newName)) {
        emit error(QString("Preset '%1' already exists").arg(newName));
        return false;
    }
    
    PlanningPreset preset = loadPreset(name);
    if (!preset.isValid()) {
        emit error(QString("Failed to load preset '%1'").arg(name));
        return false;
    }
    
    preset.name = newName;
    preset.created = QDateTime::currentDateTime();
    
    return savePreset(preset);
}

bool PresetManager::exportPreset(const QString& name, const QString& filePath) const {
    QString srcPath = presetFilePath(name);
    
    if (!QFile::exists(srcPath)) {
        return false;
    }
    
    // Remove destination if exists
    if (QFile::exists(filePath)) {
        QFile::remove(filePath);
    }
    
    return QFile::copy(srcPath, filePath);
}

bool PresetManager::importPreset(const QString& filePath, QString* importedName) {
    QFile file(filePath);
    
    if (!file.open(QIODevice::ReadOnly)) {
        emit error(QString("Failed to open file: %1").arg(filePath));
        return false;
    }
    
    QByteArray data = file.readAll();
    file.close();
    
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    
    if (parseError.error != QJsonParseError::NoError) {
        emit error(QString("Invalid preset file: %1").arg(parseError.errorString()));
        return false;
    }
    
    PlanningPreset preset = PlanningPreset::fromJson(doc.object());
    
    if (!preset.isValid()) {
        emit error("Invalid preset data");
        return false;
    }
    
    // If preset with same name exists, append number
    QString baseName = preset.name;
    int counter = 1;
    while (presetExists(preset.name)) {
        preset.name = QString("%1_%2").arg(baseName).arg(counter++);
    }
    
    if (importedName) {
        *importedName = preset.name;
    }
    
    return savePreset(preset);
}

PlanningPreset PresetManager::defaultPreset() {
    PlanningPreset preset;
    preset.name = "Default";
    preset.created = QDateTime::currentDateTime();
    preset.modified = preset.created;
    // All other values are already set to defaults in the struct
    return preset;
}

} // namespace f2c_cpp
