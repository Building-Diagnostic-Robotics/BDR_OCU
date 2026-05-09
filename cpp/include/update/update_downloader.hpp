/**
 * @file update_downloader.hpp
 * @brief Downloads the OTA .deb asset + its SHA256 sidecar and verifies them.
 *
 * Headless networking class. UI consumers (modal) connect to:
 *   - progressChanged(received, total)  — drive a progress bar
 *   - downloadComplete(absDebPath)      — verified .deb is on disk
 *   - downloadFailed(reason)            — show an error state
 *   - cancelled()                       — operator aborted
 *
 * Failure handling is hard-fail: a corrupted download or a SHA256 mismatch
 * does NOT auto-retry. The operator decides whether to re-trigger.
 *
 * Pre-flight checks:
 *   - QStandardPaths::CacheLocation is writable
 *   - At least kMinFreeBytes free on the cache filesystem
 *
 * The download file is written as <asset>.part and atomically renamed to
 * <asset> only after SHA256 verification passes — never leaves an unverified
 * .deb at its final path for apply_update.sh to pick up by accident.
 */

#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>

#include "update/update_types.hpp"

class QFile;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace f2c_cpp::update {

class UpdateDownloader : public QObject {
    Q_OBJECT

public:
    explicit UpdateDownloader(QObject* parent = nullptr);
    ~UpdateDownloader() override;

    /// Required free space (bytes) on the cache filesystem before download.
    /// Allows .deb (~150 MB) + dpkg unpack staging headroom.
    static constexpr qint64 kMinFreeBytes = 500LL * 1024LL * 1024LL;

    /**
     * @brief Begin the download/verify pipeline for `info`. No-op if a
     *        download is already running.
     */
    void start(const VersionInfo& info);

    /**
     * @brief Abort an in-flight download, delete the .part file, emit
     *        cancelled().
     */
    void cancel();

    /// @return true between start() and the first terminal signal.
    bool isRunning() const;

    /**
     * @brief Pure-function parse of a `sha256sum`-style sidecar.
     *
     * Sidecar format (one or many lines):
     *     <64-hex-digest>  <filename>
     *
     * @param sidecar    Raw bytes from the .sha256 file.
     * @param assetName  Filename to look up. If empty, the first hex digest
     *                   on the first line is returned.
     * @return Lowercase hex digest, or empty string on parse failure.
     */
    static QString parseSha256Sidecar(const QByteArray& sidecar,
                                      const QString& assetName = QString());

signals:
    void progressChanged(qint64 received, qint64 total);
    void downloadComplete(const QString& debPath);
    void downloadFailed(const QString& reason);
    void cancelled();

    /**
     * @brief A network failure forced an automatic retry. UI should surface
     *        a "Retrying (N of M) in X s..." banner under the progress bar.
     *
     * Emitted before the retry sleep, once per retry attempt.
     */
    void retryScheduled(int attempt, int totalAttempts, int delayMs);

private slots:
    void onDebReadyRead();
    void onDebFinished();
    void onSha256Finished();
    void onStallTimeout();
    void onTotalCeilingHit();
    void onRetryDelayElapsed();

private:
    enum class State {
        Idle,
        DownloadingDeb,
        DownloadingSha256,
        Verifying,
        Done,
        Failed,
        Cancelled,
    };

    void startDebDownload();
    void startSha256Download();
    void verifyAndFinish();
    bool ensureCacheDir(QString* err);
    bool hasEnoughDiskSpace(QString* err) const;
    void cleanupPartFile();
    void emitFailure(const QString& reason);
    void resetState();
    /// Decide whether a transient network/stall failure should auto-retry,
    /// or escalate to a hard downloadFailed. Returns true if a retry was
    /// scheduled (caller must NOT emit downloadFailed in that case).
    bool maybeScheduleRetry(const QString& reason);
    /// Sweep older `bdr-coverage-planner_*_amd64.deb` files in the cache dir
    /// after a successful verify so disk usage doesn't grow over many updates.
    void purgeOlderCachedDebs();

    QNetworkAccessManager* nam_ = nullptr;

    State state_ = State::Idle;
    VersionInfo info_;

    QString cache_dir_;     ///< absolute, e.g. <CacheLocation>/updates
    QString target_path_;   ///< <cache_dir>/<assetName>
    QString part_path_;     ///< target_path + ".part"

    QFile* deb_file_ = nullptr;
    QPointer<QNetworkReply> deb_reply_;

    QPointer<QNetworkReply> sha256_reply_;
    QByteArray sha256_payload_;

    QTimer* stall_timer_ = nullptr;
    QTimer* total_ceiling_timer_ = nullptr;
    QTimer* retry_delay_timer_ = nullptr;

    int retry_attempt_ = 0;  ///< 0 = first try; 1..kMaxRetries = retries
    QString last_failure_reason_;
};

}  // namespace f2c_cpp::update
