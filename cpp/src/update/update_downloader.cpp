#include "update/update_downloader.hpp"

#include <QByteArrayList>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QString>
#include <QStringLiteral>
#include <QTimer>
#include <QUrl>
#include <algorithm>

#include "update/update_log.hpp"

namespace f2c_cpp::update {

UpdateDownloader::UpdateDownloader(QObject* parent) : QObject(parent) {
    nam_ = new QNetworkAccessManager(this);

    stall_timer_ = new QTimer(this);
    stall_timer_->setSingleShot(true);
    connect(stall_timer_, &QTimer::timeout,
            this, &UpdateDownloader::onStallTimeout);

    total_ceiling_timer_ = new QTimer(this);
    total_ceiling_timer_->setSingleShot(true);
    connect(total_ceiling_timer_, &QTimer::timeout,
            this, &UpdateDownloader::onTotalCeilingHit);

    retry_delay_timer_ = new QTimer(this);
    retry_delay_timer_->setSingleShot(true);
    connect(retry_delay_timer_, &QTimer::timeout,
            this, &UpdateDownloader::onRetryDelayElapsed);
}

UpdateDownloader::~UpdateDownloader() {
    if (deb_reply_) {
        deb_reply_->abort();
    }
    if (sha256_reply_) {
        sha256_reply_->abort();
    }
}

bool UpdateDownloader::isRunning() const {
    return state_ == State::DownloadingDeb
        || state_ == State::DownloadingSha256
        || state_ == State::Verifying;
}

void UpdateDownloader::start(const VersionInfo& info) {
    if (isRunning()) {
        return;
    }
    if (info.downloadUrl.isEmpty() || info.assetName.isEmpty()) {
        emit downloadFailed(QStringLiteral("Missing download URL or asset name"));
        return;
    }
    if (info.sha256Url.isEmpty()) {
        // Locked spec: SHA256 verification is mandatory. Refuse to even start
        // a download we can't verify.
        emit downloadFailed(QStringLiteral(
            "Release has no .sha256 sidecar; refusing to install unverified build"));
        return;
    }

    info_ = info;

    QString err;
    if (!ensureCacheDir(&err)) {
        emit downloadFailed(err);
        return;
    }
    if (!hasEnoughDiskSpace(&err)) {
        emit downloadFailed(err);
        return;
    }

    target_path_ = cache_dir_ + QStringLiteral("/") + info.assetName;
    part_path_ = target_path_ + QStringLiteral(".part");

    // Clear stale .part / target files so a corrupted previous attempt
    // doesn't masquerade as a verified asset.
    QFile::remove(part_path_);
    QFile::remove(target_path_);

    retry_attempt_ = 0;
    last_failure_reason_.clear();
    state_ = State::DownloadingDeb;
    log::info("downloader",
              QStringLiteral("start: asset=%1 size=%2 url=%3")
                  .arg(info.assetName)
                  .arg(info.sizeBytes)
                  .arg(info.downloadUrl));
    total_ceiling_timer_->start(kDebTotalCeilingMs);
    startDebDownload();
}

void UpdateDownloader::cancel() {
    if (!isRunning()) {
        return;
    }
    state_ = State::Cancelled;
    stall_timer_->stop();
    total_ceiling_timer_->stop();
    retry_delay_timer_->stop();
    if (deb_reply_) {
        deb_reply_->abort();
    }
    if (sha256_reply_) {
        sha256_reply_->abort();
    }
    if (deb_file_) {
        deb_file_->close();
        deb_file_->deleteLater();
        deb_file_ = nullptr;
    }
    cleanupPartFile();
    log::info("downloader", QStringLiteral("cancelled by operator"));
    emit cancelled();
    resetState();
}

QString UpdateDownloader::parseSha256Sidecar(const QByteArray& sidecar,
                                             const QString& assetName) {
    const QList<QByteArray> lines = sidecar.split('\n');
    for (const QByteArray& raw : lines) {
        const QByteArray line = raw.trimmed();
        if (line.isEmpty() || line.startsWith('#')) {
            continue;
        }
        const QList<QByteArray> cols = line.split(' ');
        QByteArrayList compact;
        for (const QByteArray& c : cols) {
            if (!c.trimmed().isEmpty()) {
                compact.append(c.trimmed());
            }
        }
        if (compact.size() < 1) {
            continue;
        }
        const QString hex = QString::fromLatin1(compact.first()).toLower();
        if (hex.size() != 64) {
            continue;
        }
        if (assetName.isEmpty()) {
            return hex;
        }
        // sha256sum prefixes the filename with a '*' for binary mode; tolerate
        // both '*<name>' and '<name>'.
        for (int i = 1; i < compact.size(); ++i) {
            QString name = QString::fromLatin1(compact[i]);
            if (name.startsWith(QChar('*'))) {
                name = name.mid(1);
            }
            if (name == assetName || name.endsWith(QStringLiteral("/") + assetName)) {
                return hex;
            }
        }
    }
    return {};
}

void UpdateDownloader::startDebDownload() {
    // Each retry restarts from byte 0 in v1 (kRangeResumeEnabled = false).
    if (deb_file_) {
        deb_file_->close();
        deb_file_->deleteLater();
        deb_file_ = nullptr;
    }
    QFile::remove(part_path_);

    deb_file_ = new QFile(part_path_, this);
    if (!deb_file_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emitFailure(QStringLiteral("Cannot open %1 for writing").arg(part_path_));
        return;
    }

    QNetworkRequest req((QUrl(info_.downloadUrl)));
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QString::fromLatin1(kUserAgent));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = nam_->get(req);
    deb_reply_ = reply;
    connect(reply, &QNetworkReply::readyRead,
            this, &UpdateDownloader::onDebReadyRead);
    connect(reply, &QNetworkReply::downloadProgress,
            this, &UpdateDownloader::progressChanged);
    connect(reply, &QNetworkReply::finished,
            this, &UpdateDownloader::onDebFinished);

