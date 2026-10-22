/**
 * @file scan_reports.hpp
 * @brief Tell BDR about every scan the next time this laptop is online.
 *
 * The work is in scripts/scan_report.py; this side only starts it.
 *
 *  - At mission close, MissionController starts `scan_report.py collect`
 *    (detached, like ros_log_bundle.py). It finds the robot's Mission
 *    folder by this OCU log id, copies mission_config.json and small robot
 *    log archives over the radio, adds the OCU log, and leaves one report
 *    in the outbox. A robot that cannot be reached leaves it pending.
 *  - ScanReportScheduler runs `scan_report.py send` at start-up and every
 *    few minutes. It fetches pending reports (never while a scan is up),
 *    uploads ready ones through /presign with the robot's cloud
 *    credentials, and writes the laptop's last-online note.
 *
 * Nothing here is operator-facing: no dialogs, no banners. Failures only
 * go to the app log; a report waits in the outbox until it can be sent.
 *
 * Qt Core only; unit-tested (tests/scan_reports_tests.cpp).
 */
#pragma once

#include <QObject>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>

class QProcess;

namespace f2c_cpp::scan_reports {

/// AppDataLocation/scan_outbox (created if missing). Empty on failure.
QString outboxDir();

/// Installed beside uploader.py; dev builds use cpp/scripts/. Empty if missing.
QString resolveScript();

struct CollectRequest {
    QString log_id;
    QString client_id;
    QString robot_id;
    QString host;
    QString ssh_user = QStringLiteral("roofus");
    QString data_root = QStringLiteral("/R_DATA");
    QString logs_dir;
    QString building;
    QString operator_name;
    QString ocu_version;
};

/// Why a collect cannot start, or empty when it can.
QString collectBlocker(const CollectRequest& request);

/// python3 argv (after "python3") for `scan_report.py collect`.
QStringList collectArgs(const QString& script, const QString& outbox,
                        const CollectRequest& request);

/// python3 argv for `scan_report.py send`. `no_fetch` while a scan is up:
/// nothing may use the radio link to the robot then.
QStringList sendArgs(const QString& script, const QString& outbox,
                     const QString& robot_id, const QString& ocu_version,
                     bool no_fetch);

/// `base` plus the BDR_CLOUD_* variables uploader.py reads.
QProcessEnvironment sendEnvironment(const QProcessEnvironment& base,
                                    const QString& api_base,
                                    const QString& client_id,
                                    const QString& device_token);

/// Starts `scan_report.py collect` detached. False (with `why`) when it
/// could not start; the scan is then reported only by its upload.
bool startCollect(const CollectRequest& request, QString* why = nullptr);

/// One `send` run, resolved fresh each time (the paired robot can change).
struct SendRun {
    bool valid = false;
    QString reason;          ///< why there is no run, when !valid
    QString program = QStringLiteral("python3");
    QStringList args;
    QProcessEnvironment env;
};

class ScanReportScheduler : public QObject {
    Q_OBJECT

public:
    using Provider = std::function<SendRun()>;

    explicit ScanReportScheduler(Provider provider, QObject* parent = nullptr);
    ~ScanReportScheduler() override;

    /// First run after `first_delay_ms`, then every `interval_ms`.
    void start(int first_delay_ms = 60 * 1000, int interval_ms = 5 * 60 * 1000);
    /// Run now unless a run is still going (runs never overlap).
    void runNow();
    bool running() const;

signals:
    /// One line per finished run, for the app log.
    void runFinished(int exit_code, const QString& summary);

private:
    Provider provider_;
    QTimer timer_;
    QProcess* proc_ = nullptr;
    QString output_;
};

}  // namespace f2c_cpp::scan_reports
