/**
 * @file satellite_site_prefetch.hpp
 * @brief Office-side imagery prefetch for one job site.
 *
 * Caches the tile pyramid around a site into the job's assets folder,
 * stitches a max-zoom `site.jpg`, and writes `imagery.json`. Split out of
 * the UI so the same engine serves the plan-confirmation dialog today and
 * any future batch / CLI prefetch without dragging a QDialog along.
 *
 * The request carries every knob the operator could once type into the old
 * Download Area form. None of them are required: `PrefetchRequest::forSite`
 * derives them from the ROI, and the dialog only exposes them behind an
 * Advanced disclosure.
 */

#pragma once

#include "satellite_geo_math.hpp"
#include "satellite_tile_service.hpp"

#include <QDate>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

namespace f2c_cpp {

struct PrefetchRequest {
    double lat = 0.0;
    double lon = 0.0;
    int radius_m = 500;
    /** Requested ceiling; the imagery-age probe may lower it. */
    int max_zoom = 19;
    int max_age_years = 3;
    bool clarity = false;
    QString wayback_release;
    /** Per-job assets folder that receives tiles/, site.jpg, imagery.json. */
    QString assets_dir;

    /**
     * Defaults for a roof whose ROI has the given centroid and radius. The
     * cached disc is the ROI plus `kFieldMarginM`, floored at
     * `kMinRadiusM`: the operator is expected to redraw on site, and a
     * polygon dragged past the cached edge lands on blank tiles.
     */
    static PrefetchRequest forSite(const geo::GeoPoint& centroid,
                                   double roi_radius_m,
                                   const QString& assets_dir);

    static constexpr int kFieldMarginM = 60;
    static constexpr int kMinRadiusM = 150;
    static constexpr int kDefaultMaxZoom = 19;
    static constexpr int kDefaultMaxAgeYears = 3;
};

struct PrefetchResult {
    bool ok = false;
    int cached = 0;
    int failed = 0;
    /** Populated on success; `cached` is true only when the manifest was
        written, which requires zero failed tiles. */
    TileService::SiteManifest manifest;
    QString message;
};

class SitePrefetcher : public QObject {
    Q_OBJECT

public:
    explicit SitePrefetcher(TileService* tiles, QObject* parent = nullptr);
    ~SitePrefetcher() override;

    /** Tile count the request would touch, before cache dedup. */
    static int estimateTileCount(const PrefetchRequest& request);

    /** Starts the age probe, then the fetch queue, then the stitch. */
    void start(const PrefetchRequest& request);
    /** Stops issuing fetches; in-flight ones finish into the cache harmlessly. */
    void cancel();
    bool busy() const { return busy_; }

    /** Coarse-context floor. A handful of tiles per site gives the operator
        a zoomed-out orientation view at no meaningful quota cost. */
    static constexpr int kMinZoom = 13;

signals:
    void statusChanged(const QString& text);
    void progress(int done, int total);
    void finished(const PrefetchResult& result);

private:
    void beginFetchQueue();
    void startNextFetches();
    void onTileDone(int z, int x, int y, bool ok);
    void finish();

    static constexpr int kParallelFetches = 6;

    TileService* tiles_;
    PrefetchRequest request_;
    QVector<TileService::TileId> queue_;
    QSet<QString> pending_;
    int total_ = 0;
    int done_ = 0;
    int failed_ = 0;
    bool busy_ = false;
    bool cancelled_ = false;
    QDate captured_;
    double res_m_ = 0.0;
};

}  // namespace f2c_cpp