    // Stall watchdog: if no readyRead arrives within kStallTimeoutMs, abort
    // and let maybeScheduleRetry decide whether to escalate.
    stall_timer_->start(kStallTimeoutMs);
}

void UpdateDownloader::onDebReadyRead() {
    if (!deb_reply_ || !deb_file_) {
        return;
    }
    // Bytes are flowing → the connection is alive. Restart the stall watchdog.
    stall_timer_->start(kStallTimeoutMs);
    const QByteArray chunk = deb_reply_->readAll();
    if (deb_file_->write(chunk) != chunk.size()) {
        log::error("downloader",
                   QStringLiteral("disk write failed at %1 bytes")
                       .arg(deb_file_->size()));
        // Disk-full / IO error: not retryable, fail loud.
        deb_reply_->disconnect(this);
        deb_reply_->abort();
        deb_reply_.clear();
        emitFailure(QStringLiteral("Disk write failed (out of space?)"));
    }
}

void UpdateDownloader::onDebFinished() {
    QNetworkReply* reply = deb_reply_.data();
    if (!reply) {
        return;
    }
    deb_reply_.clear();
    reply->deleteLater();
    stall_timer_->stop();

    // Drain anything still buffered post-finished.
    if (deb_file_ && reply->bytesAvailable() > 0) {
        deb_file_->write(reply->readAll());
    }

    if (state_ == State::Cancelled) {
        return;
    }

    if (deb_file_) {
        deb_file_->flush();
        deb_file_->close();
    }

    if (reply->error() != QNetworkReply::NoError) {
        const QString reason = QStringLiteral("Download error: %1")
                                   .arg(reply->errorString());
        log::warn("downloader", reason);
        if (maybeScheduleRetry(reason)) {
            return;
        }
        emitFailure(reason);
        return;
    }

    log::info("downloader",
              QStringLiteral(".deb fetched (%1 bytes), starting SHA256 sidecar")
                  .arg(deb_file_ ? deb_file_->size() : -1));
    state_ = State::DownloadingSha256;
    startSha256Download();
}

void UpdateDownloader::startSha256Download() {
    QNetworkRequest req((QUrl(info_.sha256Url)));
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QString::fromLatin1(kUserAgent));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = nam_->get(req);
    sha256_reply_ = reply;
    sha256_payload_.clear();
    connect(reply, &QNetworkReply::finished,
            this, &UpdateDownloader::onSha256Finished);
}

void UpdateDownloader::onSha256Finished() {
    QNetworkReply* reply = sha256_reply_.data();
    if (!reply) {
        return;
    }
    sha256_reply_.clear();
    reply->deleteLater();

    if (state_ == State::Cancelled) {
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        emitFailure(QStringLiteral("SHA256 sidecar download failed: %1")
                        .arg(reply->errorString()));
        return;
    }
    sha256_payload_ = reply->readAll();
    state_ = State::Verifying;
    verifyAndFinish();
}

