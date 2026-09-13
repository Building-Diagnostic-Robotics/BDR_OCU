#include "satellite_geo_math.hpp"
#include "satellite_tile_service.hpp"

#include "arcgis_key_gen.hpp"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>

namespace f2c_cpp {

namespace {

// ArcGIS tile scheme is {z}/{y}/{x} — note y before x.
constexpr const char* kTileUrlBase =
    "https://ibasemaps-api.arcgis.com/arcgis/rest/services/"
    "World_Imagery/MapServer/tile";

constexpr const char* kClarityTileUrlBase =
    "https://clarity.maptiles.arcgis.com/arcgis/rest/services/"
    "World_Imagery/MapServer/tile";

constexpr const char* kWaybackTileUrlBase =
    "https://wayback.maptiles.arcgis.com/arcgis/rest/services/"
    "World_Imagery/WMTS/1.0.0/default028mm/MapServer/tile";

constexpr const char* kGeocodeUrl =
    "https://geocode-api.arcgis.com/arcgis/rest/services/"
    "World/GeocodeServer/findAddressCandidates";

constexpr const char* kSuggestUrl =
    "https://geocode-api.arcgis.com/arcgis/rest/services/"
    "World/GeocodeServer/suggest";

constexpr const char* kWaybackReleasesUrl =
    "https://wayback.maptiles.arcgis.com/arcgis/rest/services/"
    "World_Imagery/MapServer?f=json";

constexpr double kMetersPerDegreeLat = 111320.0;

// Source-imagery provenance lives in scale-banded metadata footprint
// sublayers (5..18) hanging off the World_Imagery MapServer.
//
// This deliberately points at the PUBLIC services.arcgisonline.com copy, not
// the ibasemaps-api host the tiles use: the public one serves these sublayer
// queries without a token and is the one whose layer ids/fields were verified.
// If ibasemaps-api is confirmed to serve /{layer}/query with our key, swapping
// this constant (and re-adding the token param below) is the only change.
constexpr const char* kImageryMetadataMapServer =
    "https://services.arcgisonline.com/arcgis/rest/services/"
    "World_Imagery/MapServer";

// Metadata cache granularity. 3 decimal places is ~110 m at the equator —
// far finer than the footprint polygons, coarse enough that panning across a
// roof reuses one entry.
constexpr int kImageryCellDecimals = 3;

}  // namespace

int ImageryInfo::ageYears() const {
    if (!captured.isValid()) {
        return -1;
    }
    return int(captured.daysTo(QDate::currentDate()) / 365.25);
}

int TileService::metadataLayerForZoom(int zoom) {
    // Derived from the service's published scale bands:
    //   layer 7 = 7.5cm (1:425-212)  -> z21 (1:282)
    //   layer 8 = 15cm  (1:850-425)  -> z20 (1:564)
    //   layer 9 = 30cm  (1:1700-850) -> z19 (1:1128)
    // i.e. one layer per zoom, counting down as zoom counts up.
    return qBound(5, 28 - zoom, 18);
}

TileService::TileService(QObject* parent) : QObject(parent) {
    nam_ = new QNetworkAccessManager(this);
    cache_root_ =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
        QStringLiteral("/satellite_tiles");
    QDir().mkpath(cache_root_);
}

bool TileService::hasApiKey() const {
    return kArcgisApiKey[0] != '\0';
}

QString TileService::tileKey(int z, int x, int y) {
    return QStringLiteral("%1/%2/%3").arg(z).arg(x).arg(y);
}

QString TileService::tilePath(int z, int x, int y) const {
    return cache_root_ + QLatin1Char('/') + tileKey(z, x, y) +
           QStringLiteral(".jpg");
}

bool TileService::isCached(int z, int x, int y) const {
    return QFileInfo::exists(tilePath(z, x, y));
}

bool TileService::isFailedRecently(int z, int x, int y) const {
    const auto it = failed_.constFind(tileKey(z, x, y));
    if (it == failed_.constEnd()) {
        return false;
    }
    return QDateTime::currentMSecsSinceEpoch() - it.value() < kFailedRetryMs;
}

QPixmap TileService::cachedTile(int z, int x, int y) {
    const QString key = tileKey(z, x, y);
    if (QPixmap* cached = memory_cache_.object(key)) {
        return *cached;
    }
    const QString path = tilePath(z, x, y);
    if (!QFileInfo::exists(path)) {
        return QPixmap();
    }
    QPixmap pm(path);
    if (pm.isNull()) {
        // Corrupt cache entry — drop it so a re-fetch can heal it.
        QFile::remove(path);
        return QPixmap();
    }
    memory_cache_.insert(key, new QPixmap(pm));
    return pm;
}

void TileService::fetch(int z, int x, int y) {
    if (max_zoom_cap_ > 0 && z > max_zoom_cap_) {
        emit tileFailed(z, x, y);
        return;
    }
    const QString key = tileKey(z, x, y);
    if (inflight_.contains(key) || isCached(z, x, y) ||
        isFailedRecently(z, x, y)) {
        return;
    }
    if (!hasApiKey() && wayback_release_.isEmpty() &&
        layer_ == ImageryLayer::World) {
        failed_.insert(key, QDateTime::currentMSecsSinceEpoch());
        emit tileFailed(z, x, y);
        return;
    }
    inflight_.insert(key);

    QUrl url(tileUrl(z, x, y));
    QNetworkRequest req(url);
    QNetworkReply* reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, key, z, x, y]() {
        reply->deleteLater();
        inflight_.remove(key);

        const QString content_type =
            reply->header(QNetworkRequest::ContentTypeHeader).toString();
        if (reply->error() != QNetworkReply::NoError ||
            !content_type.startsWith(QLatin1String("image"))) {
            // Typical cause: zoom level beyond the imagery's native LOD for
            // this area (the service 404s). Remember it so the paint loop
            // stops re-requesting and falls back to ancestor tiles.
            failed_.insert(key, QDateTime::currentMSecsSinceEpoch());
            emit tileFailed(z, x, y);
            return;
        }

        const QByteArray data = reply->readAll();
        const QString path = tilePath(z, x, y);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            emit tileFailed(z, x, y);
            return;
        }
        file.write(data);
        if (!file.commit()) {
            emit tileFailed(z, x, y);
            return;
        }
        emit tileReady(z, x, y);
    });
}

