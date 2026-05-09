/**
 * @file update_runner_window.cpp
 * @brief UpdateRunnerWindow implementation. See header for design notes.
 */

#include "runner/update_runner_window.hpp"

#include <QApplication>
#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStringList>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "update/update_state.hpp"

#include "ui_theme_constants.hpp"
#include "update/update_downloader.hpp"
#include "update/update_log.hpp"

namespace f2c_cpp {

namespace {

// 2 second pause on the "Install Complete" screen before execv'ing the
// OCU back. Long enough for the operator to see "Done — restarting…",
// short enough not to feel sluggish.
constexpr int kRestartPauseSeconds = 2;

// Absolute path of the privileged installer wrapper. Phase 8 ships this
// in the .deb at /usr/bin/bdr-apply-update; the sudoers drop-in pinning
// the same absolute path is at /etc/sudoers.d/bdr-coverage-planner.
constexpr const char* kApplyUpdateWrapperPath = "/usr/bin/bdr-apply-update";

// Hard ceiling on a single dpkg invocation. Generous — typical wall is
// 8-15s on a 22 MB package, but a postinst that hangs (e.g. interactive
// prompt smuggled past --force-confdef) would otherwise wedge the runner
// indefinitely. 5 minutes is far longer than any legitimate install.
constexpr int kDpkgTimeoutMs = 5 * 60 * 1000;

// Max bytes of the tail of update.log we slurp into the details expander.
// 64 KB is enough for ~600 lines of typical OTA output.
constexpr qint64 kLogTailMaxBytes = 64LL * 1024LL;

// EWMA smoothing factor for the throughput readout.
// 0.3 gives a noticeably steady "X.X MB/s" without lag.
constexpr double kBpsSmoothingAlpha = 0.3;

QString formatBytes(qint64 bytes) {
    if (bytes <= 0) return QStringLiteral("\u2014");
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    return QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
}

}  // namespace

UpdateRunnerWindow::UpdateRunnerWindow(const RunnerArgs& args,
                                       QWidget* parent)
    : QWidget(parent),
      args_(args) {
    setObjectName(QStringLiteral("UpdateRunnerWindow"));
    setWindowTitle(QStringLiteral("BDR Coverage Planner — Updating"));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setMinimumSize(640, 360);

    buildUi();
    applyStyle();

    // Show window centered on the primary screen (locked spec concern #5).
    if (QScreen* s = QGuiApplication::primaryScreen()) {
        const QRect avail = s->availableGeometry();
        adjustSize();
        move(avail.center().x() - width() / 2,
             avail.center().y() - height() / 2);
    }

    // FileSystemWatcher on the OCU's update.log so the details expander
    // can refresh its tail as the runner appends.
    log_watcher_ = new QFileSystemWatcher(this);
    const QString log_path = deriveOcuLogPath();
    if (!log_path.isEmpty() && QFile::exists(log_path)) {
        log_watcher_->addPath(log_path);
    }
    connect(log_watcher_, &QFileSystemWatcher::fileChanged,
            this, &UpdateRunnerWindow::onLogFileChanged);

    if (!args_.rollback) {
        downloader_ = new update::UpdateDownloader(this);
        connect(downloader_, &update::UpdateDownloader::progressChanged,
                this, &UpdateRunnerWindow::onDownloadProgress);
        connect(downloader_, &update::UpdateDownloader::downloadComplete,
                this, &UpdateRunnerWindow::onDownloadComplete);
        connect(downloader_, &update::UpdateDownloader::downloadFailed,
                this, &UpdateRunnerWindow::onDownloadFailed);
        connect(downloader_, &update::UpdateDownloader::retryScheduled,
                this, &UpdateRunnerWindow::onRetryScheduled);

        // Kick the download once Qt has the event loop spinning — gives
        // the window a moment to draw before we start hammering network.
        QTimer::singleShot(0, this, &UpdateRunnerWindow::startDownload);
    } else {
        // Phase 9 rollback: skip download/SHA, jump straight to the
        // install state with the previous-generation .deb. The window
        // copy is relabeled to "Rolling back…" inside enterStateInstalling
        // via the args_.rollback gate.
        update::log::info(
            "runner",
            QStringLiteral("rollback mode: prev_deb=%1")
                .arg(args_.rollbackPrevDeb));
        QTimer::singleShot(0, this, [this]() {
            enterStateInstalling(args_.rollbackPrevDeb);
        });
    }
}

UpdateRunnerWindow::~UpdateRunnerWindow() {
    // Defensive: never leave a dpkg child running past the runner. In
    // practice the runner only exits via execv (which preserves child
    // processes) or process kill (which the kernel propagates); but a
    // future Qt::WindowClose path could land here.
    if (dpkg_proc_ && dpkg_proc_->state() != QProcess::NotRunning) {
        update::log::warn(
            "runner",
            QStringLiteral("dtor: killing in-flight dpkg pid=%1")
                .arg(dpkg_proc_->processId()));
        dpkg_proc_->kill();
        dpkg_proc_->waitForFinished(2000);
    }
}

void UpdateRunnerWindow::buildUi() {
    auto* container = new QWidget(this);
    container->setObjectName(QStringLiteral("UpdateRunnerWindow_Container"));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(container);

    auto* root = new QVBoxLayout(container);
    root->setContentsMargins(28, 24, 28, 24);
    root->setSpacing(18);

    // Header: icon tile + title col. No close button — locked Q2=A.
    auto* header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(14);

    lbl_icon_tile_ = new QLabel(QStringLiteral("\u21A7"), container);
    lbl_icon_tile_->setObjectName(
        QStringLiteral("UpdateRunnerWindow_IconTile"));
    lbl_icon_tile_->setAlignment(Qt::AlignCenter);
    lbl_icon_tile_->setFixedSize(48, 48);
    header->addWidget(lbl_icon_tile_, 0, Qt::AlignTop);

    auto* title_col = new QVBoxLayout();
    title_col->setContentsMargins(0, 0, 0, 0);
    title_col->setSpacing(2);

    lbl_title_ = new QLabel(
        args_.rollback
            ? QStringLiteral("Restoring previous version")
            : QStringLiteral("Updating Roofus OCU"),
        container);
    lbl_title_->setObjectName(QStringLiteral("UpdateRunnerWindow_Title"));
    title_col->addWidget(lbl_title_);

    QString subtitle;
    if (args_.rollback) {
        subtitle = QStringLiteral(
            "The new version did not start cleanly. Restoring previous build.");
    } else {
        QString version = args_.tag.isEmpty()
            ? args_.commitSha.left(7)
            : args_.tag;
        if (version.startsWith(QStringLiteral("v-"))) {
            version = version.mid(2);
        }
        subtitle = QStringLiteral("Version %1").arg(version);
    }
    lbl_subtitle_ = new QLabel(subtitle, container);
    lbl_subtitle_->setObjectName(
        QStringLiteral("UpdateRunnerWindow_Subtitle"));
    title_col->addWidget(lbl_subtitle_);

    header->addLayout(title_col, 1);
    root->addLayout(header);

    // Status block. enterStateInstalling overwrites this on the first
    // event-loop tick in rollback mode (where there's no download phase).
    lbl_status_ = new QLabel(
        args_.rollback
            ? QStringLiteral("Preparing rollback…")
            : QStringLiteral("Downloading update package…"),
        container);
    lbl_status_->setObjectName(QStringLiteral("UpdateRunnerWindow_Status"));
    root->addWidget(lbl_status_);

    progress_ = new QProgressBar(container);
    progress_->setObjectName(QStringLiteral("UpdateRunnerWindow_Progress"));
    progress_->setRange(0, 0);  // indeterminate until first progress tick
    progress_->setTextVisible(false);
    progress_->setFixedHeight(8);
    root->addWidget(progress_);

    lbl_size_row_ = new QLabel(container);
    lbl_size_row_->setObjectName(
        QStringLiteral("UpdateRunnerWindow_SizeRow"));
    lbl_size_row_->setText(formatSizeRow(0, args_.sizeBytes));
    root->addWidget(lbl_size_row_);

    // Retry strip — initially hidden.
    lbl_retry_ = new QLabel(container);
    lbl_retry_->setObjectName(QStringLiteral("UpdateRunnerWindow_Retry"));
    lbl_retry_->setWordWrap(true);
    lbl_retry_->hide();
    root->addWidget(lbl_retry_);

    // Failure strip — initially hidden. Locked Q4=A: red callout +
    // Try Again + Cancel.
    lbl_warning_ = new QLabel(container);
    lbl_warning_->setObjectName(
        QStringLiteral("UpdateRunnerWindow_Warning"));
    lbl_warning_->setWordWrap(true);
    lbl_warning_->hide();
    root->addWidget(lbl_warning_);

    auto* fail_buttons = new QHBoxLayout();
    fail_buttons->setContentsMargins(0, 0, 0, 0);
    fail_buttons->setSpacing(10);
    fail_buttons->addStretch();

    btn_cancel_ = new QPushButton(QStringLiteral("Cancel"), container);
    btn_cancel_->setObjectName(QStringLiteral("UpdateRunnerWindow_Cancel"));
    btn_cancel_->setCursor(Qt::PointingHandCursor);
    btn_cancel_->setFixedHeight(36);
    btn_cancel_->setMinimumWidth(120);
    btn_cancel_->hide();
    connect(btn_cancel_, &QPushButton::clicked,
            this, &UpdateRunnerWindow::onCancelClicked);
    fail_buttons->addWidget(btn_cancel_);

    btn_try_again_ = new QPushButton(QStringLiteral("Try Again"), container);
    btn_try_again_->setObjectName(
        QStringLiteral("UpdateRunnerWindow_TryAgain"));
    btn_try_again_->setCursor(Qt::PointingHandCursor);
    btn_try_again_->setFixedHeight(36);
    btn_try_again_->setMinimumWidth(120);
    btn_try_again_->hide();
    connect(btn_try_again_, &QPushButton::clicked,
            this, &UpdateRunnerWindow::onTryAgainClicked);
    fail_buttons->addWidget(btn_try_again_);

    root->addLayout(fail_buttons);

    // Restart strip — visible only in InstallComplete.
    lbl_restart_ = new QLabel(container);
    lbl_restart_->setObjectName(
        QStringLiteral("UpdateRunnerWindow_Restart"));
    lbl_restart_->setWordWrap(true);
    lbl_restart_->setAlignment(Qt::AlignCenter);
    lbl_restart_->hide();
    root->addWidget(lbl_restart_);

    // Show Details expander (locked Q3=C: hidden by default).
    btn_show_details_ = new QToolButton(container);
    btn_show_details_->setObjectName(
        QStringLiteral("UpdateRunnerWindow_ShowDetails"));
    btn_show_details_->setText(QStringLiteral("\u25B6  Show details"));
    btn_show_details_->setCheckable(true);
    btn_show_details_->setChecked(false);
    btn_show_details_->setCursor(Qt::PointingHandCursor);
    btn_show_details_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    connect(btn_show_details_, &QToolButton::toggled,
            this, &UpdateRunnerWindow::onShowDetailsToggled);
    root->addWidget(btn_show_details_, 0, Qt::AlignLeft);

    log_tail_ = new QPlainTextEdit(container);
    log_tail_->setObjectName(QStringLiteral("UpdateRunnerWindow_LogTail"));
    log_tail_->setReadOnly(true);
    log_tail_->setMaximumBlockCount(500);
    log_tail_->setFrameShape(QFrame::NoFrame);
    log_tail_->setLineWrapMode(QPlainTextEdit::NoWrap);
    log_tail_->setMinimumHeight(140);
    log_tail_->hide();
    root->addWidget(log_tail_);

    // Timers.
    retry_countdown_ = new QTimer(this);
    retry_countdown_->setInterval(1000);
    connect(retry_countdown_, &QTimer::timeout,
            this, &UpdateRunnerWindow::onRetryCountdownTick);

    restart_countdown_ = new QTimer(this);
    restart_countdown_->setInterval(1000);
    connect(restart_countdown_, &QTimer::timeout,
            this, &UpdateRunnerWindow::onRestartCountdownTick);
}

void UpdateRunnerWindow::applyStyle() {
    const UiThemeTokens t = uiThemeTokens(args_.darkMode);

    // Mirror UpdateModal's tokens (locked Q5=A). Container card: bg, 1px
    // border, 12px radius. Icon tile: accent. Progress bar accent chunk.
    setStyleSheet(QStringLiteral(
        "QWidget#UpdateRunnerWindow_Container {"
        "  background-color: %1;"
        "  border: 1px solid %2;"
        "  border-radius: 12px;"
        "}"
        "QLabel#UpdateRunnerWindow_IconTile {"
        "  background-color: %3;"
        "  color: white;"
        "  border-radius: 10px;"
        "  font-size: 26px;"
        "  font-weight: 700;"
        "}"
        "QLabel#UpdateRunnerWindow_Title {"
        "  color: %4;"
        "  font-size: 20px;"
        "  font-weight: 700;"
        "  background: transparent;"
        "  border: none;"
        "}"
        "QLabel#UpdateRunnerWindow_Subtitle {"
        "  color: %5;"
        "  font-size: 13px;"
        "  background: transparent;"
        "  border: none;"
        "}"
        "QLabel#UpdateRunnerWindow_Status {"
        "  color: %4;"
        "  font-size: 14px;"
        "  font-weight: 600;"
        "  background: transparent;"
        "  border: none;"
        "}"
        "QLabel#UpdateRunnerWindow_SizeRow {"
        "  color: %5;"
        "  font-size: 12px;"
        "  background: transparent;"
        "  border: none;"
        "}"
        "QLabel#UpdateRunnerWindow_Retry {"
        "  color: %7;"
        "  background-color: rgba(245, 158, 11, 0.12);"
        "  border: 1px solid %7;"
        "  border-radius: 8px;"
        "  padding: 10px 12px;"
        "  font-size: 12px;"
        "  font-weight: 600;"
        "}"
        "QLabel#UpdateRunnerWindow_Warning {"
        "  color: %8;"
        "  background-color: rgba(239, 68, 68, 0.12);"
        "  border: 1px solid %8;"
        "  border-radius: 8px;"
        "  padding: 10px 12px;"
        "  font-size: 13px;"
        "  font-weight: 600;"
        "}"
        "QLabel#UpdateRunnerWindow_Restart {"
        "  color: %3;"
        "  font-size: 14px;"
        "  font-weight: 700;"
        "  padding: 6px 0;"
        "  background: transparent;"
        "  border: none;"
        "}"
        "QProgressBar#UpdateRunnerWindow_Progress {"
        "  background-color: %6;"
        "  border: 1px solid %2;"
        "  border-radius: 4px;"
        "}"
        "QProgressBar#UpdateRunnerWindow_Progress::chunk {"
        "  background-color: %3;"
        "  border-radius: 3px;"
        "}"
        "QToolButton#UpdateRunnerWindow_ShowDetails {"
        "  color: %5;"
        "  background: transparent;"
        "  border: none;"
        "  font-size: 12px;"
        "  font-weight: 600;"
        "  padding: 4px 0;"
        "}"
        "QToolButton#UpdateRunnerWindow_ShowDetails:hover {"
        "  color: %4;"
        "}"
        "QPlainTextEdit#UpdateRunnerWindow_LogTail {"
        "  background-color: %6;"
        "  color: %5;"
        "  font-family: monospace;"
        "  font-size: 11px;"
        "  border: 1px solid %2;"
        "  border-radius: 8px;"
        "  padding: 8px;"
        "}"
        "QPushButton#UpdateRunnerWindow_TryAgain {"
        "  color: white;"
        "  background-color: %3;"
        "  border: none;"
        "  border-radius: 8px;"
        "  font-size: 13px;"
        "  font-weight: 700;"
        "  padding: 0 16px;"
        "}"
        "QPushButton#UpdateRunnerWindow_TryAgain:hover {"
        "  background-color: %9;"
        "}"
        "QPushButton#UpdateRunnerWindow_Cancel {"
        "  color: %5;"
        "  background-color: transparent;"
        "  border: 1px solid %2;"
        "  border-radius: 8px;"
        "  font-size: 13px;"
        "  font-weight: 600;"
        "  padding: 0 16px;"
        "}"
        "QPushButton#UpdateRunnerWindow_Cancel:hover {"
        "  color: %4;"
        "  background-color: %6;"
        "}"
    )
        .arg(t.card_bg)        // %1
        .arg(t.border)         // %2
        .arg(t.accent)         // %3
        .arg(t.text)           // %4
        .arg(t.muted)          // %5
        .arg(t.bg)             // %6
        .arg(t.warning)        // %7
        .arg(t.danger)         // %8
        .arg(t.accent_hover)); // %9
}

void UpdateRunnerWindow::startDownload() {
    update::log::info("runner",
                      QStringLiteral("starting download: tag=%1 sha=%2 size=%3")
                          .arg(args_.tag)
                          .arg(args_.commitSha)
                          .arg(args_.sizeBytes));

    update::VersionInfo info;
    info.tag = args_.tag;
    info.commitSha = args_.commitSha;
    info.assetName = args_.assetName;
    info.downloadUrl = args_.debUrl;
    info.sha256Url = args_.sha256Url;
    info.sizeBytes = args_.sizeBytes;

    last_progress_received_ = 0;
    last_progress_at_ms_ = QDateTime::currentMSecsSinceEpoch();
    smoothed_bps_ = 0.0;

    enterStateDownloading();
    downloader_->start(info);
}

void UpdateRunnerWindow::enterStateDownloading() {
    state_ = State::Downloading;
    lbl_status_->setText(QStringLiteral("Downloading update package…"));
    progress_->setRange(0, 0);
    progress_->setValue(0);
    lbl_size_row_->setText(formatSizeRow(0, args_.sizeBytes));
    lbl_retry_->hide();
    lbl_warning_->hide();
    lbl_restart_->hide();
    btn_try_again_->hide();
    btn_cancel_->hide();
    if (retry_countdown_->isActive()) retry_countdown_->stop();
    if (restart_countdown_->isActive()) restart_countdown_->stop();
}

void UpdateRunnerWindow::enterStateRetrying(int attempt,
                                            int totalAttempts,
                                            int seconds) {
    state_ = State::Retrying;
    retry_countdown_seconds_ = seconds;
    retry_countdown_attempt_ = attempt;
    retry_countdown_total_ = totalAttempts;
    lbl_status_->setText(QStringLiteral("Network hiccup — retrying."));
    progress_->setRange(0, 0);
    progress_->setValue(0);
    lbl_retry_->setText(
        QStringLiteral("Retrying (attempt %1 of %2) in %3 s…")
            .arg(attempt).arg(totalAttempts).arg(seconds));
    lbl_retry_->show();
    if (!retry_countdown_->isActive()) {
        retry_countdown_->start();
    }
}

void UpdateRunnerWindow::enterStateInstalling(const QString& debPath) {
    state_ = State::Installing;
    completed_deb_path_ = debPath;
    install_attempts_++;

    // Forward-install: stash the just-replaced .deb so Phase 9 rollback
    // knows what to fall back to. In rollback mode the "previous" we'd
    // resolve here is the just-failed-install .deb, which is NOT a valid
    // rollback target — so leave previous_deb_path_ empty.
    if (args_.rollback) {
        previous_deb_path_.clear();
    } else {
        previous_deb_path_ = resolvePreviousDebPath(debPath);
    }

    update::log::info(
        "runner",
        QStringLiteral("install: mode=%1 attempt=%2 deb=%3 prev=%4")
            .arg(args_.rollback ? QStringLiteral("rollback")
                                : QStringLiteral("forward"))
            .arg(install_attempts_)
            .arg(debPath)
            .arg(previous_deb_path_.isEmpty()
                     ? QStringLiteral("(none)")
                     : previous_deb_path_));

    // Mark dpkg_running BEFORE we invoke dpkg so that even a power loss
    // between marker write and dpkg start leaves Phase 9 a breadcrumb
    // (the recover path will run `dpkg --configure -a`).
    update::UpdateStateData marker;
    marker.stage = update::UpdateStage::DpkgRunning;
    marker.currentDebPath = debPath;
    marker.previousDebPath = previous_deb_path_;
    update::writeUpdateState(marker);

    // UI copy varies by mode. Operator-facing — keep it plain.
    lbl_status_->setText(
        args_.rollback
            ? QStringLiteral(
                  "Rolling back to previous version — please do not power off.")
            : QStringLiteral(
                  "Installing update package — please do not power off."));
    progress_->setRange(0, 0);
    progress_->setValue(0);
    lbl_size_row_->setText(QStringLiteral("Running dpkg…"));
    lbl_retry_->hide();
    lbl_warning_->hide();
    lbl_restart_->hide();
    btn_try_again_->hide();
    btn_cancel_->hide();
    if (retry_countdown_->isActive()) retry_countdown_->stop();
    if (restart_countdown_->isActive()) restart_countdown_->stop();

    runDpkg();
}

void UpdateRunnerWindow::runDpkg() {
    // Guard against double-spawn (e.g. Try Again clicked while a dpkg
    // process is still alive — shouldn't happen, but defensive).
    if (dpkg_proc_ && dpkg_proc_->state() != QProcess::NotRunning) {
        update::log::warn(
            "runner",
            QStringLiteral("runDpkg: prior dpkg still running, ignoring"));
        return;
    }
    if (dpkg_proc_) {
        dpkg_proc_->deleteLater();
        dpkg_proc_ = nullptr;
    }

    dpkg_proc_ = new QProcess(this);
    dpkg_proc_->setProcessChannelMode(QProcess::MergedChannels);
    connect(dpkg_proc_, &QProcess::readyReadStandardOutput,
            this, &UpdateRunnerWindow::onDpkgReadyReadStdout);
    connect(dpkg_proc_,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this](int code, QProcess::ExitStatus status) {
                onDpkgFinished(code, static_cast<int>(status));
            });

