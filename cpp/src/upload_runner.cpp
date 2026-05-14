/**
 * @file upload_runner.cpp
 * @brief Implementation of UploadStateProbe + UploadRunner.
 */

#include "upload_runner.hpp"

#include <QDebug>
#include <QRegularExpression>
#include <QStringList>
#include <QTimer>

namespace f2c_cpp {

namespace {

// Single quote a string for safe inclusion inside a `bash -lc '...'`
// remote command. We never interpolate operator-controlled data into
// this, but `run_id` derives from operator-typed building names so we
// keep escaping disciplined.
QString shellSingleQuote(const QString& s) {
    QString out = QStringLiteral("'");
    for (const QChar c : s) {
        if (c == QLatin1Char('\'')) {
            out += QStringLiteral("'\\''");
        } else {
            out += c;
        }
    }
    out += QLatin1Char('\'');
    return out;
}

// Default install path for `uploader.py` after `colcon build` lands the
// pilot_control package on the robot. Matches the directory used for
// `finalize_mission_local.py` (same packaging rule in
// `pilot_ws/src/pilot_control/CMakeLists.txt`).
QString defaultRemoteScriptPath(const QString& ssh_user) {
    return QStringLiteral(
               "/home/%1/pilot_ws/install/pilot_control/lib/pilot_control/uploader.py")
        .arg(ssh_user);
}

QStringList sshOptions() {
    return QStringList()
        << QStringLiteral("-o") << QStringLiteral("ConnectTimeout=8")
        << QStringLiteral("-o") << QStringLiteral("ServerAliveInterval=10")
        << QStringLiteral("-o") << QStringLiteral("ServerAliveCountMax=3")
        << QStringLiteral("-o") << QStringLiteral("StrictHostKeyChecking=no")
        << QStringLiteral("-o") << QStringLiteral("UserKnownHostsFile=/dev/null")
        << QStringLiteral("-o") << QStringLiteral("BatchMode=yes");
}

// Probe script: emit one `SECTION|...|...|` line per section/mission
// folder under /R_DATA at depth 3 (date/building/section). Status
// derives from sentinel files; counts come from `find` and `du`.
QString buildProbeRemoteCommand(const QString& data_root) {
    const QString quoted_root = shellSingleQuote(data_root);
    return QStringLiteral(
               "set -e; cd %1 2>/dev/null || { echo NO_R_DATA; exit 0; }; "
               "find . -mindepth 3 -maxdepth 3 -type d "
               "\\( -name 'Section_*' -o -name 'Mission_*' \\) "
               "| sort | while read -r p; do "
               "  date=$(echo \"$p\" | cut -d/ -f2); "
               "  building=$(echo \"$p\" | cut -d/ -f3); "
               "  section=$(basename \"$p\"); "
               "  if [ -f \"$p/manifest.json\" ]; then "
               "    state=done; "
               "    completed=$(python3 -c \"import json,sys;d=json.load(open(sys.argv[1]));print(len(d.get('files',[])))\" \"$p/manifest.json\" 2>/dev/null || echo 0); "
               "  elif [ -f \"$p/upload_state.json\" ]; then "
               "    state=partial; "
               "    completed=$(python3 -c \"import json,sys;d=json.load(open(sys.argv[1]));print(len(d.get('completed',[])))\" \"$p/upload_state.json\" 2>/dev/null || echo 0); "
               "  else "
               "    state=none; completed=0; "
               "  fi; "
               "  total=$(find \"$p\" -type f "
               "    ! -name upload_state.json ! -name pause.flag ! -name manifest.json "
               "    | wc -l); "
               "  size=$(du -sb \"$p\" 2>/dev/null | awk '{print $1}'); "
               "  echo \"SECTION|$date|$building|$section|$state|$completed|$total|$size\"; "
               "done")
        .arg(quoted_root);
}

UploadStatus parseStatus(const QString& token) {
    if (token == QLatin1String("done")) return UploadStatus::Done;
    if (token == QLatin1String("partial")) return UploadStatus::Partial;
    return UploadStatus::None;
}

}  // namespace

// ---------------------------------------------------------------------------
// UploadStateProbe
// ---------------------------------------------------------------------------

UploadStateProbe::UploadStateProbe(QObject* parent) : QObject(parent) {}

UploadStateProbe::~UploadStateProbe() {
    if (proc_ && proc_->state() != QProcess::NotRunning) {
        proc_->disconnect(this);
        proc_->kill();
        proc_->waitForFinished(500);
    }
}

void UploadStateProbe::setRemote(const QString& host, const QString& ssh_user) {
    remote_host_ = host.trimmed();
    ssh_user_ = ssh_user.trimmed();
}

void UploadStateProbe::setDataRoot(const QString& data_root) {
    if (!data_root.trimmed().isEmpty()) {
        data_root_ = data_root.trimmed();
    }
}

bool UploadStateProbe::isRunning() const {
    return proc_ && proc_->state() != QProcess::NotRunning;
}

void UploadStateProbe::start() {
    if (isRunning()) {
        return;
    }
    if (remote_host_.isEmpty() || ssh_user_.isEmpty()) {
        emit targetsReady(false, {},
                          QStringLiteral("No robot host/SSH user configured."));
        return;
    }

    stdout_buf_.clear();
    stderr_buf_.clear();
    last_error_.clear();

    proc_ = new QProcess(this);
    connect(proc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &UploadStateProbe::onProcessFinished);
    connect(proc_, &QProcess::errorOccurred,
            this, &UploadStateProbe::onProcessError);

    QStringList args = sshOptions();
    args << QStringLiteral("%1@%2").arg(ssh_user_, remote_host_);
    args << QStringLiteral("bash -lc %1")
                .arg(shellSingleQuote(buildProbeRemoteCommand(data_root_)));

    proc_->setProgram(QStringLiteral("ssh"));
    proc_->setArguments(args);
    proc_->start();
}

void UploadStateProbe::onProcessError(QProcess::ProcessError error) {
    Q_UNUSED(error);
    if (!proc_) return;
    last_error_ = proc_->errorString();
}

void UploadStateProbe::onProcessFinished(int exit_code,
                                         QProcess::ExitStatus status) {
    if (!proc_) {
        return;
    }
    stdout_buf_ = proc_->readAllStandardOutput();
    stderr_buf_ = proc_->readAllStandardError();

    if (status == QProcess::CrashExit || exit_code != 0) {
        QString err = last_error_;
        if (err.isEmpty()) {
            err = QString::fromUtf8(stderr_buf_).trimmed();
        }
        if (err.isEmpty()) {
            err = QStringLiteral("ssh probe exited with code %1").arg(exit_code);
        }
        emit targetsReady(false, {}, err);
        proc_->deleteLater();
        proc_ = nullptr;
        return;
    }

    QList<UploadTarget> out;
    const QString stdout_text = QString::fromUtf8(stdout_buf_);
    const QStringList lines = stdout_text.split(QLatin1Char('\n'),
                                                Qt::SkipEmptyParts);
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line == QLatin1String("NO_R_DATA")) {
            // /R_DATA missing on robot — empty list, but not a hard
            // error. Operators see "no scans recorded" in the dialog.
            break;
        }
        if (!line.startsWith(QLatin1String("SECTION|"))) {
            continue;
        }
        const QStringList fields = line.split(QLatin1Char('|'));
        if (fields.size() < 8) {
            continue;
        }
        UploadTarget t;
        t.date_folder = fields.at(1);
        t.building_slug = fields.at(2);
        t.section_name = fields.at(3);
        t.status = parseStatus(fields.at(4));
        t.completed_files = fields.at(5).toInt();
        t.total_files = fields.at(6).toInt();
        t.total_bytes = fields.at(7).toLongLong();
        t.run_id = QStringLiteral("%1/%2/%3")
                       .arg(t.date_folder, t.building_slug, t.section_name);
        t.remote_path = QStringLiteral("%1/%2")
                            .arg(data_root_, t.run_id);
        if (!t.date_folder.isEmpty() && !t.building_slug.isEmpty() &&
            !t.section_name.isEmpty()) {
            out.append(t);
        }
    }

    emit targetsReady(true, out, QString());
    proc_->deleteLater();
    proc_ = nullptr;
}

