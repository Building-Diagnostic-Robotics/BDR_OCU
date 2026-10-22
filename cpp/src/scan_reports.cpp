#include "scan_reports.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

namespace f2c_cpp::scan_reports {

QString outboxDir() {
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
        QStringLiteral("/scan_outbox");
    return QDir().mkpath(dir) ? dir : QString();
}

QString resolveScript() {
    QStringList candidates{
        QStringLiteral("/usr/share/bdr-coverage-planner/scan_report.py")};
    const QString app_dir = QCoreApplication::instance()
                                ? QCoreApplication::applicationDirPath()
                                : QString();
    if (!app_dir.isEmpty()) {
        candidates << QDir(app_dir).filePath(QStringLiteral("../scripts/scan_report.py"))
                   << QDir(app_dir).filePath(QStringLiteral("scripts/scan_report.py"));
    }
    for (const QString& c : candidates) {
        const QFileInfo fi(c);
        if (fi.isFile() && fi.isReadable()) {
            return fi.canonicalFilePath();
        }
    }
    return QString();
}

QString collectBlocker(const CollectRequest& r) {
    if (r.log_id.trimmed().isEmpty()) {
        return QStringLiteral("no mission log id");
    }
    if (r.client_id.trimmed().isEmpty()) {
        return QStringLiteral("robots.json has no cloud_client_id for %1").arg(r.robot_id);
    }
    if (r.robot_id.trimmed().isEmpty() || r.host.trimmed().isEmpty()) {
        return QStringLiteral("no robot host");
    }
    return QString();
}

QStringList collectArgs(const QString& script, const QString& outbox,
                        const CollectRequest& r) {
    return {
        script,
        QStringLiteral("collect"),
        QStringLiteral("--outbox"), outbox,
        QStringLiteral("--logs-dir"), r.logs_dir,
        QStringLiteral("--log-id"), r.log_id.trimmed(),
        QStringLiteral("--client-id"), r.client_id.trimmed(),
        QStringLiteral("--robot-id"), r.robot_id.trimmed(),
        QStringLiteral("--host"), r.host.trimmed(),
        QStringLiteral("--user"), r.ssh_user.trimmed().isEmpty() ? QStringLiteral("roofus") : r.ssh_user.trimmed(),
        QStringLiteral("--data-root"), r.data_root.trimmed().isEmpty() ? QStringLiteral("/R_DATA") : r.data_root.trimmed(),
        QStringLiteral("--building"), r.building,
        QStringLiteral("--operator"), r.operator_name,
        QStringLiteral("--ocu-version"), r.ocu_version,
    };
}

QStringList sendArgs(const QString& script, const QString& outbox,
                     const QString& robot_id, const QString& ocu_version,
                     bool no_fetch) {
    QStringList args{
        script,
        QStringLiteral("send"),
        QStringLiteral("--outbox"), outbox,
        QStringLiteral("--robot-id"), robot_id.trimmed(),
        QStringLiteral("--ocu-version"), ocu_version,
    };
    if (no_fetch) {
        args << QStringLiteral("--no-fetch");
    }
    return args;
}

QProcessEnvironment sendEnvironment(const QProcessEnvironment& base,
                                    const QString& api_base,
                                    const QString& client_id,
                                    const QString& device_token) {
    QProcessEnvironment env = base;
    if (!api_base.trimmed().isEmpty()) {
        env.insert(QStringLiteral("BDR_CLOUD_API_BASE"), api_base.trimmed());
    }
    env.insert(QStringLiteral("BDR_CLOUD_CLIENT_ID"), client_id.trimmed());
    env.insert(QStringLiteral("BDR_CLOUD_DEVICE_TOKEN"), device_token.trimmed());
    return env;
}

bool startCollect(const CollectRequest& request, QString* why) {
    const QString blocker = collectBlocker(request);
    const QString script = resolveScript();
    const QString outbox = outboxDir();
    QString reason = blocker;
    if (reason.isEmpty() && script.isEmpty()) {
        reason = QStringLiteral("scan_report.py is not installed");
    }
    if (reason.isEmpty() && outbox.isEmpty()) {
        reason = QStringLiteral("cannot create the scan report outbox");
    }
    if (reason.isEmpty() &&
        !QProcess::startDetached(QStringLiteral("python3"),
                                 collectArgs(script, outbox, request))) {
        reason = QStringLiteral("python3 did not start");
    }
    if (why) {
        *why = reason;
    }
    return reason.isEmpty();
}

ScanReportScheduler::ScanReportScheduler(Provider provider, QObject* parent)
    : QObject(parent), provider_(std::move(provider)) {
    connect(&timer_, &QTimer::timeout, this, &ScanReportScheduler::runNow);
}

ScanReportScheduler::~ScanReportScheduler() {
    if (proc_ && proc_->state() != QProcess::NotRunning) {
        // A cut run loses nothing: the outbox only marks a report sent after
        // S3 accepted it, and the next start sends the rest.
        proc_->kill();
        proc_->waitForFinished(1000);
    }
}

void ScanReportScheduler::start(int first_delay_ms, int interval_ms) {
    timer_.setInterval(qMax(1000, interval_ms));
    timer_.start();
    QTimer::singleShot(qMax(0, first_delay_ms), this, &ScanReportScheduler::runNow);
}

bool ScanReportScheduler::running() const {
    return proc_ && proc_->state() != QProcess::NotRunning;
}

void ScanReportScheduler::runNow() {
    if (running() || !provider_) {
        return;
    }
    const SendRun run = provider_();
    if (!run.valid) {
        emit runFinished(-1, run.reason);
        return;
    }
    if (!proc_) {
        proc_ = new QProcess(this);
        proc_->setProcessChannelMode(QProcess::MergedChannels);
        connect(proc_, &QProcess::readyReadStandardOutput, this, [this]() {
            output_ += QString::fromUtf8(proc_->readAllStandardOutput());
            if (output_.size() > 64 * 1024) {
                output_ = output_.right(32 * 1024);
            }
        });
        connect(proc_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                this, [this](int code, QProcess::ExitStatus status) {
                    output_ += QString::fromUtf8(proc_->readAllStandardOutput());
                    const QStringList lines = output_.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
                    const QString summary = lines.isEmpty() ? QString() : lines.mid(qMax(0, lines.size() - 3)).join(QStringLiteral(" | "));
                    output_.clear();
                    emit runFinished(status == QProcess::NormalExit ? code : -2, summary);
                });
        connect(proc_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                emit runFinished(-3, QStringLiteral("python3 did not start"));
            }
        });
    }
    output_.clear();
    proc_->setProcessEnvironment(run.env);
    proc_->start(run.program, run.args);
}

}  // namespace f2c_cpp::scan_reports