    // sudo -n: non-interactive. With our NOPASSWD sudoers drop-in this
    // is silent; without it, sudo exits immediately with a clear error
    // we can surface to the operator instead of hanging on a hidden
    // password prompt.
    const QStringList dpkg_args = {
        QStringLiteral("-n"),
        QString::fromLatin1(kApplyUpdateWrapperPath),
        QStringLiteral("install"),
        completed_deb_path_,
    };
    update::log::info(
        "runner",
        QStringLiteral("spawning: sudo %1").arg(dpkg_args.join(QLatin1Char(' '))));

    // Fire a timeout safety net. Real dpkg should finish in ~15 s; any
    // hang past kDpkgTimeoutMs means something is wedged (interactive
    // prompt, broken postinst). We kill the child and surface a clear
    // error rather than letting the runner sit there forever.
    QTimer::singleShot(kDpkgTimeoutMs, this, [this]() {
        if (dpkg_proc_ && dpkg_proc_->state() != QProcess::NotRunning) {
            update::log::error(
                "runner",
                QStringLiteral("dpkg timed out after %1 min — killing")
                    .arg(kDpkgTimeoutMs / 60000));
            dpkg_proc_->kill();
        }
    });

    dpkg_proc_->start(QStringLiteral("sudo"), dpkg_args);
    if (!dpkg_proc_->waitForStarted(2000)) {
        const QString err = QStringLiteral(
            "Failed to start sudo: %1").arg(dpkg_proc_->errorString());
        update::log::error("runner", err);
        enterStateFailed(err, FailureCause::Install);
        return;
    }
}