// ---------------------------------------------------------------------------
// UploadRunner
// ---------------------------------------------------------------------------

UploadRunner::UploadRunner(QObject* parent) : QObject(parent) {}

UploadRunner::~UploadRunner() {
    cancelPendingRetry();
    if (proc_ && proc_->state() != QProcess::NotRunning) {
        proc_->disconnect(this);
        proc_->kill();
        proc_->waitForFinished(500);
    }
}

int UploadRunner::retryBackoffMs(int attempt) {
    // Exponential backoff: 5 s → 15 s → 45 s.  Locked numbers per
    // operator design review.  `attempt` is 1-based; values outside
    // [1, kMaxConnectionRetries] clamp to 5 s so a buggy caller can
    // never accidentally sleep for hours.
    switch (attempt) {
        case 1: return 5000;
        case 2: return 15000;
        case 3: return 45000;
        default: return 5000;
    }
}

void UploadRunner::setRemote(const QString& host, const QString& ssh_user) {
    remote_host_ = host.trimmed();
    ssh_user_ = ssh_user.trimmed();
}

void UploadRunner::setCloudAuth(const QString& api_base,
                                const QString& client_id,
                                const QString& device_token) {
    cloud_api_base_ = api_base.trimmed();
    while (cloud_api_base_.endsWith('/')) cloud_api_base_.chop(1);
    cloud_client_id_ = client_id.trimmed();
    cloud_device_token_ = device_token.trimmed();
}