void TileService::geocode(
    const QString& query_text,
    std::function<void(bool, double, double, QString)> cb) {
    geocode(query_text, GeocodeBias{},
            [cb](bool ok, double lat, double lon, QString label, bool) {
                cb(ok, lat, lon, std::move(label));
            });
}

namespace {

/** Parameters common to suggest and findAddressCandidates. */
void addGeocodeTuning(QUrlQuery& query, const TileService::GeocodeBias& bias) {
    query.addQueryItem(QStringLiteral("f"), QStringLiteral("json"));
    // Roofus scans buildings: rank real addresses and named places, never
    // cities / postcodes / coordinates, and never a lookalike abroad.
    query.addQueryItem(QStringLiteral("countryCode"), QStringLiteral("USA"));
    query.addQueryItem(
        QStringLiteral("category"),
        QStringLiteral("Address,Point Address,Street Address,POI"));
    if (bias.valid) {
        // Nearby first: the operator is standing at (or planning) this site.
        query.addQueryItem(QStringLiteral("location"),
                           QStringLiteral("%1,%2")
                               .arg(bias.lon, 0, 'f', 6)
                               .arg(bias.lat, 0, 'f', 6));
    }
    query.addQueryItem(QStringLiteral("token"), QLatin1String(kArcgisApiKey));
}

}  // namespace

