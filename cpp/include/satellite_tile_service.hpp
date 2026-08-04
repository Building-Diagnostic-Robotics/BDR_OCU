/**
 * @file tile_service.hpp
 * @brief Esri World Imagery tile access: disk cache, async fetch, geocoding.
 *
 * All tiles are persisted under the app cache dir (tiles/z/x/y.jpg) so the
 * map works fully offline once an area has been downloaded. The ArcGIS API
 * key is compiled in via the generated arcgis_key_gen.hpp — never read from
 * on-disk config.
 */

#pragma once

#include <QCache>
#include <QObject>
#include <QPixmap>
#include <QSet>
#include <QString>

#include <functional>

class QNetworkAccessManager;

namespace f2c_cpp {

class TileService : public QObject {
    Q_OBJECT

public:
    explicit TileService(QObject* parent = nullptr);

    /** Memory-then-disk lookup. Returns a null pixmap when not cached. */
    QPixmap cachedTile(int z, int x, int y);
    bool isCached(int z, int x, int y) const;

    /** Async fetch into the disk cache. No-op if cached or already in flight. */
    void fetch(int z, int x, int y);

    /**
     * Forward-geocode via the Esri World Geocoder. Callback fires on the GUI
     * thread with (ok, lat, lon, matched_label_or_error_text).
     */
    void geocode(const QString& query,
                 std::function<void(bool, double, double, QString)> cb);

    bool hasApiKey() const;
    QString cacheRoot() const { return cache_root_; }
    int diskTileCount() const;

    static QString tileKey(int z, int x, int y);

signals:
    void tileReady(int z, int x, int y);
    void tileFailed(int z, int x, int y);

private:
    QString tilePath(int z, int x, int y) const;

    QNetworkAccessManager* nam_ = nullptr;
    QString cache_root_;
    QCache<QString, QPixmap> memory_cache_{512};
    QSet<QString> inflight_;
};

}  // namespace f2c_cpp