void UploadRunner::setRobotId(const QString& robot_id) {
    robot_id_ = robot_id.trimmed();
}

void UploadRunner::setRemoteScriptPath(const QString& path) {
    remote_script_path_ = path.trimmed();
}

void UploadRunner::setQueue(const QList<UploadTarget>& targets) {
    if (busy_) {
        qWarning("[UploadRunner] setQueue while busy: ignored. Cancel first.");
        return;
    }
    queue_ = targets;
    queue_total_ = queue_.size();
    current_index_ = -1;
}

UploadTarget UploadRunner::activeTarget() const {
    if (current_index_ >= 0 && current_index_ < queue_.size()) {
        return queue_.at(current_index_);
    }
    return UploadTarget{};
}

void UploadRunner::resetQueueState() {
    busy_ = false;
    cancel_requested_ = false;
    paused_during_target_ = false;
    pause_reason_.clear();
    fatal_error_.clear();
    stdout_carry_.clear();
    retry_attempts_done_ = 0;
    cancelPendingRetry();
}

void UploadRunner::cancelPendingRetry() {
    retry_pending_ = false;
    if (retry_timer_) {
        retry_timer_->stop();
        retry_timer_->deleteLater();
        retry_timer_ = nullptr;
    }
}

void UploadRunner::start() {
    if (busy_) {
        return;
    }
    cancelPendingRetry();
    if (queue_.isEmpty()) {
        emit queueFinished(false);
        return;
    }
    if (remote_host_.isEmpty() || ssh_user_.isEmpty()) {
        emit targetFailed(activeTarget(),
                          QStringLiteral("No robot host/SSH user configured."));
        emit queueFinished(false);
        return;
    }
    if (cloud_client_id_.isEmpty() || cloud_device_token_.isEmpty()) {
        emit targetFailed(activeTarget(),
                          QStringLiteral("Cloud credentials missing for this robot. "
                                         "Update robots.json with cloud_client_id "
                                         "+ cloud_device_token."));
        emit queueFinished(false);
        return;
    }
    if (cloud_api_base_.isEmpty()) {
        emit targetFailed(activeTarget(),
                          QStringLiteral("cloud_api_base not configured."));
        emit queueFinished(false);
        return;
    }

    busy_ = true;
    cancel_requested_ = false;
    if (current_index_ < 0) {
        current_index_ = 0;
    }
    retry_attempts_done_ = 0;
    launchNext();
}

