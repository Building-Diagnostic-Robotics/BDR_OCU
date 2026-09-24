#pragma once

#include <QObject>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace f2c_cpp {

/**
 * Periodic HEAD against Esri imagery. Two successes in a row mark the
 * laptop online; one failure clears it. AppShell starts this on the
 * dashboard and stops it on leaving. A successful site download latches
 * it off for the rest of the session.
 */
class ImageryReachabilityProbe : public QObject {
    Q_OBJECT

public:
    explicit ImageryReachabilityProbe(QObject* parent = nullptr);

    void start();
    void stop();
    /** Download finished. Later start() calls do nothing. */
    void noteImageryDownloaded();

    bool reachable() const { return reachable_; }

signals:
    void reachableChanged(bool reachable);

private:
    void tick();
    void onFinished(QNetworkReply* reply);

    static constexpr int kIntervalMs = 5000;
    static constexpr int kTimeoutMs = 4000;
    static constexpr int kSuccessesToEnable = 2;

    QNetworkAccessManager* nam_ = nullptr;
    QTimer* timer_ = nullptr;
    QNetworkReply* inflight_ = nullptr;
    int successes_ = 0;
    bool reachable_ = false;
    bool downloaded_ = false;
};

}  // namespace f2c_cpp
