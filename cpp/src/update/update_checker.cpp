#include "update/update_checker.hpp"

#include <QByteArray>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <algorithm>

#include "update/update_log.hpp"
#include "update/update_settings.hpp"
#include "version_info.hpp"

namespace f2c_cpp::update {

UpdateChecker::UpdateChecker(QObject* parent) : QObject(parent) {
    nam_ = new QNetworkAccessManager(this);

    poll_timer_ = new QTimer(this);
    poll_timer_->setSingleShot(false);
    connect(poll_timer_, &QTimer::timeout, this, &UpdateChecker::performCheck);

    request_timeout_ = new QTimer(this);
    request_timeout_->setSingleShot(true);
    connect(request_timeout_, &QTimer::timeout,
            this, &UpdateChecker::onRequestTimeout);
}

UpdateChecker::~UpdateChecker() {
    abortInFlight();
}

void UpdateChecker::start() {
    if (started_) {
        return;
    }
    started_ = true;
    current_backoff_ms_ = kPollIntervalMs;
    consecutive_failures_ = 0;
    QTimer::singleShot(kFirstCheckDelayMs, this, &UpdateChecker::performCheck);
}

void UpdateChecker::stop() {
    started_ = false;
    poll_timer_->stop();
    request_timeout_->stop();
    abortInFlight();
}

void UpdateChecker::checkNow() {
    if (in_flight_) {
        return;
    }
    performCheck();
}

QString UpdateChecker::currentSha() const {
    QString sha = QString::fromLatin1(version::kAppCommitSha);
    if (sha.endsWith(QStringLiteral("-dirty"))) {
        sha.chop(static_cast<int>(QStringLiteral("-dirty").size()));
    }
    return sha;
}

void UpdateChecker::setSnoozedUntil(qint64 epochMs) {
    QSettings s = settings();
    s.setValue(QString::fromLatin1(kKeySnoozeUntilMs), epochMs);
}

qint64 UpdateChecker::snoozedUntil() const {
    QSettings s = settings();
    return s.value(QString::fromLatin1(kKeySnoozeUntilMs), 0).toLongLong();
}

bool UpdateChecker::isSnoozed() const {
    return QDateTime::currentMSecsSinceEpoch() < snoozedUntil();
}

void UpdateChecker::clearSnooze() {
    setSnoozedUntil(0);
}

bool UpdateChecker::isUpdateNewer(const QString& embeddedShortSha,
                                  const QString& remoteFullOrShort) {
    if (embeddedShortSha.isEmpty() || remoteFullOrShort.isEmpty()) {
        return false;
    }
    if (embeddedShortSha == QStringLiteral("unknown")) {
        // Embedded build wasn't stamped (dev build outside git). Don't surprise
        // the operator with a forced update prompt.
        return false;
    }

    // GitHub's target_commitish is a full 40-char SHA when the release was
    // created from a commit; tag_name in our scheme is "v-<short>" or "latest".
    // Normalize: take the first kEmbeddedLen chars of whichever side is longer
    // and compare against the shorter as a prefix.
    const int embeddedLen = embeddedShortSha.size();
    const QString remoteHead = remoteFullOrShort.left(embeddedLen);
    return remoteHead.compare(embeddedShortSha, Qt::CaseInsensitive) != 0;
}

VersionInfo UpdateChecker::parseReleaseJson(const QByteArray& json,
                                            QString* err) {
    auto fail = [err](const QString& msg) {
        if (err) *err = msg;
        return VersionInfo{};
    };

    QJsonParseError parseErr{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
        return fail(QStringLiteral("malformed JSON: %1").arg(parseErr.errorString()));
    }

    const QJsonObject obj = doc.object();
    VersionInfo out;
    out.tag = obj.value(QStringLiteral("tag_name")).toString();
    out.commitSha = obj.value(QStringLiteral("target_commitish")).toString();
    out.releaseNotes = obj.value(QStringLiteral("body")).toString();
    out.publishedAtIso8601 = obj.value(QStringLiteral("published_at")).toString();

    const QJsonArray assets = obj.value(QStringLiteral("assets")).toArray();
    QString sha256_for_deb;
    for (const QJsonValue& v : assets) {
        const QJsonObject a = v.toObject();
        const QString name = a.value(QStringLiteral("name")).toString();
        const QString url = a.value(QStringLiteral("browser_download_url")).toString();
        if (name.endsWith(QString::fromLatin1(kDebAssetSuffix))) {
            out.assetName = name;
            out.downloadUrl = url;
            out.sizeBytes = static_cast<qint64>(
                a.value(QStringLiteral("size")).toDouble(0.0));
            sha256_for_deb = name + QString::fromLatin1(kSha256SidecarSuffix);
        }
    }

    if (out.downloadUrl.isEmpty()) {
        return fail(QStringLiteral("release has no %1 asset")
                        .arg(QString::fromLatin1(kDebAssetSuffix)));
    }

    // Second pass: locate the matching .sha256 sidecar by exact-name lookup.
    if (!sha256_for_deb.isEmpty()) {
        for (const QJsonValue& v : assets) {
            const QJsonObject a = v.toObject();
            if (a.value(QStringLiteral("name")).toString() == sha256_for_deb) {
                out.sha256Url =
                    a.value(QStringLiteral("browser_download_url")).toString();
                break;
            }
        }
    }

    if (out.commitSha.isEmpty()) {
        // Fall back to tag_name; some release-creation paths leave
        // target_commitish empty when the tag is rolling.
        out.commitSha = out.tag;
    }

    if (err) err->clear();
    return out;
}

void UpdateChecker::performCheck() {
    if (in_flight_) {
        return;
    }

    const QUrl url(version::releasesApiUrl());
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QString::fromLatin1(kUserAgent));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("X-GitHub-Api-Version", "2022-11-28");