void UploadRunner::launchNext() {
    if (cancel_requested_) {
        resetQueueState();
        emit queueFinished(true);
        return;
    }
    if (current_index_ >= queue_.size()) {
        resetQueueState();
        emit queueFinished(false);
        return;
    }

    // Fresh target — reset retry counter.  Retries reuse
    // `launchActiveTarget()` directly without bumping current_index_.
    retry_attempts_done_ = 0;
    launchActiveTarget();
}

void UploadRunner::launchActiveTarget() {
    if (current_index_ < 0 || current_index_ >= queue_.size()) {
        return;
    }
    paused_during_target_ = false;
    pause_reason_.clear();
    fatal_error_.clear();
    stdout_carry_.clear();

    const UploadTarget& target = queue_.at(current_index_);
    emit targetStarted(current_index_, queue_total_, target);

    proc_ = new QProcess(this);
    proc_->setProcessChannelMode(QProcess::SeparateChannels);
    connect(proc_, &QProcess::readyReadStandardOutput,
            this, &UploadRunner::onStdoutReady);
    connect(proc_, &QProcess::readyReadStandardError,
            this, &UploadRunner::onStderrReady);
    connect(proc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &UploadRunner::onProcessFinished);
    connect(proc_, &QProcess::errorOccurred,
            this, &UploadRunner::onProcessError);

    QStringList args = sshBaseArgs();
    args << QStringLiteral("%1@%2").arg(ssh_user_, remote_host_);
    args << QStringLiteral("bash -lc %1")
                .arg(shellSingleQuote(buildRemoteCommand(target)));

    proc_->setProgram(QStringLiteral("ssh"));
    proc_->setArguments(args);
    proc_->start();
}

QStringList UploadRunner::sshBaseArgs() const {
    return sshOptions();
}

QString UploadRunner::buildRemoteCommand(const UploadTarget& target) const {
    const QString script = remote_script_path_.isEmpty()
                               ? defaultRemoteScriptPath(ssh_user_)
                               : remote_script_path_;

    // Compose the env(1) prefix manually so we control quoting; QProcess'
    // default arg-list path won't help us because everything past the
    // ssh user@host arg is a single shell command on the remote side.
    return QStringLiteral(
               "env "
               "BDR_CLOUD_API_BASE=%1 "
               "BDR_CLOUD_CLIENT_ID=%2 "
               "BDR_CLOUD_DEVICE_TOKEN=%3 "
               "BDR_UPLOAD_WORKERS=12 "
               "python3 -u %4 %5 %6 %7")
        .arg(shellSingleQuote(cloud_api_base_),
             shellSingleQuote(cloud_client_id_),
             shellSingleQuote(cloud_device_token_),
             shellSingleQuote(script),
             shellSingleQuote(target.remote_path),
             shellSingleQuote(robot_id_),
             shellSingleQuote(target.run_id));
}

void UploadRunner::requestPause() {
    if (!busy_ || !proc_ || current_index_ < 0 ||
        current_index_ >= queue_.size()) {
        return;
    }
    const UploadTarget& target = queue_.at(current_index_);
    paused_during_target_ = true;
    if (pause_reason_.isEmpty()) {
        pause_reason_ = QStringLiteral("Operator paused");
    }

    // SSH a tiny `touch <pause.flag>` in a separate process; the active
    // upload script polls for it and exits at the next file boundary.
    QStringList args = sshOptions();
    args << QStringLiteral("%1@%2").arg(ssh_user_, remote_host_);
    args << QStringLiteral("touch %1")
                .arg(shellSingleQuote(target.remote_path +
                                      QStringLiteral("/pause.flag")));

    auto* tap = new QProcess(this);
    connect(tap,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            tap, &QProcess::deleteLater);
    tap->setProgram(QStringLiteral("ssh"));
    tap->setArguments(args);
    tap->start();
}