void UpdateRunnerWindow::onDpkgReadyReadStdout() {
    if (!dpkg_proc_) return;
    const QByteArray bytes = dpkg_proc_->readAllStandardOutput();
    if (bytes.isEmpty()) return;
    // Tee dpkg's chatter into update.log line-by-line. The wrapper also
    // tees into /var/log/bdr-apply-update.log; this duplication is
    // intentional so a postmortem on the cache log alone is sufficient.
    const QStringList lines =
        QString::fromUtf8(bytes).split(QChar(QLatin1Char('\n')),
                                       Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        update::log::info("dpkg", line.trimmed());
    }
    // Refresh the visible log tail if the operator has Show Details open.
    if (log_tail_ && log_tail_->isVisible()) {
        appendLogTail();
    }
}

void UpdateRunnerWindow::onDpkgFinished(int exit_code, int exit_status_int) {
    const auto exit_status = static_cast<QProcess::ExitStatus>(exit_status_int);
    update::log::info(
        "runner",
        QStringLiteral("dpkg finished: rc=%1 status=%2")
            .arg(exit_code).arg(static_cast<int>(exit_status)));

    if (exit_status != QProcess::NormalExit || exit_code != 0) {
        // Decode common wrapper exit codes for a clearer operator message.
        QString reason;
        switch (exit_code) {
            case 64:
                reason = QStringLiteral("Installer rejected its arguments "
                                        "(internal error).");
                break;
            case 65:
                reason = QStringLiteral(
                    "Installer rejected the package path (security check).");
                break;
            case 66:
                reason = QStringLiteral(
                    "dpkg failed to install the package. The previous "
                    "version remains intact.");
                break;
            case 67:
                reason = QStringLiteral(
                    "Installer was not run as root. Check that the OTA "
                    "sudoers entry is installed.");
                break;
            default:
                reason = QStringLiteral("Installer exited with code %1.")
                             .arg(exit_code);
                break;
        }

        if (args_.rollback) {
            // Phase 9 Q4=A: rollback dpkg also failed. Best-effort
            // `recover` (dpkg --configure -a) to leave the system in
            // a configurable state, then mark RolledBack and execv OCU.
            // The operator will see the rollback banner and can contact
            // support. There is no Try Again from this state — re-running
            // the same prev.deb against the same broken dpkg state is
            // unlikely to help, and we don't have a third version to fall
            // back to.
            update::log::error(
                "runner",
                QStringLiteral("rollback dpkg failed: %1 — running recover")
                    .arg(reason));

            QProcess recover;
            recover.setProcessChannelMode(QProcess::MergedChannels);
            recover.start(QStringLiteral("sudo"),
                          QStringList() << QStringLiteral("-n")
                                        << QString::fromLatin1(
                                               kApplyUpdateWrapperPath)
                                        << QStringLiteral("recover"));
            const bool recover_finished = recover.waitForFinished(120000);
            const int recover_rc = recover_finished
                                       ? recover.exitCode()
                                       : -1;
            update::log::info(
                "runner",
                QStringLiteral("recover finished: rc=%1 finished=%2")
                    .arg(recover_rc).arg(recover_finished));

            update::UpdateStateData marker;
            marker.stage = update::UpdateStage::RolledBack;
            marker.currentDebPath.clear();
            marker.previousDebPath = completed_deb_path_;
            update::writeUpdateState(marker);

            enterStateInstallComplete();
            return;
        }

        // Forward-install failure path: keep `dpkg_running` marker so the
        // OCU's recover path will run on next launch (operator clicks
        // Cancel) or on a Try Again here.
        enterStateFailed(reason, FailureCause::Install);
        return;
    }

    update::UpdateStateData marker;
    if (args_.rollback) {
        // Rollback dpkg succeeded. We're now back on the previous .deb.
        // Mark RolledBack — OCU's startup dispatch reads this and shows
        // the rollback advisory banner instead of the watchdog path.
        marker.stage = update::UpdateStage::RolledBack;
        marker.currentDebPath = completed_deb_path_;
        marker.previousDebPath.clear();
    } else {
        // Forward dpkg succeeded. Marker advances to InstalledPendingProbe
        // — the OCU rewrites this to ProbingHealth when it starts its
        // 60 s watchdog. The two-state split lets the watchdog distinguish
        // first-launch-after-install (we got here legitimately) from
        // ctor-crash-relaunch (the OCU crashed before clearing the marker).
        marker.stage = update::UpdateStage::InstalledPendingProbe;
        marker.currentDebPath = completed_deb_path_;
        marker.previousDebPath = previous_deb_path_;
    }
    update::writeUpdateState(marker);

    enterStateInstallComplete();
}

