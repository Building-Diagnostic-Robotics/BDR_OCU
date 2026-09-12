/**
 * @file job_model.hpp
 * @brief Job = one building/mission plan: geo-anchored ROI rectangle plus the
 *        planned robot placement (position + heading). Persisted as JSON so
 *        the office plan is editable onsite and the export math runs only at
 *        Send time against whatever the operator last confirmed.
 */

#pragma once

#include "satellite_geo_math.hpp"
#include "similarity_2d.hpp"

#include <QDate>
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>
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
    QVector<geo::GeoPoint> corners() const;
};

/** Operator-drawn coverage polygon (schema 4). Edge i = vertex i -> i+1. */
struct RoiPolygon {
    QVector<geo::GeoPoint> vertices;
    QVector<bool> roof_edges;
    /**
     * Per-edge pinned length in metres; 0 means free. Set when the operator
     * types a dimension, which makes that measurement authoritative: dragging
     * an endpoint afterwards slides along the constraint instead of silently
     * discarding the number that was typed in.
     */
    QVector<double> edge_locks_m;

    bool valid() const { return vertices.size() >= 3; }
    void ensureEdgeFlags();
    double lockedLength(int edge) const {
        return edge >= 0 && edge < edge_locks_m.size() ? edge_locks_m[edge]
                                                       : 0.0;
    }
    static RoiPolygon fromRect(const RoiRect& rect);
};

struct GpsFix {
    bool valid = false;
    double lat = 0.0;
    double lon = 0.0;
    double alt_m = 0.0;
    bool heading_valid = false;
    double heading_deg = 0.0;
    QString fix_type;
    double hacc_m = 0.0;
    int num_sats = 0;
    QDateTime utc;
    QString source;
};

struct ImageryCache {
    bool cached = false;
    int max_zoom = 0;
    QDate captured;
    double res_m = 0.0;
    QString layer;
    QString wayback_release;
    QDate min_date;
    QString stitch_relpath;
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
    /** Stamped when a mission on this plan FINALIZED with data on disk (not
        at launch). Valid => the plan is COMPLETED; cleared by an operator
        Save Plan so the plan can be scanned again. */
    QDateTime last_executed_at;

    /**
     * Provenance of the imagery this plan was DRAWN against (schema 3).
     * World Imagery is a mosaic — the same roof is served from different
     * flights at different zooms — so a plan authored months ago may have
     * been drawn on imagery years older than its own creation date. Carrying
     * it forward is what lets the plan picker warn about that later.
     * Absent on schema <= 2 plans, which is not an error.
     */
    QDate imagery_captured;
    double imagery_res_m = 0.0;
    int imagery_zoom = 0;

    RoiPolygon polygon;
    GpsFix gps;
    ImageryCache imagery_cache;
    Similarity2D alignment;
    double align_rmse_m = 0.0;

    bool isMeasured() const {
        return mode == QLatin1String(kModeMeasured);
    }
    bool executed() const { return last_executed_at.isValid(); }
    bool hasImageryProvenance() const { return imagery_captured.isValid(); }

    QJsonObject toJson() const;
    static Job fromJson(const QJsonObject& obj);
};

/** Loads/saves jobs as individual JSON files in the app data directory. */
class JobStore {
public:
    /** AppData/satellite_jobs — the production store. */
    JobStore();
    /** A store rooted at an explicit directory (tests, tooling). */
    explicit JobStore(const QString& jobs_dir);

    QString jobsDir() const { return jobs_dir_; }
    QString assetsDir(const QString& job_id) const;
    QVector<Job> loadAll() const;
    bool save(const Job& job, QString* error = nullptr) const;
    /** Deletes the plan JSON AND its assets folder (cached imagery, stitch,
        manifest). A plan without its pyramid is not usable on the roof, and
        an assets folder without its plan is an orphan nothing can open. */
    bool remove(const QString& job_id) const;

    /**
     * Completed plans kept on disk. Older completed plans are removed by
     * pruneCompleted() once a newer one lands; PLANNED plans are never
     * pruned — the operator has not scanned them yet.
     */
    static constexpr int kCompletedPlansKept = 5;
    /** Removes completed plans beyond the `keep` most recently executed.
        Returns the ids that were removed. */
    QStringList pruneCompleted(int keep = kCompletedPlansKept) const;

    static QString slugify(const QString& name);

private:
    QString jobs_dir_;
};

}  // namespace f2c_cpp
