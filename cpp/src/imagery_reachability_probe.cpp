#include "imagery_reachability_probe.hpp"

#include "satellite_tile_service.hpp"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <algorithm>

namespace f2c_cpp {

ImageryReachabilityProbe::ImageryReachabilityProbe(QObject* parent)
    : QObject(parent) {
    nam_ = new QNetworkAccessManager(this);
    timer_ = new QTimer(this);
    timer_->setInterval(kIntervalMs);
    connect(timer_, &QTimer::timeout, this, &ImageryReachabilityProbe::tick);
}

void ImageryReachabilityProbe::start() {
    if (downloaded_ || timer_->isActive()) {
        return;
    }
    tick();
    timer_->start();
}

void ImageryReachabilityProbe::stop() {
    timer_->stop();
    if (inflight_) {
        inflight_->disconnect(this);
        inflight_->abort();
        inflight_->deleteLater();
        inflight_ = nullptr;
    }
}

void ImageryReachabilityProbe::noteImageryDownloaded() {
    downloaded_ = true;
    stop();
}

void ImageryReachabilityProbe::tick() {
    if (inflight_) {
        return;
    }
    QNetworkRequest request(TileService::connectivityProbeUrl());
    request.setTransferTimeout(kTimeoutMs);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    inflight_ = nam_->head(request);
    connect(inflight_, &QNetworkReply::finished, this,
            [this, reply = inflight_] { onFinished(reply); });
}

void ImageryReachabilityProbe::onFinished(QNetworkReply* reply) {
    reply->deleteLater();
    if (reply == inflight_) {
        inflight_ = nullptr;
    }
    const bool ok = reply->error() == QNetworkReply::NoError;
    successes_ = ok ? std::min(successes_ + 1, kSuccessesToEnable) : 0;
    const bool now = successes_ >= kSuccessesToEnable;
    if (now == reachable_) {
        return;
    }
    reachable_ = now;
    emit reachableChanged(now);
}

}  // namespace f2c_cpp
