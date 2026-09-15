#include "satellite_job_model.hpp"

#include <QDir>
#include <QDirIterator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointF>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

namespace f2c_cpp {

namespace {

double snapToGrid(double value, double lo, double hi, double step,
                  double fallback) {
    if (!std::isfinite(value)) {
        return fallback;
    }
    const double clamped = std::min(hi, std::max(lo, value));
    const double snapped = lo + std::round((clamped - lo) / step) * step;
    // Two decimals is the finest grid either knob uses; kills 0.30000000004.
    return std::round(std::min(hi, snapped) * 100.0) / 100.0;
}

}  // namespace

double ScanParams::snapWidth(double meters) {
    return snapToGrid(meters, kWidthMinM, kWidthMaxM, kWidthStepM,
                      kWidthDefaultM);
}

double ScanParams::snapSpeed(double mps) {
    return snapToGrid(mps, kSpeedMinMps, kSpeedMaxMps, kSpeedStepMps,
                      kSpeedDefaultMps);
}

ScanParams ScanParams::snapped() const {
    ScanParams out;
    out.coverage_width_m = snapWidth(coverage_width_m);
    out.scan_speed_mps = snapSpeed(scan_speed_mps);
    return out;
}

QString ScanParams::launchArgs() const {
    const ScanParams s = snapped();
    // `swath_overlap:=0.0`, never `:=0`. The director's launch file passes
    // this straight into the node's `parameters` dict as a bare
    // LaunchConfiguration, so launch_ros type-infers it with YAML: "0" is an
    // int, and the node declares the parameter as a strict double
    // (`declare_parameter("swath_overlap", 0.05)`). An int override against a
    // DOUBLE descriptor raises InvalidParameterTypeException inside the
    // director's __init__ — before it creates the /coverage/status publisher —
    // so the director dies while every other node in the launch keeps running
    // and Start Scan never unlocks. Same trap for any float arg added here.
    return QStringLiteral(" coverage_width:=%1 swath_overlap:=0.0 "
                          "desired_linear_speed:=%2")
        .arg(QString::number(s.coverage_width_m, 'f', 2),
             QString::number(s.scan_speed_mps, 'f', 2));
}

QVector<geo::GeoPoint> RoiRect::corners() const {
    QVector<geo::GeoPoint> out;
    if (!valid) {
        return out;
    }
    const double s = std::sin(heading_deg * geo::kDegToRad);
    const double c = std::cos(heading_deg * geo::kDegToRad);
    const QPointF u(s, c);
    const QPointF v(c, -s);
    const double hl = length_m / 2.0;
    const double hw = width_m / 2.0;
    const QPointF signs[4] = {
        QPointF(+hl, +hw), QPointF(-hl, +hw),
        QPointF(-hl, -hw), QPointF(+hl, -hw)};
    for (const QPointF& sgn : signs) {
        const double e = sgn.x() * u.x() + sgn.y() * v.x();
        const double n = sgn.x() * u.y() + sgn.y() * v.y();
        out.append(geo::geoFromEnu(center, e, n));
    }
    return out;
}

void RoiPolygon::ensureEdgeFlags() {
    roof_edges.resize(vertices.size());
    edge_locks_m.resize(vertices.size());
}

RoiPolygon RoiPolygon::fromRect(const RoiRect& rect) {
    RoiPolygon poly;
    if (!rect.valid) {
        return poly;
    }
    poly.vertices = rect.corners();
    poly.roof_edges.resize(4);
    poly.edge_locks_m.resize(4);
    for (int i = 0; i < 4; ++i) {
        poly.roof_edges[i] = rect.roof_edges[size_t(i)];
    }
    return poly;
}

QJsonObject Job::toJson() const {
    QJsonObject roi_obj;
    roi_obj["valid"] = roi.valid;
    roi_obj["center_lat"] = roi.center.lat;
    roi_obj["center_lon"] = roi.center.lon;
    roi_obj["length_m"] = roi.length_m;
    roi_obj["width_m"] = roi.width_m;
    roi_obj["heading_deg"] = roi.heading_deg;
    QJsonArray roof_edges;
    for (bool marked : roi.roof_edges) {
        roof_edges.append(marked ? 1 : 0);
    }
    roi_obj["roof_edges"] = roof_edges;

    QJsonObject robot_obj;
    robot_obj["valid"] = robot.valid;
    robot_obj["lat"] = robot.lat;
    robot_obj["lon"] = robot.lon;
    robot_obj["heading_deg"] = robot.heading_deg;

    QJsonObject obj;
    // Schema 6 adds the scan knobs (coverage width / speed); 5 added
    // per-edge pinned lengths; 4 added the polygon, alignment and
    // imagery-cache blocks; 3 added imagery provenance. Readers tolerate
    // every one of those being absent, so older plans load unchanged.
    obj["schema"] = 6;
    obj["id"] = id;
    obj["name"] = name;
    obj["address"] = address;
    obj["mode"] = mode;
    obj["roi"] = roi_obj;
    obj["robot"] = robot_obj;
    obj["created"] = created.toString(Qt::ISODate);
    obj["updated"] = updated.toString(Qt::ISODate);
    if (last_executed_at.isValid()) {
        obj["last_executed_at"] = last_executed_at.toString(Qt::ISODate);
    }
    if (imagery_captured.isValid()) {
        obj["imagery_captured"] = imagery_captured.toString(Qt::ISODate);
        obj["imagery_res_m"] = imagery_res_m;
        obj["imagery_zoom"] = imagery_zoom;
    }
    if (polygon.valid()) {
        QJsonArray verts;
        for (const geo::GeoPoint& p : polygon.vertices) {
            QJsonObject v;
            v["lat"] = p.lat;
            v["lon"] = p.lon;
            verts.append(v);
        }
        obj["polygon"] = verts;
        QJsonArray flags;
        for (bool marked : polygon.roof_edges) {
            flags.append(marked ? 1 : 0);
        }
        obj["polygon_roof_edges"] = flags;
        QJsonArray locks;
        for (double meters : polygon.edge_locks_m) {
            locks.append(meters);
        }
        obj["polygon_edge_locks_m"] = locks;
    }
    if (gps.valid) {
        QJsonObject g;
        g["lat"] = gps.lat;
        g["lon"] = gps.lon;
        g["alt_m"] = gps.alt_m;
        if (gps.heading_valid) {
            g["heading_deg"] = gps.heading_deg;
        }
        g["fix_type"] = gps.fix_type;
        g["hacc_m"] = gps.hacc_m;
        g["num_sats"] = gps.num_sats;
        g["utc"] = gps.utc.toString(Qt::ISODate);
        g["source"] = gps.source;
        obj["gps"] = g;
    }
    if (imagery_cache.cached || imagery_cache.max_zoom > 0) {
        QJsonObject im;
        im["cached"] = imagery_cache.cached;
        im["max_zoom"] = imagery_cache.max_zoom;
        if (imagery_cache.captured.isValid()) {
            im["captured"] = imagery_cache.captured.toString(Qt::ISODate);
        }
        im["res_m"] = imagery_cache.res_m;
        im["layer"] = imagery_cache.layer;
        im["wayback_release"] = imagery_cache.wayback_release;
        if (imagery_cache.min_date.isValid()) {
            im["min_date"] = imagery_cache.min_date.toString(Qt::ISODate);
        }
        im["stitch"] = imagery_cache.stitch_relpath;
        obj["imagery_cache"] = im;
    }
    if (alignment.valid) {
        QJsonObject a;
        a["a00"] = alignment.a00;
        a["a01"] = alignment.a01;
        a["a10"] = alignment.a10;
        a["a11"] = alignment.a11;
        a["tx"] = alignment.tx;
        a["ty"] = alignment.ty;
        a["reflected"] = alignment.reflected;
        a["rmse_m"] = align_rmse_m;
        obj["alignment"] = a;
    }
    {
        const ScanParams s = scan.snapped();
        QJsonObject sp;
        sp["coverage_width_m"] = s.coverage_width_m;
        sp["scan_speed_mps"] = s.scan_speed_mps;
        obj["scan"] = sp;
    }
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
    const QJsonArray roof_edges = roi_obj.value("roof_edges").toArray();
    for (int i = 0; i < 4 && i < roof_edges.size(); ++i) {
        job.roi.roof_edges[size_t(i)] = roof_edges[i].toInt(0) != 0;
    }

    const QJsonObject robot_obj = obj.value("robot").toObject();
    job.robot.valid = robot_obj.value("valid").toBool(false);
    job.robot.lat = robot_obj.value("lat").toDouble();
    job.robot.lon = robot_obj.value("lon").toDouble();
    job.robot.heading_deg = robot_obj.value("heading_deg").toDouble(0.0);

    job.created = QDateTime::fromString(obj.value("created").toString(), Qt::ISODate);
    job.updated = QDateTime::fromString(obj.value("updated").toString(), Qt::ISODate);
    // Schema 1 files predate the mode field — they were all satellite plans.
    job.mode = obj.value("mode").toString(
        QString::fromLatin1(kModeSatellite));
    job.last_executed_at = QDateTime::fromString(
        obj.value("last_executed_at").toString(), Qt::ISODate);
    // Schema <= 2 files carry no imagery provenance — an invalid date here
    // means "unknown", not "fresh". Callers must not treat it as current.
    job.imagery_captured = QDate::fromString(
        obj.value("imagery_captured").toString(), Qt::ISODate);
    job.imagery_res_m = obj.value("imagery_res_m").toDouble(0.0);
    job.imagery_zoom = obj.value("imagery_zoom").toInt(0);

    const QJsonArray verts = obj.value("polygon").toArray();
    if (!verts.isEmpty()) {
        for (const QJsonValue& v : verts) {
            const QJsonObject p = v.toObject();
            job.polygon.vertices.append(
                geo::GeoPoint{p.value("lat").toDouble(),
                              p.value("lon").toDouble()});
        }
        const QJsonArray flags = obj.value("polygon_roof_edges").toArray();
        job.polygon.roof_edges.resize(job.polygon.vertices.size());
        for (int i = 0; i < flags.size() && i < job.polygon.roof_edges.size();
             ++i) {
            job.polygon.roof_edges[i] = flags[i].toInt(0) != 0;
        }
        // Absent on schema <= 4 plans, which simply have no pinned edges.
        const QJsonArray locks = obj.value("polygon_edge_locks_m").toArray();
        job.polygon.edge_locks_m.resize(job.polygon.vertices.size());
        for (int i = 0;
             i < locks.size() && i < job.polygon.edge_locks_m.size(); ++i) {
            job.polygon.edge_locks_m[i] = locks[i].toDouble(0.0);
        }
        job.polygon.ensureEdgeFlags();
    } else if (job.roi.valid) {
        job.polygon = RoiPolygon::fromRect(job.roi);
    }

    const QJsonObject g = obj.value("gps").toObject();
    if (!g.isEmpty()) {
        job.gps.valid = true;
        job.gps.lat = g.value("lat").toDouble();
        job.gps.lon = g.value("lon").toDouble();
        job.gps.alt_m = g.value("alt_m").toDouble();
        if (g.contains("heading_deg") && !g.value("heading_deg").isNull()) {
            job.gps.heading_valid = true;
            job.gps.heading_deg = g.value("heading_deg").toDouble();
        }
        job.gps.fix_type = g.value("fix_type").toString();
        job.gps.hacc_m = g.value("hacc_m").toDouble();
        job.gps.num_sats = g.value("num_sats").toInt();
        job.gps.utc = QDateTime::fromString(g.value("utc").toString(), Qt::ISODate);
        job.gps.source = g.value("source").toString();
    }

    const QJsonObject im = obj.value("imagery_cache").toObject();
    if (!im.isEmpty()) {
        job.imagery_cache.cached = im.value("cached").toBool(false);
        job.imagery_cache.max_zoom = im.value("max_zoom").toInt();
        job.imagery_cache.captured =
            QDate::fromString(im.value("captured").toString(), Qt::ISODate);
        job.imagery_cache.res_m = im.value("res_m").toDouble();
        job.imagery_cache.layer = im.value("layer").toString();
        job.imagery_cache.wayback_release = im.value("wayback_release").toString();
        job.imagery_cache.min_date =
            QDate::fromString(im.value("min_date").toString(), Qt::ISODate);
        job.imagery_cache.stitch_relpath = im.value("stitch").toString();
    }

    const QJsonObject a = obj.value("alignment").toObject();
    if (!a.isEmpty()) {
        job.alignment.a00 = a.value("a00").toDouble(1.0);
        job.alignment.a01 = a.value("a01").toDouble();
        job.alignment.a10 = a.value("a10").toDouble();
        job.alignment.a11 = a.value("a11").toDouble(1.0);
        job.alignment.tx = a.value("tx").toDouble();
        job.alignment.ty = a.value("ty").toDouble();
        job.alignment.reflected = a.value("reflected").toBool(false);
        job.alignment.valid = true;
        job.align_rmse_m = a.value("rmse_m").toDouble();
    }
    // Absent on schema <= 5 plans: they ran the director defaults, which is
    // exactly what ScanParams{} is. Snap so a hand-edited file cannot push
    // a value outside the operator range onto the robot.
    const QJsonObject sp = obj.value("scan").toObject();
    job.scan.coverage_width_m = ScanParams::snapWidth(
        sp.value("coverage_width_m").toDouble(ScanParams::kWidthDefaultM));
    job.scan.scan_speed_mps = ScanParams::snapSpeed(
        sp.value("scan_speed_mps").toDouble(ScanParams::kSpeedDefaultMps));
    return job;
}

JobStore::JobStore() {
    jobs_dir_ =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
        QStringLiteral("/satellite_jobs");
    QDir().mkpath(jobs_dir_);
}

JobStore::JobStore(const QString& jobs_dir) : jobs_dir_(jobs_dir) {
    QDir().mkpath(jobs_dir_);
}

QString JobStore::assetsDir(const QString& job_id) const {
    return jobs_dir_ + QLatin1Char('/') + job_id;
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
    QDir().mkpath(assetsDir(job.id));
    return true;
}

bool JobStore::remove(const QString& job_id) const {
    if (job_id.isEmpty()) {
        return false;
    }
    // JSON first: once the plan file is gone nothing references the assets,
    // so a crash between the two leaves an orphan folder rather than a plan
    // whose imagery has vanished under it.
    const bool removed = QFile::remove(jobs_dir_ + QLatin1Char('/') + job_id +
                                       QStringLiteral(".json"));
    QDir assets(assetsDir(job_id));
    if (assets.exists()) {
        assets.removeRecursively();
    }
    return removed;
}

QStringList JobStore::pruneCompleted(int keep) const {
    QVector<Job> completed;
    for (const Job& job : loadAll()) {
        if (job.executed()) {
            completed.append(job);
        }
    }
    std::sort(completed.begin(), completed.end(), [](const Job& a, const Job& b) {
        return a.last_executed_at > b.last_executed_at;
    });
    QStringList removed;
    for (int i = std::max(keep, 0); i < completed.size(); ++i) {
        if (remove(completed[i].id)) {
            removed.append(completed[i].id);
        }
    }
    return removed;
}

}  // namespace f2c_cpp