void TileService::suggest(const QString& text, const GeocodeBias& bias,
                          std::function<void(QVector<Suggestion>)> cb) {
    if (suggest_reply_) {
        // Superseded: drop the older request so its (stale) result never
        // paints over the newer text.
        suggest_reply_->abort();
        suggest_reply_ = nullptr;
    }
    if (!hasApiKey() || text.trimmed().size() < 3) {
        cb({});
        return;
    }
    QUrl url{QLatin1String(kSuggestUrl)};
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("text"), text.trimmed());
    query.addQueryItem(QStringLiteral("maxSuggestions"), QStringLiteral("6"));
    addGeocodeTuning(query, bias);
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setTransferTimeout(3000);
    QNetworkReply* reply = nam_->get(request);
    suggest_reply_ = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, cb]() {
        reply->deleteLater();
        if (suggest_reply_ == reply) {
            suggest_reply_ = nullptr;
        }
        QVector<Suggestion> out;
        if (reply->error() == QNetworkReply::NoError) {
            const QJsonArray items = QJsonDocument::fromJson(reply->readAll())
                                         .object()
                                         .value(QStringLiteral("suggestions"))
                                         .toArray();
            for (const QJsonValue& v : items) {
                const QJsonObject o = v.toObject();
                // Collections ("Starbucks" as a category) are not places.
                if (o.value(QStringLiteral("isCollection")).toBool()) {
                    continue;
                }
                out.append({o.value(QStringLiteral("text")).toString(),
                            o.value(QStringLiteral("magicKey")).toString()});
            }
        } else if (reply->error() == QNetworkReply::OperationCanceledError) {
            return;  // aborted by a newer suggest(); its callback will fire
        }
        cb(out);
    });
}

void TileService::geocode(
    const QString& query_text, const GeocodeBias& bias,
    std::function<void(bool, double, double, QString, bool)> cb) {
    if (!hasApiKey()) {
        cb(false, 0.0, 0.0, QStringLiteral("No ArcGIS API key compiled in."),
           false);
        return;
    }

    QUrl url{QLatin1String(kGeocodeUrl)};
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("maxLocations"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("singleLine"), query_text);
    if (!bias.magic_key.isEmpty()) {
        // Pins the candidate to the suggestion row the operator tapped.
        query.addQueryItem(QStringLiteral("magicKey"), bias.magic_key);
    }
    query.addQueryItem(QStringLiteral("outSR"), QStringLiteral("4326"));
    // Not stored: keeps the call inside the free geocode allowance.
    query.addQueryItem(QStringLiteral("forStorage"), QStringLiteral("false"));
    // Without outFields the response carries no attributes at all — no
    // rooftop point and no way to tell an interpolated match from a real one.
    query.addQueryItem(
        QStringLiteral("outFields"),
        QStringLiteral("DisplayX,DisplayY,Addr_type,Score,LongLabel"));
    addGeocodeTuning(query, bias);
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setTransferTimeout(5000);
    QNetworkReply* reply = nam_->get(request);
    connect(reply, &QNetworkReply::finished, this, [reply, cb]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            cb(false, 0.0, 0.0, reply->errorString(), false);
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        const QJsonArray candidates =
            doc.object().value(QStringLiteral("candidates")).toArray();
        if (candidates.isEmpty()) {
            cb(false, 0.0, 0.0, QStringLiteral("Address not found."), false);
            return;
        }
        const QJsonObject best = candidates.first().toObject();
        const QJsonObject attrs =
            best.value(QStringLiteral("attributes")).toObject();
        const QJsonObject loc =
            best.value(QStringLiteral("location")).toObject();

        // DisplayX/DisplayY is the rooftop. `location` is the street-entry
        // point the routing engine uses — for a commercial building set back
        // from the road that is the driveway, not the roof.
        const QJsonValue display_x = attrs.value(QStringLiteral("DisplayX"));
        const QJsonValue display_y = attrs.value(QStringLiteral("DisplayY"));
        const bool has_rooftop = display_x.isDouble() && display_y.isDouble();
        const double lon =
            has_rooftop ? display_x.toDouble()
                        : loc.value(QStringLiteral("x")).toDouble();
        const double lat =
            has_rooftop ? display_y.toDouble()
                        : loc.value(QStringLiteral("y")).toDouble();

        const QString addr_type =
            attrs.value(QStringLiteral("Addr_type")).toString();
        const double score = attrs.value(QStringLiteral("Score")).toDouble();

        // PointAddress/Subaddress are real building points. StreetAddress
        // means the house number was INTERPOLATED along a block range — thin
        // rural reference data does this and it can miss by a whole parcel.
        // Client code cannot correct Esri's reference data; it can only say
        // when not to trust the jump.
        const bool rooftop_grade =
            addr_type == QLatin1String("PointAddress") ||
            addr_type == QLatin1String("Subaddress");

        QString address = attrs.value(QStringLiteral("LongLabel")).toString();
        if (address.isEmpty()) {
            address = best.value(QStringLiteral("address")).toString();
        }
        const QString label =
            QStringLiteral("%1  [%2, score %3]%4")
                .arg(address,
                     addr_type.isEmpty() ? QStringLiteral("?") : addr_type)
                .arg(score, 0, 'f', 0)
                .arg(rooftop_grade
                         ? QString()
                         : QStringLiteral("  ** INTERPOLATED — confirm the "
                                          "building before drawing **"));

        cb(true, lat, lon, label, rooftop_grade);
    });
}

