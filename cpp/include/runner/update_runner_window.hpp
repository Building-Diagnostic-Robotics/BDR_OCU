/**
 * @file update_runner_window.hpp
 * @brief Frameless window for the bdr-update-runner external binary.
 *
 * Phase 7 lifecycle:
 *   1. main() acquires the runner Lockfile.
 *   2. main() parses argv into a RunnerArgs struct (deb URL, expected SHA,
 *      version label, asset name, size, dark mode).
 *   3. main() instantiates UpdateRunnerWindow with those args.
 *   4. The window runs UpdateDownloader::start() with a synthesized
 *      VersionInfo, drives the progress UI, handles retries, and on
 *      success/SHA-verify shows "Download Complete" for ~2s before
 *      ::execv-ing the OCU binary back into existence (locked Q6=C: stop
 *      at Download Complete; Phase 8 inserts dpkg between SHA verify and
 *      execv).
 *   5. On terminal failure, the window stays open (locked Q4=A) with
 *      Try Again + Cancel.
 *
 * Visual language: mirrors UpdateModal (locked Q5=A) — same icon tile,
 * amber warning callout pattern, project tokens (uiThemeTokens). All
 * theme decisions piggyback on the OCU's QSettings dark_mode value
 * (already shared because both processes read the same settings file).
 *
 * No cancel during install (locked Q2=A): the close button (X) is
 * intentionally disabled while a download is in-flight. A Cancel button
 * appears ONLY in the terminal-failure error state.
 *
 * Hidden Show Details expander (locked Q3=C): a single QToolButton
 * toggles a QPlainTextEdit log tail showing the most recent N lines
 * read from update.log. Default = collapsed.
 */

#pragma once

#include <QString>
#include <QWidget>

#include "update/update_types.hpp"

class QFileSystemWatcher;
class QLabel;
class QPlainTextEdit;
class QProcess;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace f2c_cpp::update {
class UpdateDownloader;
}

namespace f2c_cpp {

/**
 * @brief Inputs the runner needs from argv.
 *
 * Populated by runner/main.cpp from CLI args. The runner refuses to start
 * if any required field is empty (see RunnerArgs::isValid).
 */
struct RunnerArgs {
    // Forward-install fields (Phase 7-8). Required when rollback==false.
    QString debUrl;
    QString sha256Url;        ///< matching .sha256 sidecar URL
    QString assetName;        ///< filename, used as on-disk cache key
    QString tag;              ///< for display only
    QString commitSha;        ///< for display + post-install banner gate
    qint64  sizeBytes = 0;

    bool    darkMode = false;
    QString ocuBinaryPath;    ///< absolute path to OCU; runner execv's this on completion

    // Phase 9 rollback fields. When rollback==true, the runner skips the
    // download/SHA stages entirely and goes straight to dpkg -i on
    // rollbackPrevDeb (an existing .deb in the cache, retained by
    // purgeOlderCachedDebs's "keep two generations" rule).
    bool    rollback = false;
    QString rollbackPrevDeb;

    bool isValid() const {
        if (ocuBinaryPath.isEmpty()) return false;
        if (rollback) {
            return !rollbackPrevDeb.isEmpty();
        }
        return !debUrl.isEmpty()
            && !sha256Url.isEmpty()
            && !assetName.isEmpty();
    }
};

class UpdateRunnerWindow : public QWidget {
    Q_OBJECT

public:
    explicit UpdateRunnerWindow(const RunnerArgs& args,
                                QWidget* parent = nullptr);
    ~UpdateRunnerWindow() override;

private slots:
    void onDownloadProgress(qint64 received, qint64 total);
    void onDownloadComplete(const QString& debPath);
    void onDownloadFailed(const QString& reason);
    void onRetryScheduled(int attempt, int totalAttempts, int delayMs);
    void onRetryCountdownTick();
    void onRestartCountdownTick();
    void onTryAgainClicked();
    void onCancelClicked();
    void onShowDetailsToggled();
    void onLogFileChanged();
    // Phase 8 dpkg pipe.
    void onDpkgReadyReadStdout();
    void onDpkgFinished(int exit_code, int exit_status);

private:
    enum class State {
        Downloading,
        Retrying,
        Installing,        ///< dpkg invocation in flight (Phase 8)
        InstallComplete,   ///< dpkg succeeded; pausing N s before execv
        Failed,
    };