void UploadRunner::requestCancel() {
    cancel_requested_ = true;
    cancelPendingRetry();
    if (proc_ && proc_->state() != QProcess::NotRunning) {
        // SIGTERM first, then forced kill if it lingers. The script's
        // upload_state.json is written after each file, so a hard kill
        // mid-upload still leaves a resumable state on disk.
        proc_->terminate();
        if (!proc_->waitForFinished(2000)) {
            proc_->kill();
            proc_->waitForFinished(500);
        }
    } else {
        // Idle cancel (or cancel-while-retry-armed) — just clear the
        // queue tail and emit done.
        const UploadTarget current = (current_index_ >= 0 &&
                                      current_index_ < queue_.size())
                                         ? queue_.at(current_index_)
                                         : UploadTarget{};
        queue_.clear();
        current_index_ = -1;
        resetQueueState();
        if (!current.run_id.isEmpty()) {
            emit targetPaused(current,
                              QStringLiteral("Cancelled by operator"));
        }
        emit queueFinished(true);
    }
}

void UploadRunner::onStdoutReady() {
    if (!proc_) return;
    stdout_carry_ += QString::fromUtf8(proc_->readAllStandardOutput());
    while (true) {
        const int nl = stdout_carry_.indexOf(QLatin1Char('\n'));
        if (nl < 0) break;
        const QString line = stdout_carry_.left(nl).trimmed();
        stdout_carry_.remove(0, nl + 1);
        if (!line.isEmpty()) {
            processStdoutLine(line);
        }
    }
}

void UploadRunner::onStderrReady() {
    if (!proc_) return;
    const QString err = QString::fromUtf8(proc_->readAllStandardError())
                            .trimmed();
    if (!err.isEmpty()) {
        // ssh + python3 stderr is mostly noise (Pseudo-terminal,
        // BatchMode hints) but useful in the log strip when a real
        // error fires.
        emit logLine(QStringLiteral("[stderr] ") + err);
    }
}

void UploadRunner::processStdoutLine(const QString& line) {
    if (current_index_ < 0 || current_index_ >= queue_.size()) {
        return;
    }
    const UploadTarget& target = queue_.at(current_index_);

    static const QChar kCheck = QChar(0x2713);  // matches uploader.py "✓"
    if (line.startsWith(kCheck)) {
        // "✓ Uploaded: <relpath>"
        const int colon = line.indexOf(QLatin1Char(':'));
        const QString relpath =
            (colon >= 0) ? line.mid(colon + 1).trimmed() : line.mid(1).trimmed();
        emit fileUploaded(target, relpath);
        return;
    }
    if (line.startsWith(QLatin1String("Skipping already uploaded:"))) {
        const QString relpath = line.section(QLatin1Char(':'), 1).trimmed();
        emit fileSkipped(target, relpath);
        return;
    }
    if (line.startsWith(QLatin1String("Connection error:"))) {
        pause_reason_ = line;
        paused_during_target_ = true;
        emit logLine(line);
        return;
    }
    if (line.startsWith(QLatin1String("Auto-pausing"))) {
        if (pause_reason_.isEmpty()) {
            pause_reason_ = line;
        }
        paused_during_target_ = true;
        emit logLine(line);
        return;
    }
    if (line.startsWith(QLatin1String("Manual pause detected"))) {
        paused_during_target_ = true;
        if (pause_reason_.isEmpty()) {
            pause_reason_ = QStringLiteral("Manual pause");
        }
        emit logLine(line);
        return;
    }
    if (line.startsWith(QLatin1String("Already uploaded:"))) {
        // No-op short-circuit on a `Done` re-run — surface as log line.
        emit logLine(line);
        return;
    }
    if (line.startsWith(QLatin1String("Unexpected error:"))) {
        fatal_error_ = line;
        emit logLine(line);
        return;
    }
    // "Resuming upload.", "Uploading N files...", "Generating manifest...",
    // "Upload complete: ...", "State cleaned up."
    emit logLine(line);
}

void UploadRunner::onProcessError(QProcess::ProcessError error) {
    Q_UNUSED(error);
    if (!proc_) return;
    fatal_error_ = proc_->errorString();
}