void TileService::imageryInfoAt(double lat, double lon, int zoom,
                                std::function<void(ImageryInfo)> cb) {
    const int layer = metadataLayerForZoom(zoom);
    const QString cell = QStringLiteral("%1/%2/%3")
                             .arg(layer)
                             .arg(lat, 0, 'f', kImageryCellDecimals)
                             .arg(lon, 0, 'f', kImageryCellDecimals);

    const auto cached = imagery_cache_.constFind(cell);
    if (cached != imagery_cache_.constEnd()) {
        cb(cached.value());
        return;
    }
    auto waiting = imagery_inflight_.find(cell);
    if (waiting != imagery_inflight_.end()) {
        // A request for this cell is already out. Queue behind it rather than
        // firing an empty result now — an "unknown" answer would blank the
        // caller's label, and the download dialog would cap the zoom on a
        // provenance it never actually looked up.
        waiting->append(cb);
        return;
    }
    imagery_inflight_.insert(cell, {});

    QUrl url(QStringLiteral("%1/%2/query")
                 .arg(QLatin1String(kImageryMetadataMapServer))
                 .arg(layer));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("f"), QStringLiteral("json"));
    query.addQueryItem(QStringLiteral("geometry"),
                       QStringLiteral("%1,%2")
                           .arg(lon, 0, 'f', 7)
                           .arg(lat, 0, 'f', 7));
    query.addQueryItem(QStringLiteral("geometryType"),
                       QStringLiteral("esriGeometryPoint"));
    query.addQueryItem(QStringLiteral("inSR"), QStringLiteral("4326"));
    query.addQueryItem(QStringLiteral("spatialRel"),
                       QStringLiteral("esriSpatialRelIntersects"));
    query.addQueryItem(QStringLiteral("outFields"),
                       QStringLiteral("SRC_DATE2,SRC_RES,SRC_DESC,NICE_NAME,"
                                      "MinMapLevel,MaxMapLevel"));
    query.addQueryItem(QStringLiteral("returnGeometry"),
                       QStringLiteral("false"));
    url.setQuery(query);

    QNetworkReply* reply = nam_->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, cell, zoom, cb]() {
                reply->deleteLater();
                const QVector<std::function<void(ImageryInfo)>> queued =
                    imagery_inflight_.take(cell);
                auto deliver = [&cb, &queued](const ImageryInfo& result) {
                    cb(result);
                    for (const auto& waiting : queued) {
                        waiting(result);
                    }
                };

                ImageryInfo info;
                info.queried_zoom = zoom;
                if (reply->error() != QNetworkReply::NoError) {
                    // Provenance is advisory — a failed lookup must never
                    // block planning. Not cached, so it retries later.
                    deliver(info);
                    return;
                }

                const QJsonArray features =
                    QJsonDocument::fromJson(reply->readAll())
                        .object()
                        .value(QStringLiteral("features"))
                        .toArray();
                if (features.isEmpty()) {
                    // Genuinely no footprint here (ocean, or a LOD with no
                    // source). Cache it so we stop asking.
                    imagery_cache_.insert(cell, info);
                    deliver(info);
                    return;
                }

                const QJsonObject a = features.first()
                                          .toObject()
                                          .value(QStringLiteral("attributes"))
                                          .toObject();
                // SRC_DATE2 is epoch MILLISECONDS. (SRC_DATE is a separate
                // integer field — not the same value, do not substitute it.)
                const qint64 ms =
                    qint64(a.value(QStringLiteral("SRC_DATE2")).toDouble());
                if (ms > 0) {
                    info.captured =
                        QDateTime::fromMSecsSinceEpoch(ms, Qt::UTC).date();
                }
                info.src_res_m = a.value(QStringLiteral("SRC_RES")).toDouble();
                info.source = a.value(QStringLiteral("NICE_NAME")).toString();
                if (info.source.isEmpty()) {
                    info.source = a.value(QStringLiteral("SRC_DESC")).toString();
                }
                info.min_map_level =
                    a.value(QStringLiteral("MinMapLevel")).toInt();
                info.max_map_level =
                    a.value(QStringLiteral("MaxMapLevel")).toInt();
                info.valid = info.captured.isValid();

                imagery_cache_.insert(cell, info);
                emit imageryEpochChanged(info);
                deliver(info);
            });
}