    enum class FailureCause {
        None,
        Download,
        Install,
    };

    void buildUi();
    void applyStyle();
    void enterStateDownloading();
    void enterStateRetrying(int attempt, int totalAttempts, int seconds);
    void enterStateInstalling(const QString& debPath);
    void enterStateInstallComplete();
    void enterStateFailed(const QString& reason, FailureCause cause);
    void startDownload();
    /// Invoke `sudo -n /usr/bin/bdr-apply-update install <deb>` via QProcess.
    /// Output is teed into update.log via the wrapper itself; we also
    /// capture stdout+stderr in-process for terminal-failure surfacing.
    /// Phase 8 locked Q4=A — Try Again from an install failure re-runs
    /// THIS function (not the full download) since the .deb is verified.
    void runDpkg();
    /// Replace this process image with the OCU binary. The runner
    /// inherited the OCU's env when it was spawned, so the OCU comes
    /// up with the same env it had pre-update. No `return` on success.
    void execvOcu();
    /// Resolve previous-generation .deb in the cache for the rollback
    /// marker (Phase 9 reads this). Empty if no other .deb exists yet.
    QString resolvePreviousDebPath(const QString& current_deb) const;
    void appendLogTail(int max_lines = 30);
    QString formatBytesPerSec(qint64 bytes_per_sec) const;
    QString formatSizeRow(qint64 received, qint64 total) const;

    static QString deriveOcuLogPath();

    RunnerArgs args_;
    State state_ = State::Downloading;
    FailureCause last_failure_cause_ = FailureCause::None;

    update::UpdateDownloader* downloader_ = nullptr;
    QString completed_deb_path_;
    QString previous_deb_path_;  ///< stashed in marker for Phase 9 rollback
    int     install_attempts_ = 0;

    // dpkg child process (Phase 8). Owned by `this`; killed in dtor.
    QProcess* dpkg_proc_ = nullptr;

    // Header
    QLabel* lbl_icon_tile_ = nullptr;
    QLabel* lbl_title_ = nullptr;
    QLabel* lbl_subtitle_ = nullptr;

    // Status
    QLabel* lbl_status_ = nullptr;
    QLabel* lbl_size_row_ = nullptr;
    QProgressBar* progress_ = nullptr;

    // Retry strip (visible only in State::Retrying)
    QLabel* lbl_retry_ = nullptr;

    // Restart strip (visible only in State::DownloadComplete)
    QLabel* lbl_restart_ = nullptr;

    // Failure strip (visible only in State::Failed)
    QLabel* lbl_warning_ = nullptr;
    QPushButton* btn_try_again_ = nullptr;
    QPushButton* btn_cancel_ = nullptr;

    // Details expander (always present, hidden by default)
    QToolButton* btn_show_details_ = nullptr;
    QPlainTextEdit* log_tail_ = nullptr;
    QFileSystemWatcher* log_watcher_ = nullptr;

    // Timers
    QTimer* retry_countdown_ = nullptr;
    int retry_countdown_seconds_ = 0;
    int retry_countdown_attempt_ = 0;
    int retry_countdown_total_ = 0;

    QTimer* restart_countdown_ = nullptr;
    int restart_countdown_seconds_ = 0;

    // Throughput tracking for the size row.
    qint64 last_progress_received_ = 0;
    qint64 last_progress_at_ms_ = 0;
    double smoothed_bps_ = 0.0;
};

}  // namespace f2c_cpp
