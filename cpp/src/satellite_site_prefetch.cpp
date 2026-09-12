#include "satellite_site_prefetch.hpp"

#include <QDir>

#include <cmath>

namespace f2c_cpp {

PrefetchRequest PrefetchRequest::forSite(const geo::GeoPoint& centroid,
                                         double roi_radius_m,
                                         const QString& assets_dir) {
    PrefetchRequest request;
    request.lat = centroid.lat;
    request.lon = centroid.lon;
    request.radius_m = std::max(
        kMinRadiusM, int(std::ceil(roi_radius_m)) + kFieldMarginM);
    request.max_zoom = kDefaultMaxZoom;
    request.max_age_years = kDefaultMaxAgeYears;
    request.assets_dir = assets_dir;
    return request;
}

SitePrefetcher::SitePrefetcher(TileService* tiles, QObject* parent)
    : QObject(parent), tiles_(tiles) {
    connect(tiles_, &TileService::tileReady, this,
            [this](int z, int x, int y) { onTileDone(z, x, y, true); });
    connect(tiles_, &TileService::tileFailed, this,
            [this](int z, int x, int y) { onTileDone(z, x, y, false); });
}

SitePrefetcher::~SitePrefetcher() = default;

int SitePrefetcher::estimateTileCount(const PrefetchRequest& request) {
    return TileService::tilesForArea(request.lat, request.lon,
                                     request.radius_m, kMinZoom,
                                     request.max_zoom)
        .size();
}

void SitePrefetcher::start(const PrefetchRequest& request) {
    if (busy_) {
        return;
    }
    request_ = request;
    busy_ = true;
    cancelled_ = false;
    captured_ = QDate();
    res_m_ = 0.0;

    tiles_->setLayer(request_.clarity ? TileService::ImageryLayer::Clarity
                                      : TileService::ImageryLayer::World);
    tiles_->setWaybackRelease(request_.wayback_release);
    if (!request_.assets_dir.isEmpty()) {
        QDir().mkpath(request_.assets_dir);
        tiles_->setCacheRoot(request_.assets_dir + QStringLiteral("/tiles"));
    }

    emit statusChanged(QStringLiteral("Checking imagery date…"));
    // The age probe decides the real ceiling: sharper-but-older tiles are
    // refused, and unknown provenance is treated as too old (imageryMeetsAge
    // fails closed). The requested zoom is a maximum, never a promise.
    tiles_->imageryInfoAt(
        request_.lat, request_.lon, request_.max_zoom, [this](ImageryInfo info) {
            if (cancelled_) {
                return;
            }
            captured_ = info.captured;
            res_m_ = info.src_res_m;
            int z = request_.max_zoom;
            if (!TileService::imageryMeetsAge(info, request_.max_age_years,
                                              QDate::currentDate())) {
                if (info.max_map_level >= 16) {
                    z = std::min(z, info.max_map_level);
                }
                emit statusChanged(
                    QStringLiteral("Imagery at z%1 is older than %2 y — "
                                   "capping at z%3 (%4).")
                        .arg(request_.max_zoom)
                        .arg(request_.max_age_years)
                        .arg(z)
                        .arg(info.captured.isValid()
                                 ? info.captured.toString(Qt::ISODate)
                                 : QStringLiteral("date unknown")));
                request_.max_zoom = z;
            }
            tiles_->setMaxZoomCap(z);
            beginFetchQueue();
        });
}

void SitePrefetcher::cancel() {
    if (!busy_) {
        return;
    }
    cancelled_ = true;
    queue_.clear();
    pending_.clear();
    busy_ = false;
    PrefetchResult result;
    result.ok = false;
    result.cached = done_;
    result.failed = failed_;
    result.message = QStringLiteral("Cancelled — %1 tiles kept.").arg(done_);
    emit finished(result);
}

void SitePrefetcher::beginFetchQueue() {
    queue_.clear();
    pending_.clear();
    done_ = 0;
    failed_ = 0;

    const QVector<TileService::TileId> all = TileService::tilesForArea(
        request_.lat, request_.lon, request_.radius_m, kMinZoom,
        request_.max_zoom);
    total_ = all.size();
    int already_cached = 0;
    for (const TileService::TileId& t : all) {
        if (tiles_->isCached(t.z, t.x, t.y)) {
            ++already_cached;
        } else {
            queue_.append(t);
        }
    }
    done_ = already_cached;
    emit progress(done_, total_);

    if (queue_.isEmpty()) {
        emit statusChanged(
            QStringLiteral("All %1 tiles already cached.").arg(total_));
        finish();
        return;
    }
    emit statusChanged(QStringLiteral("Downloading %1 tiles (%2 cached)…")
                           .arg(queue_.size())
                           .arg(already_cached));
    startNextFetches();
}

void SitePrefetcher::startNextFetches() {
    while (pending_.size() < kParallelFetches && !queue_.isEmpty()) {
        const TileService::TileId t = queue_.takeFirst();
        if (tiles_->isCached(t.z, t.x, t.y)) {
            ++done_;
            continue;
        }
        pending_.insert(TileService::tileKey(t.z, t.x, t.y));
        tiles_->fetch(t.z, t.x, t.y);
    }
    if (pending_.isEmpty() && queue_.isEmpty() && busy_) {
        finish();
    }
}

void SitePrefetcher::onTileDone(int z, int x, int y, bool ok) {
    if (!busy_) {
        return;
    }
    if (!pending_.remove(TileService::tileKey(z, x, y))) {
        return;  // a map-widget fetch, not one of ours
    }
    ok ? ++done_ : ++failed_;
    emit progress(done_ + failed_, total_);
    startNextFetches();
}

void SitePrefetcher::finish() {
    busy_ = false;
    PrefetchResult result;
    result.cached = done_;
    result.failed = failed_;

    if (failed_ > 0) {
        result.ok = false;
        result.message =
            QStringLiteral("%1 of %2 tiles failed. Check the connection and "
                           "retry — tiles already cached are kept.")
                .arg(failed_)
                .arg(total_);
        emit finished(result);
        return;
    }

    // The stitch is what alignment picks against, so a site without one is
    // a plan that cannot be anchored in the field. It needs every max-zoom
    // tile, which zero failures guarantees.
    const QString stitch_path =
        request_.assets_dir + QStringLiteral("/site.jpg");
    QRectF stitch_bounds;
    const bool stitched = tiles_->stitchArea(
        request_.lat, request_.lon, request_.radius_m, request_.max_zoom,
        stitch_path, &stitch_bounds);

    TileService::SiteManifest m;
    m.stitch_bounds = stitch_bounds;
    m.lat = request_.lat;
    m.lon = request_.lon;
    m.radius_m = request_.radius_m;
    m.min_zoom = kMinZoom;
    m.max_zoom = request_.max_zoom;
    m.captured = captured_;
    m.res_m = res_m_;
    m.layer = request_.clarity ? QStringLiteral("clarity")
                               : QStringLiteral("world");
    m.wayback_release = request_.wayback_release;
    m.min_date = QDate::currentDate().addYears(-request_.max_age_years);
    m.stitch_relpath = stitched ? QStringLiteral("site.jpg") : QString();
    m.cached = true;
    tiles_->writeSiteManifest(
        request_.assets_dir + QStringLiteral("/imagery.json"), m);

    result.ok = true;
    result.manifest = m;
    result.message =
        QStringLiteral("%1 tiles cached to z%2%3.")
            .arg(total_)
            .arg(request_.max_zoom)
            .arg(stitched ? QString()
                          : QStringLiteral(" (site image skipped — a tile "
                                           "went missing between fetch and "
                                           "stitch)"));
    emit finished(result);
}

}  // namespace f2c_cpp
