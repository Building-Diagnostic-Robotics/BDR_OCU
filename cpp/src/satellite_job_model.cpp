#include "satellite_job_model.hpp"

#include <QDir>
#include <QDirIterator>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace f2c_cpp {

QJsonObject Job::toJson() const {
    QJsonObject roi_obj;
    roi_obj["valid"] = roi.valid;
    roi_obj["center_lat"] = roi.center.lat;
    roi_obj["center_lon"] = roi.center.lon;
    roi_obj["length_m"] = roi.length_m;
    roi_obj["width_m"] = roi.width_m;
    roi_obj["heading_deg"] = roi.heading_deg;

    QJsonObject robot_obj;
    robot_obj["valid"] = robot.valid;
    robot_obj["lat"] = robot.lat;
    robot_obj["lon"] = robot.lon;
    robot_obj["heading_deg"] = robot.heading_deg;

    QJsonObject obj;
    obj["schema"] = 1;
    obj["id"] = id;
    obj["name"] = name;
    obj["address"] = address;
    obj["roi"] = roi_obj;
    obj["robot"] = robot_obj;
    obj["created"] = created.toString(Qt::ISODate);
    obj["updated"] = updated.toString(Qt::ISODate);
    return obj;
}

Job Job::fromJson(const QJsonObject& obj) {
    Job job;
    job.id = obj.value("id").toString();
    job.name = obj.value("name").toString();
    job.address = obj.value("address").toString();

    const QJsonObject roi_obj = obj.value("roi").toObject();
    job.roi.valid = roi_obj.value("valid").toBool(false);
    job.roi.center.lat = roi_obj.value("center_lat").toDouble();
    job.roi.center.lon = roi_obj.value("center_lon").toDouble();
    job.roi.length_m = roi_obj.value("length_m").toDouble(20.0);
    job.roi.width_m = roi_obj.value("width_m").toDouble(15.0);
    job.roi.heading_deg = roi_obj.value("heading_deg").toDouble(0.0);

    const QJsonObject robot_obj = obj.value("robot").toObject();
    job.robot.valid = robot_obj.value("valid").toBool(false);
    job.robot.lat = robot_obj.value("lat").toDouble();
    job.robot.lon = robot_obj.value("lon").toDouble();
    job.robot.heading_deg = robot_obj.value("heading_deg").toDouble(0.0);

    job.created = QDateTime::fromString(obj.value("created").toString(), Qt::ISODate);
    job.updated = QDateTime::fromString(obj.value("updated").toString(), Qt::ISODate);
    return job;
}

JobStore::JobStore() {
    jobs_dir_ =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
        QStringLiteral("/satellite_jobs");
    QDir().mkpath(jobs_dir_);
}

QString JobStore::slugify(const QString& name) {
    QString s = name.trimmed().toLower();
    static const QRegularExpression non_alnum(QStringLiteral("[^a-z0-9]+"));
    s.replace(non_alnum, QStringLiteral("_"));
    while (s.startsWith('_')) s.remove(0, 1);
    while (s.endsWith('_')) s.chop(1);
    return s.isEmpty() ? QStringLiteral("job") : s;
}

QVector<Job> JobStore::loadAll() const {
    QVector<Job> jobs;
    QDirIterator it(jobs_dir_, {QStringLiteral("*.json")}, QDir::Files);
    while (it.hasNext()) {
        QFile file(it.next());
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (doc.isObject()) {
            Job job = Job::fromJson(doc.object());
            if (!job.id.isEmpty()) {
                jobs.append(job);
            }
        }
    }
    std::sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) {
        return a.updated > b.updated;
    });
    return jobs;
}

bool JobStore::save(const Job& job, QString* error) const {
    if (job.id.isEmpty()) {
        if (error) *error = QStringLiteral("Job has no id.");
        return false;
    }
    QSaveFile file(jobs_dir_ + QLatin1Char('/') + job.id +
                   QStringLiteral(".json"));
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    file.write(QJsonDocument(job.toJson()).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

bool JobStore::remove(const QString& job_id) const {
    return QFile::remove(jobs_dir_ + QLatin1Char('/') + job_id +
                         QStringLiteral(".json"));
}

}  // namespace f2c_cpp