void UpdateDownloader::verifyAndFinish() {
    const QString expected = parseSha256Sidecar(sha256_payload_, info_.assetName);
    if (expected.isEmpty()) {
        emitFailure(QStringLiteral("Could not parse SHA256 sidecar"));
        return;
    }

    QFile f(part_path_);
    if (!f.open(QIODevice::ReadOnly)) {
        emitFailure(QStringLiteral("Cannot read downloaded file for verification"));
        return;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&f)) {
        emitFailure(QStringLiteral("Failed reading file during hashing"));
        return;
    }
    f.close();

    const QString actual = QString::fromLatin1(hash.result().toHex()).toLower();
    if (actual != expected) {
        emitFailure(QStringLiteral(
            "SHA256 mismatch: expected %1 got %2 (corrupted or tampered .deb)")
                        .arg(expected, actual));
        return;
    }

    // Atomic rename so the consumer never sees an unverified file at
    // target_path_.
    QFile::remove(target_path_);
    if (!QFile::rename(part_path_, target_path_)) {
        emitFailure(QStringLiteral("Could not rename %1 -> %2")
                        .arg(part_path_, target_path_));
        return;
    }

    state_ = State::Done;
    total_ceiling_timer_->stop();
    log::info("downloader",
              QStringLiteral("verified + renamed -> %1").arg(target_path_));
    purgeOlderCachedDebs();
    emit downloadComplete(target_path_);
    resetState();
}

void UpdateDownloader::onStallTimeout() {
    if (state_ != State::DownloadingDeb) {
        return;
    }
    log::warn("downloader",
              QStringLiteral("stall timeout (%1 s no progress)")
                  .arg(kStallTimeoutMs / 1000));
    if (deb_reply_) {
        deb_reply_->disconnect(this);
        deb_reply_->abort();
        deb_reply_->deleteLater();
        deb_reply_.clear();
    }
    if (deb_file_) {
        deb_file_->close();
    }
    const QString reason = QStringLiteral("Download stalled (no data for %1 s)")
                               .arg(kStallTimeoutMs / 1000);
    if (maybeScheduleRetry(reason)) {
        return;
    }
    emitFailure(reason);
}

void UpdateDownloader::onTotalCeilingHit() {
    if (state_ == State::Idle || state_ == State::Done
        || state_ == State::Cancelled) {
        return;
    }
    log::error("downloader",
               QStringLiteral("total time ceiling hit (%1 min) — giving up")
                   .arg(kDebTotalCeilingMs / 60000));
    if (deb_reply_) {
        deb_reply_->abort();
    }
    if (sha256_reply_) {
        sha256_reply_->abort();
    }
    // Total ceiling is a hard fail — no further retries even if budget left.
    retry_attempt_ = kMaxRetries;
    emitFailure(QStringLiteral(
        "Update download exceeded %1 minute ceiling")
                    .arg(kDebTotalCeilingMs / 60000));
}

void UpdateDownloader::onRetryDelayElapsed() {
    if (state_ != State::DownloadingDeb) {
        return;  // user cancelled while we were sleeping
    }
    log::info("downloader",
              QStringLiteral("retry attempt %1/%2 begin")
                  .arg(retry_attempt_).arg(kMaxRetries));
    startDebDownload();
}

bool UpdateDownloader::maybeScheduleRetry(const QString& reason) {
    if (retry_attempt_ >= kMaxRetries) {
        return false;
    }
    ++retry_attempt_;

    // Exponential delay: kRetryBaseMs * 3^(attempt-1) → 5 s, 15 s, 45 s.
    int delay_ms = kRetryBaseMs;
    for (int i = 1; i < retry_attempt_; ++i) {
        delay_ms *= 3;
    }

    last_failure_reason_ = reason;
    log::warn("downloader",
              QStringLiteral("scheduling retry %1/%2 in %3 s (reason: %4)")
                  .arg(retry_attempt_).arg(kMaxRetries)
                  .arg(delay_ms / 1000).arg(reason));
    emit retryScheduled(retry_attempt_, kMaxRetries, delay_ms);
    retry_delay_timer_->start(delay_ms);
    return true;
}