void UpdateRunnerWindow::enterStateInstallComplete() {
    state_ = State::InstallComplete;
    update::log::info(
        "runner",
        QStringLiteral("install complete: deb=%1").arg(completed_deb_path_));
    lbl_status_->setText(
        args_.rollback
            ? QStringLiteral("Rollback complete — restarting application…")
            : QStringLiteral("Installation complete — restarting application…"));
    progress_->setRange(0, 100);
    progress_->setValue(100);
    lbl_size_row_->setText(
        args_.rollback
            ? QStringLiteral("Previous version restored.")
            : formatSizeRow(args_.sizeBytes > 0 ? args_.sizeBytes : 0,
                            args_.sizeBytes));
    lbl_retry_->hide();
    lbl_warning_->hide();
    btn_try_again_->hide();
    btn_cancel_->hide();
    restart_countdown_seconds_ = kRestartPauseSeconds;
    lbl_restart_->setText(
        QStringLiteral("Restarting in %1 s…").arg(restart_countdown_seconds_));
    lbl_restart_->show();
    if (!restart_countdown_->isActive()) {
        restart_countdown_->start();
    }
}

void UpdateRunnerWindow::enterStateFailed(const QString& reason,
                                          FailureCause cause) {
    state_ = State::Failed;
    last_failure_cause_ = cause;
    const char* cause_label = (cause == FailureCause::Download) ? "download"
                              : (cause == FailureCause::Install) ? "install"
                              : "unknown";
    update::log::error(
        "runner",
        QStringLiteral("failure (cause=%1): %2")
            .arg(QString::fromLatin1(cause_label)).arg(reason));
    lbl_status_->setText(
        cause == FailureCause::Install
            ? QStringLiteral("Update install failed.")
            : QStringLiteral("Update download failed."));
    progress_->setRange(0, 100);
    progress_->setValue(0);
    lbl_retry_->hide();
    lbl_restart_->hide();
    lbl_warning_->setText(
        QStringLiteral("\u26A0  %1\n\nYou can try again or cancel and "
                       "return to the previous version of the application.")
            .arg(reason));
    lbl_warning_->show();
    btn_try_again_->show();
    btn_cancel_->show();
    if (retry_countdown_->isActive()) retry_countdown_->stop();
    if (restart_countdown_->isActive()) restart_countdown_->stop();
}