int TileService::diskTileCount() const {
    int count = 0;
    QDirIterator it(cache_root_, {QStringLiteral("*.jpg")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        ++count;
    }
    return count;
}

QUrl TileService::connectivityProbeUrl() {
    // The public metadata MapServer: no token, a few hundred bytes of JSON,
    // and the same host family the provenance queries already depend on.
    return QUrl(QString::fromLatin1(kImageryMetadataMapServer) +
                QStringLiteral("?f=json"));
}

QString TileService::tileUrl(int z, int x, int y) const {
    if (!wayback_release_.isEmpty()) {
        return QStringLiteral("%1/%2/%3/%4/%5")
            .arg(QLatin1String(kWaybackTileUrlBase), wayback_release_)
            .arg(z)
            .arg(y)
            .arg(x);
    }
    if (layer_ == ImageryLayer::Clarity) {
        return QStringLiteral("%1/%2/%3/%4")
            .arg(QLatin1String(kClarityTileUrlBase))
            .arg(z)
            .arg(y)
            .arg(x);
    }
    QUrl url(QStringLiteral("%1/%2/%3/%4")
                 .arg(QLatin1String(kTileUrlBase))
                 .arg(z)
                 .arg(y)
                 .arg(x));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("token"),
                       QLatin1String(kArcgisApiKey));
    url.setQuery(query);
    return url.toString();
}

void TileService::setCacheRoot(const QString& root) {
    if (root.isEmpty() || root == cache_root_) {
        return;
    }
    cache_root_ = root;
    QDir().mkpath(cache_root_);
    memory_cache_.clear();
}

void TileService::setLayer(ImageryLayer layer) {
    if (layer_ == layer) {
        return;
    }
    layer_ = layer;
    memory_cache_.clear();
}

void TileService::setWaybackRelease(const QString& release_id) {
    if (wayback_release_ == release_id) {
        return;
    }
    wayback_release_ = release_id;
    memory_cache_.clear();
}

void TileService::setMaxZoomCap(int zoom) {
    max_zoom_cap_ = qMax(0, zoom);
}

bool TileService::imageryMeetsAge(const ImageryInfo& info, int max_age_years,
                                   const QDate& today) {
    if (!info.valid || !info.captured.isValid() || max_age_years < 0) {
        return false;
    }
    const qint64 days = info.captured.daysTo(today);
    return days >= 0 && days <= qint64(max_age_years) * 365 + 1;
}