void UploadRunner::onProcessFinished(int exit_code,
                                     QProcess::ExitStatus status) {
    if (current_index_ < 0 || current_index_ >= queue_.size()) {
        if (proc_) {
            proc_->deleteLater();
            proc_ = nullptr;
        }
        resetQueueState();
        emit queueFinished(cancel_requested_);
        return;
    }

    const UploadTarget current = queue_.at(current_index_);
    if (proc_) {
        // Drain any tail of stdout that didn't end in newline before
        // reporting per-target outcome.
        if (!stdout_carry_.isEmpty()) {
            const QString tail = stdout_carry_.trimmed();
            stdout_carry_.clear();
            if (!tail.isEmpty()) {
                processStdoutLine(tail);
            }
        }
        proc_->deleteLater();
        proc_ = nullptr;
    }

    if (cancel_requested_) {
        // Operator hit Cancel — emit pause for the in-flight target so
        // the row reflects "stopped (resumable)" rather than failed.
        emit targetPaused(current,
                          QStringLiteral("Cancelled by operator"));
        queue_.clear();
        current_index_ = -1;
        resetQueueState();
        emit queueFinished(true);
        return;
    }

    if (status == QProcess::CrashExit) {
        emit targetFailed(current,
                          QStringLiteral("ssh / uploader crashed (signalled exit)"));
        resetQueueState();
        queue_.clear();
        current_index_ = -1;
        emit queueFinished(false);
        return;
    }

    if (paused_during_target_) {
        // Decide between auto-retry (transient cloud connection error)
        // and operator-visible pause.  Manual pauses never auto-retry
        // — that would defeat the operator's intent.
        const bool is_transient_conn_err =
            pause_reason_.contains(QStringLiteral("Connection error"),
                                   Qt::CaseInsensitive);
        const bool can_retry =
            is_transient_conn_err &&
            retry_enabled_ &&
            retry_attempts_done_ < kMaxConnectionRetries;

        if (can_retry) {
            const int next_attempt = retry_attempts_done_ + 1;
            const int wait_ms = retryBackoffMs(next_attempt);
            retry_pending_ = true;
            cancelPendingRetry();  // also resets retry_pending_ — restore
            retry_pending_ = true;
            retry_timer_ = new QTimer(this);
            retry_timer_->setSingleShot(true);
            retry_timer_->setInterval(wait_ms);
            const UploadTarget snapshot = current;  // copy in case queue mutates
            connect(retry_timer_, &QTimer::timeout, this,
                    [this]() {
                        retry_pending_ = false;
                        if (retry_timer_) {
                            retry_timer_->deleteLater();
                            retry_timer_ = nullptr;
                        }
                        if (cancel_requested_ ||
                            current_index_ < 0 ||
                            current_index_ >= queue_.size()) {
                            return;
                        }
                        retry_attempts_done_ += 1;
                        launchActiveTarget();
                    });
            emit targetRetryScheduled(snapshot, next_attempt, wait_ms,
                                      pause_reason_);
            retry_timer_->start();
            // Stay busy_ — we are still owning the active target.
            return;
        }

        // Either non-transient pause (operator) OR retry budget
        // exhausted.  In the exhausted-budget case we promote to a
        // hard failure so the dialog modal fires.
        if (is_transient_conn_err) {
            const QString final_msg =
                QStringLiteral("Connection error persisted after %1 retries: %2")
                    .arg(kMaxConnectionRetries)
                    .arg(pause_reason_);
            emit targetFailed(current, final_msg);
            queue_.clear();
            current_index_ = -1;
            resetQueueState();
            emit queueFinished(false);
            return;
        }

        emit targetPaused(current,
                          pause_reason_.isEmpty()
                              ? QStringLiteral("Paused")
                              : pause_reason_);
        // Keep the queue intact so the operator can resume by pressing
        // Upload again — the target stays at `current_index_`.
        busy_ = false;
        emit queueFinished(false);
        return;
    }

    if (!fatal_error_.isEmpty() || exit_code != 0) {
        emit targetFailed(current,
                          fatal_error_.isEmpty()
                              ? QStringLiteral("uploader exited with code %1")
                                    .arg(exit_code)
                              : fatal_error_);
        // Hard error on a target halts the queue so the operator sees
        // the message before mass-failing every other section.
        resetQueueState();
        queue_.clear();
        current_index_ = -1;
        emit queueFinished(false);
        return;
    }

    emit targetCompleted(current);
    current_index_ += 1;
    launchNext();
}

}  // namespace f2c_cpp