void UpdateRunnerWindow::onDownloadProgress(qint64 received, qint64 total) {
    if (state_ != State::Downloading) {
        // Could fire mid-retry — switch the UI back to downloading.
        enterStateDownloading();
    }
    if (total > 0) {
        progress_->setRange(0, 100);
        const int pct = static_cast<int>((received * 100) / total);
        progress_->setValue(pct);
    }

    // Throughput (EWMA smoothed) for the size row.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (last_progress_at_ms_ > 0 && now > last_progress_at_ms_) {
        const qint64 dt_ms = now - last_progress_at_ms_;
        const qint64 d_bytes = received - last_progress_received_;
        if (dt_ms > 0 && d_bytes >= 0) {
            const double inst_bps =
                static_cast<double>(d_bytes) * 1000.0 /
                static_cast<double>(dt_ms);
            smoothed_bps_ = (smoothed_bps_ <= 0.0)
                                ? inst_bps
                                : (kBpsSmoothingAlpha * inst_bps +
                                   (1.0 - kBpsSmoothingAlpha) * smoothed_bps_);
        }
    }
    last_progress_at_ms_ = now;
    last_progress_received_ = received;

    QString row = formatSizeRow(received, total > 0 ? total : args_.sizeBytes);
    if (smoothed_bps_ > 1024.0) {
        row += QStringLiteral("   \u2022   %1")
                   .arg(formatBytesPerSec(
                       static_cast<qint64>(smoothed_bps_)));
    }
    lbl_size_row_->setText(row);
}

