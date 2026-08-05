/**
 * @file job_model.hpp
 * @brief Job = one building/mission plan: geo-anchored ROI rectangle plus the
 *        planned robot placement (position + heading). Persisted as JSON so
 *        the office plan is editable onsite and the export math runs only at
 *        Send time against whatever the operator last confirmed.
 */

#pragma once

#include "satellite_geo_math.hpp"

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include <array>

namespace f2c_cpp {

/**
 * Rotated rectangle in ground meters, anchored at a geographic center.
 * length_m runs along heading (the sweep direction), width_m across it.
 */
struct RoiRect {
    geo::GeoPoint center;
    double length_m = 20.0;
    double width_m = 15.0;
    double heading_deg = 0.0;
    bool valid = false;

    /**
     * Per-edge roof-edge marking. Edge i runs corner i -> corner (i+1)%4
     * in corners() order. Marked edges are physical fall hazards: rendered
     * hazard-red on the canvas and (once the robot-side roi_edge_flags
     * parameter lands) given a larger boundary clearance by the coverage
     * manager. Unmarked edges are virtual limits well inside the roof.
     */
    std::array<bool, 4> roof_edges{{false, false, false, false}};

    /** Four corners as geo points, CCW. */
    QVector<geo::GeoPoint> corners() const {
        QVector<geo::GeoPoint> out;
        if (!valid) {
            return out;
        }
        const double s = std::sin(heading_deg * geo::kDegToRad);
        const double c = std::cos(heading_deg * geo::kDegToRad);
        // u = along heading (ENU), v = right of heading.
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
};

struct Job {
    /** Planning canvas the plan was authored on — determines the mode the
        planning screen opens in when the plan is executed. */
    static constexpr const char* kModeSatellite = "satellite";
    static constexpr const char* kModeMeasured = "measured";

    QString id;          // filesystem-safe slug, unique
    QString name;        // operator-facing building/job name
    QString address;     // free text, for reference
    QString mode = QString::fromLatin1(kModeSatellite);
    RoiRect roi;
    geo::GeoPose robot;  // planned robot placement (the anchor at Send)
    QDateTime created;
    QDateTime updated;
    QDateTime last_executed_at;  // stamped when a mission actually launches

    bool isMeasured() const {
        return mode == QLatin1String(kModeMeasured);
    }
    bool executed() const { return last_executed_at.isValid(); }

    QJsonObject toJson() const;
    static Job fromJson(const QJsonObject& obj);
};

/** Loads/saves jobs as individual JSON files in the app data directory. */
class JobStore {
public:
    JobStore();

    QString jobsDir() const { return jobs_dir_; }
    QVector<Job> loadAll() const;
    bool save(const Job& job, QString* error = nullptr) const;
    bool remove(const QString& job_id) const;

    static QString slugify(const QString& name);

private:
    QString jobs_dir_;
};

}  // namespace f2c_cpp