QVector<TileService::TileId> TileService::tilesForArea(
    double lat, double lon, double radius_m, int min_zoom, int max_zoom) {
    const double dlat = radius_m / kMetersPerDegreeLat;
    const double cos_lat = std::max(0.01, std::cos(lat * M_PI / 180.0));
    const double dlon = radius_m / (kMetersPerDegreeLat * cos_lat);
    QVector<TileId> out;
    for (int z = min_zoom; z <= max_zoom; ++z) {
        const int n = 1 << z;
        auto tile_x = [n](double lon_deg) {
            return qBound(0, int(std::floor(geo::lonToNormX(lon_deg) * n)),
                          n - 1);
        };
        auto tile_y = [n](double lat_deg) {
            return qBound(0, int(std::floor(geo::latToNormY(lat_deg) * n)),
                          n - 1);
        };
        const int x0 = tile_x(lon - dlon);
        const int x1 = tile_x(lon + dlon);
        const int y0 = tile_y(lat + dlat);
        const int y1 = tile_y(lat - dlat);
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                out.append({z, x, y});
            }
        }
    }
    return out;
}

bool TileService::writeSiteManifest(const QString& path,
                                    const SiteManifest& m) const {
    QJsonObject obj;
    obj["lat"] = m.lat;
    obj["lon"] = m.lon;
    obj["radius_m"] = m.radius_m;
    obj["min_zoom"] = m.min_zoom;
    obj["max_zoom"] = m.max_zoom;
    if (m.captured.isValid()) {
        obj["captured"] = m.captured.toString(Qt::ISODate);
    }
    obj["res_m"] = m.res_m;
    obj["layer"] = m.layer;
    obj["wayback_release"] = m.wayback_release;
    if (m.min_date.isValid()) {
        obj["min_date"] = m.min_date.toString(Qt::ISODate);
    }
    obj["stitch"] = m.stitch_relpath;
    if (m.stitch_bounds.isValid()) {
        QJsonArray bounds;
        bounds.append(m.stitch_bounds.x());
        bounds.append(m.stitch_bounds.y());
        bounds.append(m.stitch_bounds.width());
        bounds.append(m.stitch_bounds.height());
        obj["stitch_bounds"] = bounds;
    }
    obj["cached"] = m.cached;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    return file.commit();
}

TileService::SiteManifest TileService::readSiteManifest(const QString& path) {
    SiteManifest m;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return m;
    }
    const QJsonObject obj = QJsonDocument::fromJson(file.readAll()).object();
    m.lat = obj.value("lat").toDouble();
    m.lon = obj.value("lon").toDouble();
    m.radius_m = obj.value("radius_m").toDouble();
    m.min_zoom = obj.value("min_zoom").toInt(13);
    m.max_zoom = obj.value("max_zoom").toInt(19);
    m.captured = QDate::fromString(obj.value("captured").toString(), Qt::ISODate);
    m.res_m = obj.value("res_m").toDouble();
    m.layer = obj.value("layer").toString();
    m.wayback_release = obj.value("wayback_release").toString();
    m.min_date = QDate::fromString(obj.value("min_date").toString(), Qt::ISODate);
    m.stitch_relpath = obj.value("stitch").toString();
    const QJsonArray bounds = obj.value("stitch_bounds").toArray();
    if (bounds.size() == 4) {
        m.stitch_bounds = QRectF(bounds[0].toDouble(), bounds[1].toDouble(),
                                 bounds[2].toDouble(), bounds[3].toDouble());
    }
    m.cached = obj.value("cached").toBool(false);
    return m;
}