void UpdateRunnerWindow::onDownloadComplete(const QString& debPath) {
    // Phase 8: download is the prelude. Pivot straight into the install
    // state — runner UI never lingers on a "download done" screen.
    enterStateInstalling(debPath);
}

void UpdateRunnerWindow::onDownloadFailed(const QString& reason) {
    enterStateFailed(reason, FailureCause::Download);
}

void UpdateRunnerWindow::onRetryScheduled(int attempt,
                                          int totalAttempts,
                                          int delayMs) {
    const int seconds = qMax(1, (delayMs + 999) / 1000);
    enterStateRetrying(attempt, totalAttempts, seconds);
}

void UpdateRunnerWindow::onRetryCountdownTick() {
    if (state_ != State::Retrying) {
        retry_countdown_->stop();
        return;
    }
    if (--retry_countdown_seconds_ <= 0) {
        retry_countdown_->stop();
        // Downloader will fire its own progress signal; UI flips back to
        // Downloading on the next progressChanged().
        return;
    }
    lbl_retry_->setText(
        QStringLiteral("Retrying (attempt %1 of %2) in %3 s…")
            .arg(retry_countdown_attempt_)
            .arg(retry_countdown_total_)
            .arg(retry_countdown_seconds_));
}

void UpdateRunnerWindow::onRestartCountdownTick() {
    if (state_ != State::InstallComplete) {
        restart_countdown_->stop();
        return;
    }
    if (--restart_countdown_seconds_ <= 0) {
        restart_countdown_->stop();
        execvOcu();
        return;
    }
    lbl_restart_->setText(
        QStringLiteral("Restarting in %1 s…").arg(restart_countdown_seconds_));
}

