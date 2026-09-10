/**
 * @file tile_service.hpp
 * @brief Esri World Imagery tile access: disk cache, async fetch, geocoding,
 *        and source-imagery provenance (capture date / resolution).
 *
 * All tiles are persisted under the app cache dir (tiles/z/x/y.jpg) so the
 * map works fully offline once an area has been downloaded. The ArcGIS API
 * key is compiled in via the generated arcgis_key_gen.hpp — never read from
 * on-disk config.
 *
 * Provenance matters here: World Imagery is a MOSAIC of contributor layers,
 * not one image pyramid. Two zoom levels over the same roof can come from
 * different flights years apart (observed at 301 S Market St, Karnes City TX:
 * z19 captured 2026-08-05, z20 from a release whose newest local change is
 * 2016-03-16). Planning a roof against decade-old imagery is a real failure
 * mode, so the capture date is surfaced rather than left implicit.
 */

#pragma once

#include <QCache>
#include <QDate>
#include <QHash>
#include <QObject>
#include <QPixmap>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QVector>

#include <functional>

class QNetworkAccessManager;

namespace f2c_cpp {

/**
 * Provenance of the SOURCE imagery under a point, at one zoom level.
 * `captured` is the flight date, not the mosaic release date — it is the
 * number that tells an operator whether the roof they are looking at still
 * exists in that configuration.
 */
struct ImageryInfo {
    QDate captured;
    double src_res_m = 0.0;   // native GSD of the contributing source
    QString source;           // NICE_NAME, e.g. "Maxar" / a county program
    int min_map_level = 0;    // LOD range this footprint actually covers
    int max_map_level = 0;
    int queried_zoom = 0;
    bool valid = false;

    /** Whole years between capture and today; -1 when unknown. */
    int ageYears() const;
};

class TileService : public QObject {
    Q_OBJECT

public:
    explicit TileService(QObject* parent = nullptr);

    /** Memory-then-disk lookup. Returns a null pixmap when not cached. */
    QPixmap cachedTile(int z, int x, int y);
    bool isCached(int z, int x, int y) const;

    /**
     * True when a recent fetch of this tile failed (usually: the zoom level
     * exceeds the imagery's native LOD for the area and the service 404s).
     * Failed tiles are retried after kFailedRetryMs in case it was a
     * transient network error instead.
     */
    bool isFailedRecently(int z, int x, int y) const;

    /** Async fetch into the disk cache. No-op if cached or already in flight. */
    void fetch(int z, int x, int y);

    /**
     * Forward-geocode via the Esri World Geocoder. Callback fires on the GUI
     * thread with (ok, lat, lon, matched_label_or_error_text).
     *
     * The returned point is the ROOFTOP location (DisplayX/DisplayY) when the
     * service has one; `location` is the street-entry point used for routing
     * and lands in the parking lot of a set-back commercial building. The
     * label carries the match type — a `StreetAddress` match had its house
     * number interpolated along a block range and can be off by a whole
     * parcel, which the operator needs to see before drawing an ROI on it.
     */
    void geocode(const QString& query,
                 std::function<void(bool, double, double, QString)> cb);

    /**
     * Source-imagery provenance under (lat, lon) at `zoom`. Callback fires on
     * the GUI thread; an invalid ImageryInfo means "unknown", never an error
     * worth interrupting the operator over.
     *
     * Results are cached per (layer, coarse cell) because the footprints are
     * large polygons — without that, every pan would fire a request.
     *
     * Every caller's callback fires exactly once. Concurrent requests for the
     * same cell share one network round-trip and are all notified when it
     * lands — the download dialog gates the whole prefetch on this callback,
     * so dropping a coalesced request would hang the download forever.
     */
    void imageryInfoAt(double lat, double lon, int zoom,
                       std::function<void(ImageryInfo)> cb);

    bool hasApiKey() const;
    QString cacheRoot() const { return cache_root_; }
    int diskTileCount() const;

    /**
     * Redirect the on-disk cache (per-job office prefetch). Existing
     * in-memory entries stay; new fetches write under `root`.
     */
    void setCacheRoot(const QString& root);

    enum class ImageryLayer { World, Clarity };
    void setLayer(ImageryLayer layer);
    ImageryLayer layer() const { return layer_; }
    void setWaybackRelease(const QString& release_id);
    QString waybackRelease() const { return wayback_release_; }
    /** 0 = no cap. The map widget should not request tiles past this. */
    void setMaxZoomCap(int zoom);
    int maxZoomCap() const { return max_zoom_cap_; }

    struct TileId {
        int z = 0;
        int x = 0;
        int y = 0;
    };

    /** Tiles covering a ground radius around (lat, lon) from min_zoom..max_zoom. */
    static QVector<TileId> tilesForArea(double lat, double lon, double radius_m,
                                         int min_zoom, int max_zoom);

    /**
     * True when the source flight date is known and within `max_age_years`
     * of `today`. Unknown provenance fails closed (returns false) so the
     * prefetch never silently keeps decade-old extra zoom.
     */
    static bool imageryMeetsAge(const ImageryInfo& info, int max_age_years,
                                 const QDate& today);

    struct SiteManifest {
        double lat = 0.0;
        double lon = 0.0;
        double radius_m = 0.0;
        int min_zoom = 13;
        int max_zoom = 19;
        QDate captured;
        double res_m = 0.0;
        QString layer;
        QString wayback_release;
        QDate min_date;
        QString stitch_relpath;
        bool cached = false;
    };

    bool writeSiteManifest(const QString& path, const SiteManifest& m) const;
    static SiteManifest readSiteManifest(const QString& path);

    /**
     * Composite cached tiles at `zoom` into a JPEG. Returns false if any
     * tile in the radius is missing. `webmerc_bounds` is [min_nx, min_ny,
     * width, height] in normalized Web Mercator [0,1].
     */
    bool stitchArea(double lat, double lon, double radius_m, int zoom,
                     const QString& out_jpg, QRectF* webmerc_bounds = nullptr);

    struct WaybackRelease {
        QString id;
        QDate date;
        QString label;
    };
    void listWaybackReleases(std::function<void(QVector<WaybackRelease>)> cb);

    static QString tileKey(int z, int x, int y);

    /**
     * World_Imagery carries scale-banded metadata footprint sublayers, one per
     * resolution tier (z19 -> layer 9 / 30cm, z20 -> 8 / 15cm, z21 -> 7 /
     * 7.5cm). Exposed for the unit test that pins the mapping.
     */
    static int metadataLayerForZoom(int zoom);

signals:
    void tileReady(int z, int x, int y);
    void tileFailed(int z, int x, int y);
    /** Emitted whenever a metadata query resolves to a new footprint. */
    void imageryEpochChanged(const ImageryInfo& info);

private:
    QString tilePath(int z, int x, int y) const;

    QString tileUrl(int z, int x, int y) const;

    static constexpr qint64 kFailedRetryMs = 60 * 1000;

    QNetworkAccessManager* nam_ = nullptr;
    QString cache_root_;
    ImageryLayer layer_ = ImageryLayer::World;
    QString wayback_release_;
    int max_zoom_cap_ = 0;
    QCache<QString, QPixmap> memory_cache_{512};
    QSet<QString> inflight_;
    QHash<QString, qint64> failed_;  // tile key -> wall ms of last failure
    QHash<QString, ImageryInfo> imagery_cache_;  // coarse cell -> provenance
    // Coarse cell -> callbacks waiting on the single in-flight query.
    QHash<QString, QVector<std::function<void(ImageryInfo)>>> imagery_inflight_;
};

}  // namespace f2c_cpp