geo::GeoPoint TileService::geoFromStitchPixel(const SiteManifest& manifest,
                                              const QSize& image_size,
                                              const QPointF& px) {
    if (!manifest.stitch_bounds.isValid() || image_size.isEmpty()) {
        return geo::GeoPoint{};
    }
    const double u = px.x() / double(image_size.width());
    const double v = px.y() / double(image_size.height());
    const double nx = manifest.stitch_bounds.x() +
                      u * manifest.stitch_bounds.width();
    const double ny = manifest.stitch_bounds.y() +
                      v * manifest.stitch_bounds.height();
    return geo::GeoPoint{geo::normYToLat(ny), geo::normXToLon(nx)};
}

bool TileService::stitchArea(double lat, double lon, double radius_m, int zoom,
                              const QString& out_jpg, QRectF* webmerc_bounds) {
    const QVector<TileId> tiles = tilesForArea(lat, lon, radius_m, zoom, zoom);
    if (tiles.isEmpty()) {
        return false;
    }
    int x0 = tiles.first().x;
    int x1 = tiles.first().x;
    int y0 = tiles.first().y;
    int y1 = tiles.first().y;
    for (const TileId& t : tiles) {
        x0 = qMin(x0, t.x);
        x1 = qMax(x1, t.x);
        y0 = qMin(y0, t.y);
        y1 = qMax(y1, t.y);
        if (!isCached(t.z, t.x, t.y)) {
            return false;
        }
    }
    const int cols = x1 - x0 + 1;
    const int rows = y1 - y0 + 1;
    QImage canvas(cols * 256, rows * 256, QImage::Format_RGB32);
    canvas.fill(Qt::black);
    QPainter painter(&canvas);
    for (const TileId& t : tiles) {
        const QPixmap pm = cachedTile(t.z, t.x, t.y);
        if (pm.isNull()) {
            return false;
        }
        painter.drawPixmap((t.x - x0) * 256, (t.y - y0) * 256, pm);
    }
    painter.end();
    QDir().mkpath(QFileInfo(out_jpg).absolutePath());
    if (!canvas.save(out_jpg, "JPEG", 90)) {
        return false;
    }
    if (webmerc_bounds) {
        const double n = double(1 << zoom);
        *webmerc_bounds = QRectF(x0 / n, y0 / n, cols / n, rows / n);
    }
    return true;
}

void TileService::listWaybackReleases(
    std::function<void(QVector<WaybackRelease>)> cb) {
    QUrl url{QLatin1String(kWaybackReleasesUrl)};
    QNetworkReply* reply = nam_->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [reply, cb]() {
        reply->deleteLater();
        QVector<WaybackRelease> out;
        if (reply->error() != QNetworkReply::NoError) {
            cb(out);
            return;
        }
        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        // Esri publishes releases either as `releases` (array of {release,
        // date}) or as nested `layers`. Accept both shapes.
        QJsonArray releases = obj.value(QStringLiteral("releases")).toArray();
        if (releases.isEmpty()) {
            releases = obj.value(QStringLiteral("ReleaseInfos")).toArray();
        }
        for (const QJsonValue& v : releases) {
            const QJsonObject r = v.toObject();
            WaybackRelease item;
            item.id = r.value(QStringLiteral("release")).toVariant().toString();
            if (item.id.isEmpty()) {
                item.id = r.value(QStringLiteral("id")).toVariant().toString();
            }
            const QString date_s =
                r.value(QStringLiteral("date")).toString();
            item.date = QDate::fromString(date_s.left(10), Qt::ISODate);
            if (!item.date.isValid() && r.contains(QStringLiteral("releaseDate"))) {
                item.date = QDate::fromString(
                    r.value(QStringLiteral("releaseDate")).toString().left(10),
                    Qt::ISODate);
            }
            item.label = item.date.isValid()
                            ? item.date.toString(Qt::ISODate)
                            : item.id;
            if (!item.id.isEmpty()) {
                out.append(item);
            }
        }
        std::sort(out.begin(), out.end(),
                  [](const WaybackRelease& a, const WaybackRelease& b) {
                      return a.date > b.date;
                  });
        cb(out);
    });
}

}  // namespace f2c_cpp