void UpdateRunnerWindow::onTryAgainClicked() {
    update::log::info(
        "runner",
        QStringLiteral("operator: try again (last cause=%1)")
            .arg(last_failure_cause_ == FailureCause::Install
                     ? "install"
                     : "download"));
    // Phase 8 Q4=A: install failure → re-run dpkg only; download failure →
    // re-run the entire flow.
    if (last_failure_cause_ == FailureCause::Install
        && !completed_deb_path_.isEmpty()
        && QFileInfo::exists(completed_deb_path_)) {
        enterStateInstalling(completed_deb_path_);
    } else {
        startDownload();
    }
}

void UpdateRunnerWindow::onCancelClicked() {
    update::log::info("runner",
                      QStringLiteral("operator: cancel from failure screen"));
    // Locked Phase 7 Q4=A: Cancel = exec OCU back, no install. The
    // dpkg_running marker (if Phase 8 made it that far) is intentionally
    // LEFT in place — Phase 9 will see it on next OCU launch and run
    // bdr-apply-update recover (dpkg --configure -a) to clean up. The
    // runner's flock releases automatically on exec().
    execvOcu();
}

void UpdateRunnerWindow::onShowDetailsToggled() {
    const bool checked = btn_show_details_->isChecked();
    btn_show_details_->setText(
        checked ? QStringLiteral("\u25BC  Hide details")
                : QStringLiteral("\u25B6  Show details"));
    log_tail_->setVisible(checked);
    if (checked) {
        appendLogTail();
    }
}