void UpdateDownloader::purgeOlderCachedDebs() {
    QDir d(cache_dir_);

    // Two passes:
    //   .deb  files       — keep the newest two (just-downloaded + one
    //                        generation back so Phase 9 rollback can
    //                        re-install the previous version).
    //   .deb.part files   — keep nothing except the just-downloaded one
    //                        (which won't exist post-verify; we already
    //                        atomic-renamed it to .deb).
    //
    // "Newest two" is determined by mtime, which is monotone for our
    // download flow because `start()` opens .part with truncation (so
    // mtime is set to the start of the latest download).
    const QFileInfoList debs = d.entryInfoList(
        QStringList() << QStringLiteral("bdr-coverage-planner_*_amd64.deb"),
        QDir::Files | QDir::NoDotAndDotDot,
        QDir::Time);  // Time = newest first

    int keep_remaining = 2;
    for (const QFileInfo& fi : debs) {
        if (fi.absoluteFilePath() == target_path_) {
            // Always keep what we just verified, even if the OS time
            // is skewed and it's not newest-by-mtime.
            keep_remaining = qMax(1, keep_remaining - 1);
            continue;
        }
        if (keep_remaining > 0) {
            keep_remaining--;
            log::info(
                "downloader",
                QStringLiteral("retained previous cached .deb for rollback: %1")
                    .arg(fi.fileName()));
            continue;
        }
        if (QFile::remove(fi.absoluteFilePath())) {
            log::info("downloader",
                      QStringLiteral("purged stale cached .deb %1")
                          .arg(fi.fileName()));
        }
    }

    // Stale .part files are never useful (we don't resume — see
    // kRangeResumeEnabled). Sweep them all.
    const QFileInfoList parts = d.entryInfoList(
        QStringList() << QStringLiteral("bdr-coverage-planner_*_amd64.deb.part"),
        QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo& fi : parts) {
        if (fi.absoluteFilePath() == part_path_) continue;
        if (QFile::remove(fi.absoluteFilePath())) {
            log::info("downloader",
                      QStringLiteral("purged stale .part file %1")
                          .arg(fi.fileName()));
        }
    }
}

bool UpdateDownloader::ensureCacheDir(QString* err) {
    cache_dir_ = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cache_dir_.isEmpty()) {
        if (err) *err = QStringLiteral("No writable cache location available");
        return false;
    }
    cache_dir_ += QStringLiteral("/updates");
    QDir d;
    if (!d.mkpath(cache_dir_)) {
        if (err) *err = QStringLiteral("Cannot create cache dir %1").arg(cache_dir_);
        return false;
    }
    return true;
}

bool UpdateDownloader::hasEnoughDiskSpace(QString* err) const {
    QStorageInfo info(cache_dir_);
    if (!info.isValid() || !info.isReady()) {
        if (err) *err = QStringLiteral("Cannot stat filesystem at %1").arg(cache_dir_);
        return false;
    }
    if (info.bytesAvailable() < kMinFreeBytes) {
        if (err) *err = QStringLiteral(
            "Less than 500 MB free on cache filesystem (%1 MB available)")
            .arg(info.bytesAvailable() / (1024 * 1024));
        return false;
    }
    return true;
}

void UpdateDownloader::cleanupPartFile() {
    if (deb_file_) {
        deb_file_->close();
        deb_file_->deleteLater();
        deb_file_ = nullptr;
    }
    if (!part_path_.isEmpty()) {
        QFile::remove(part_path_);
    }
}

void UpdateDownloader::emitFailure(const QString& reason) {
    if (state_ == State::Cancelled) {
        return;  // cancel path already announced
    }
    state_ = State::Failed;
    stall_timer_->stop();
    total_ceiling_timer_->stop();
    retry_delay_timer_->stop();
    cleanupPartFile();
    log::error("downloader",
               QStringLiteral("hard fail (attempt=%1/%2): %3")
                   .arg(retry_attempt_).arg(kMaxRetries).arg(reason));
    emit downloadFailed(reason);
    resetState();
}

void UpdateDownloader::resetState() {
    if (deb_file_) {
        deb_file_->close();
        deb_file_->deleteLater();
        deb_file_ = nullptr;
    }
    sha256_payload_.clear();
    retry_attempt_ = 0;
    last_failure_reason_.clear();
    if (state_ != State::Idle) {
        state_ = State::Idle;
    }
    stall_timer_->stop();
    total_ceiling_timer_->stop();
    retry_delay_timer_->stop();
}

}  // namespace f2c_cpp::update
