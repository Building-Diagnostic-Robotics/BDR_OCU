#include "satellite_tile_service.hpp"

#include "arcgis_key_gen.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>

namespace f2c_cpp {

namespace {

// ArcGIS tile scheme is {z}/{y}/{x} — note y before x.
constexpr const char* kTileUrlBase =
    "https://ibasemaps-api.arcgis.com/arcgis/rest/services/"
    "World_Imagery/MapServer/tile";

constexpr const char* kGeocodeUrl =
    "https://geocode-api.arcgis.com/arcgis/rest/services/"
    "World/GeocodeServer/findAddressCandidates";

}  // namespace

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
    const QString key = tileKey(z, x, y);
    if (inflight_.contains(key) || isCached(z, x, y)) {
        return;
    }
    if (!hasApiKey()) {
        emit tileFailed(z, x, y);
        return;
    }
    inflight_.insert(key);

    QUrl url(QStringLiteral("%1/%2/%3/%4")
                 .arg(QLatin1String(kTileUrlBase))
                 .arg(z)
                 .arg(y)
                 .arg(x));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("token"),
                       QLatin1String(kArcgisApiKey));
    url.setQuery(query);

    QNetworkReply* reply = nam_->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, key, z, x, y]() {
        reply->deleteLater();
        inflight_.remove(key);

        const QString content_type =
            reply->header(QNetworkRequest::ContentTypeHeader).toString();
        if (reply->error() != QNetworkReply::NoError ||
            !content_type.startsWith(QLatin1String("image"))) {
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
    if (!hasApiKey()) {
        cb(false, 0.0, 0.0, QStringLiteral("No ArcGIS API key compiled in."));
        return;
    }

    QUrl url{QLatin1String(kGeocodeUrl)};
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("f"), QStringLiteral("json"));
    query.addQueryItem(QStringLiteral("maxLocations"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("singleLine"), query_text);
    query.addQueryItem(QStringLiteral("token"),
                       QLatin1String(kArcgisApiKey));
    url.setQuery(query);

    QNetworkReply* reply = nam_->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [reply, cb]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            cb(false, 0.0, 0.0, reply->errorString());
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        const QJsonArray candidates =
            doc.object().value(QStringLiteral("candidates")).toArray();
        if (candidates.isEmpty()) {
            cb(false, 0.0, 0.0, QStringLiteral("Address not found."));
            return;
        }
        const QJsonObject best = candidates.first().toObject();
        const QJsonObject loc =
            best.value(QStringLiteral("location")).toObject();
        cb(true,
           loc.value(QStringLiteral("y")).toDouble(),
           loc.value(QStringLiteral("x")).toDouble(),
           best.value(QStringLiteral("address")).toString());
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

}  // namespace f2c_cpp
