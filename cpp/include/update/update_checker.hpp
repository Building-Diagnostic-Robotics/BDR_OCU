/**
 * @file update_checker.hpp
 * @brief Polls the BDR_OCU GitHub Releases API for new builds.
 *
 * Owns a single QNetworkAccessManager and a polling QTimer. Emits one of
 * three signals per poll cycle: updateAvailable / noUpdateAvailable /
 * checkFailed. The downstream UI (UpdateBanner, UpdateModal) attaches to
 * these signals and decides what to render.
 *
 * Failure handling is silent by design: a checkFailed signal does NOT pop a
 * dialog. The OCU runs offline most of the time (in the field), so transient
 * network errors are expected and the operator should never be nagged.
 */

#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>

#include "update/update_types.hpp"

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace f2c_cpp::update {

class UpdateChecker : public QObject {
    Q_OBJECT

public:
    explicit UpdateChecker(QObject* parent = nullptr);
    ~UpdateChecker() override;

    /**
     * @brief Schedule the first check (after kFirstCheckDelayMs) and start
     *        the rolling poll timer (kPollIntervalMs).
     *
     * Idempotent — calling start() while already running is a no-op.
     */
    void start();

    /**
     * @brief Cancel the poll timer and abort any in-flight network reply.
     */
    void stop();

    /**
     * @brief Trigger an immediate check, bypassing the poll timer's schedule.
     *        Suitable for a "Check for updates now" UI affordance.
     *
     * No-op if a check is already in flight.
     */
    void checkNow();

    /// @return The build's embedded short SHA with any "-dirty" suffix stripped.
    QString currentSha() const;

    /// Persist a snooze end time. Subsequent updateAvailable emissions are
    /// suppressed until QDateTime::currentMSecsSinceEpoch() >= epochMs.
    void setSnoozedUntil(qint64 epochMs);

    /// @return The current snooze deadline (ms epoch), or 0 if no snooze set.
    qint64 snoozedUntil() const;

    /// @return True if a snooze is in effect right now.
    bool isSnoozed() const;

    /// Reset any active snooze.
    void clearSnooze();

    /**
     * @brief Pure-function compare of the embedded SHA against a remote one.
     *        Exposed for unit testing the prefix-match logic in isolation.
     *
     * @param embeddedShortSha  e.g. "fb662d4" (post-strip of -dirty).
     * @param remoteFullOrShort full or short SHA from `target_commitish` /
     *                          `tag_name`.
     * @return true if the embedded build is older (different SHA).
     */
    static bool isUpdateNewer(const QString& embeddedShortSha,
                              const QString& remoteFullOrShort);

    /**
     * @brief Pure-function parse of a Releases API JSON payload into a
     *        VersionInfo. Exposed for unit testing without spinning up a
     *        QNetworkAccessManager.
     *
     * @param json     Raw JSON returned by the GitHub Releases endpoint.
     * @param[out] err Populated with a human-readable failure reason if the
     *                 payload is malformed or missing required fields.
     * @return Parsed VersionInfo on success; empty struct on failure (err set).
     */
    static VersionInfo parseReleaseJson(const QByteArray& json, QString* err);

signals:
    /// Fired when a poll finds a SHA different from the embedded one AND no
    /// snooze is active. Carries the parsed release details.
    void updateAvailable(const f2c_cpp::update::VersionInfo& info);

    /// Fired when a poll finds the same SHA the app was built from.
    void noUpdateAvailable();

    /// Fired on any failure path (network, JSON parse, missing asset, etc.).
    /// Reason is human-readable for logs; UI MUST stay silent.
    void checkFailed(const QString& reason);

private slots:
    void performCheck();
    void onReplyFinished();
    void onRequestTimeout();

    /// Re-emit updateAvailable from the persisted release JSON if one
    /// exists and still represents an upgrade. Invoked from start() on
    /// the next event-loop tick so UI consumers have time to connect
    /// signals first. Silent (no signal at all) when no replay applies —
    /// the regular poll path takes over from there.
    void replayPersistedRelease();

private:
    void scheduleNextPoll(int delayMs);
    void abortInFlight();
    void handleSuccess(const QByteArray& payload);
    void handleFailure(const QString& reason);

    QNetworkAccessManager* nam_ = nullptr;
    QPointer<QNetworkReply> in_flight_;
    QTimer* poll_timer_ = nullptr;
    QTimer* request_timeout_ = nullptr;

    bool started_ = false;
    int consecutive_failures_ = 0;
    int current_backoff_ms_ = kPollIntervalMs;
};

}  // namespace f2c_cpp::update