void UpdateRunnerWindow::onLogFileChanged() {
    if (log_tail_->isVisible()) {
        appendLogTail();
    }
    // QFileSystemWatcher drops the path when the inode changes (e.g. log
    // rotation). Re-add to keep watching.
    const QString log_path = deriveOcuLogPath();
    if (!log_path.isEmpty() && QFile::exists(log_path)
        && !log_watcher_->files().contains(log_path)) {
        log_watcher_->addPath(log_path);
    }
}

void UpdateRunnerWindow::appendLogTail(int max_lines) {
    const QString path = deriveOcuLogPath();
    if (path.isEmpty()) return;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return;
    const qint64 sz = f.size();
    const qint64 from = (sz > kLogTailMaxBytes)
                            ? (sz - kLogTailMaxBytes)
                            : 0;
    f.seek(from);
    const QByteArray bytes = f.readAll();
    f.close();

    const QString text = QString::fromUtf8(bytes);
    QStringList lines = text.split(QChar(QLatin1Char('\n')),
                                   Qt::SkipEmptyParts);
    if (lines.size() > max_lines) {
        lines = lines.mid(lines.size() - max_lines);
    }
    log_tail_->setPlainText(lines.join(QChar(QLatin1Char('\n'))));
    log_tail_->verticalScrollBar()->setValue(
        log_tail_->verticalScrollBar()->maximum());
}

void UpdateRunnerWindow::execvOcu() {
    // Direct execv of the OCU binary. The runner inherited the OCU's env
    // when it was spawned, so the OCU comes back up with the same env it
    // had pre-update. No re-run-the-launcher dance needed.
    update::log::info(
        "runner",
        QStringLiteral("execv'ing OCU: %1").arg(args_.ocuBinaryPath));

    const QByteArray ocu_path_bytes = args_.ocuBinaryPath.toLocal8Bit();
    QByteArray ocu_arg0 = ocu_path_bytes;
    char* const argv[] = { ocu_arg0.data(), nullptr };

    // Flush in-flight Qt events before handing off the process image.
    // ::execv replaces the running image — no return on success.
    QApplication::processEvents();

    if (::execv(ocu_path_bytes.constData(), argv) < 0) {
        update::log::error(
            "runner",
            QStringLiteral("execv failed: %1 errno=%2")
                .arg(args_.ocuBinaryPath)
                .arg(QString::fromLatin1(std::strerror(errno))));
        std::_Exit(EXIT_FAILURE);
    }
}

QString UpdateRunnerWindow::resolvePreviousDebPath(
    const QString& current_deb) const {
    QFileInfo curr_fi(current_deb);
    QDir cache_dir(curr_fi.absolutePath());
    if (!cache_dir.exists()) return {};
    const QFileInfoList entries = cache_dir.entryInfoList(
        QStringList() << QStringLiteral("bdr-coverage-planner_*_amd64.deb"),
        QDir::Files | QDir::NoDotAndDotDot,
        QDir::Time);  // newest first
    for (const QFileInfo& fi : entries) {
        if (fi.absoluteFilePath() == current_deb) continue;
        return fi.absoluteFilePath();  // first non-current = previous gen
    }
    return {};
}

QString UpdateRunnerWindow::formatBytesPerSec(qint64 bps) const {
    if (bps <= 0) return QStringLiteral("\u2014");
    const double mbps = static_cast<double>(bps) / (1024.0 * 1024.0);
    if (mbps >= 1.0) {
        return QStringLiteral("%1 MB/s").arg(mbps, 0, 'f', 1);
    }
    const double kbps = static_cast<double>(bps) / 1024.0;
    return QStringLiteral("%1 KB/s").arg(kbps, 0, 'f', 0);
}

QString UpdateRunnerWindow::formatSizeRow(qint64 received, qint64 total) const {
    if (total <= 0) {
        if (received <= 0) return QStringLiteral("Preparing…");
        return QStringLiteral("%1 received").arg(formatBytes(received));
    }
    return QStringLiteral("%1 of %2")
        .arg(formatBytes(received))
        .arg(formatBytes(total));
}

QString UpdateRunnerWindow::deriveOcuLogPath() {
    return update::log::currentLogPath();
}

}  // namespace f2c_cpp