    // Send the previously cached ETag so GitHub can answer 304 Not Modified
    // when nothing has changed. 304 responses don't count against the
    // 60/hr unauthenticated rate limit, which matters when many OCUs
    // share a NAT'd field-office gateway.
    const QString etag = settings()
                             .value(QString::fromLatin1(kKeyLastEtag))
                             .toString();
    if (!etag.isEmpty()) {
        req.setRawHeader("If-None-Match", etag.toLatin1());
    }

    QNetworkReply* reply = nam_->get(req);
    in_flight_ = reply;
    connect(reply, &QNetworkReply::finished,
            this, &UpdateChecker::onReplyFinished);

    request_timeout_->start(kRequestTimeoutMs);
}

void UpdateChecker::onReplyFinished() {
    request_timeout_->stop();

    QNetworkReply* reply = in_flight_.data();
    if (!reply) {
        return;  // aborted
    }
    in_flight_.clear();
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError
        && reply->error() != QNetworkReply::ContentNotFoundError) {
        // ContentNotFoundError = 304 in some Qt builds; let the status check
        // below handle it normally.
        handleFailure(QStringLiteral("network error: %1").arg(reply->errorString()));
        return;
    }

    const int http_status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    // 304 Not Modified: cached ETag matched, nothing to do. Reset failure
    // counters and schedule the next normal poll. Costs ~0 of our hourly
    // budget so this is the desired steady state.
    if (http_status == 304) {
        log::info("checker", QStringLiteral("304 Not Modified (etag hit)"));
        consecutive_failures_ = 0;
        current_backoff_ms_ = kPollIntervalMs;
        scheduleNextPoll(kPollIntervalMs);
        emit noUpdateAvailable();
        return;
    }

    // 403 = rate limit (or auth-gated repo). Either way, back off hard so we
    // stop hammering the API.
    if (http_status == 403) {
        log::warn("checker",
                  QStringLiteral("HTTP 403 (rate-limited?), cooling down for %1 min")
                      .arg(kRateLimitCooldownMs / 60000));
        consecutive_failures_ = 0;  // not a real failure, don't escalate backoff
        current_backoff_ms_ = kPollIntervalMs;
        scheduleNextPoll(kRateLimitCooldownMs);
        emit checkFailed(QStringLiteral("rate limited"));
        return;
    }

    if (http_status < 200 || http_status >= 300) {
        handleFailure(QStringLiteral("HTTP %1").arg(http_status));
        return;
    }

    // Persist the ETag for the next request so we can earn 304s.
    const QByteArray new_etag = reply->rawHeader("ETag");
    if (!new_etag.isEmpty()) {
        settings().setValue(QString::fromLatin1(kKeyLastEtag),
                            QString::fromLatin1(new_etag));
    }

    handleSuccess(reply->readAll());
}

void UpdateChecker::onRequestTimeout() {
    if (!in_flight_) {
        return;
    }
    in_flight_->abort();  // triggers onReplyFinished with OperationCanceledError
}

void UpdateChecker::scheduleNextPoll(int delayMs) {
    if (!started_) {
        return;
    }
    poll_timer_->start(delayMs);
}

void UpdateChecker::abortInFlight() {
    if (in_flight_) {
        in_flight_->abort();
        in_flight_.clear();
    }
}

void UpdateChecker::handleSuccess(const QByteArray& payload) {
    QString err;
    const VersionInfo info = parseReleaseJson(payload, &err);
    if (info.downloadUrl.isEmpty()) {
        handleFailure(err.isEmpty() ? QStringLiteral("empty release info") : err);
        return;
    }

    consecutive_failures_ = 0;
    current_backoff_ms_ = kPollIntervalMs;
    scheduleNextPoll(kPollIntervalMs);

    if (!isUpdateNewer(currentSha(), info.commitSha)) {
        emit noUpdateAvailable();
        return;
    }

    if (isSnoozed()) {
        // A new build exists but the operator told us to stay quiet. Treat as
        // "no update" from the UI's perspective; banner stays hidden.
        emit noUpdateAvailable();
        return;
    }

    emit updateAvailable(info);
}

void UpdateChecker::handleFailure(const QString& reason) {
    ++consecutive_failures_;
    current_backoff_ms_ =
        std::min(current_backoff_ms_ * 2, kBackoffMaxMs);
    log::warn("checker",
              QStringLiteral("check failed: %1 (consec=%2, next in %3 s)")
                  .arg(reason)
                  .arg(consecutive_failures_)
                  .arg(current_backoff_ms_ / 1000));
    scheduleNextPoll(current_backoff_ms_);
    emit checkFailed(reason);
}

}  // namespace f2c_cpp::update
