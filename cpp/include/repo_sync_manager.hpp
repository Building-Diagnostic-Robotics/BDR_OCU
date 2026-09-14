/**
 * @file repo_sync_manager.hpp
 * @brief OCU-side orchestrator for laptop <-> robot ~/pilot_ws sync.
 *
 * Ported from pilot_control/scripts/F2C/cpp (the legacy OCU copy). The
 * orchestration lives in this binary, not a script inside the git repo,
 * so a branch switch cannot delete the code that performs it.
 *
 * Robot helpers live outside git at ~/pilot_deploy/ and are installed
 * once by scripts/deploy/install_deploy_helpers.sh.
 *
 * Production always targets kRobotDeployBranch ("cliff-on-autonomy").
 * Sync now checks that branch out if the laptop is elsewhere (clean
 * tree required). Check-only never mutates.
 *
 * Safety: no reset --hard, checkout -f, clean, or push --force. Dirty
 * trees, divergence, and a misconfigured robot are hard fails.
 */

#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

class QTimer;

namespace f2c_cpp {

struct RepoSyncSnapshot {
    QString deploy_branch;
    QString laptop_branch;
    QString laptop_sha;
    QString origin_sha;
    QString robot_branch;
    QString robot_sha;
    int behind = 0;
    int ahead = 0;
    bool origin_offline = false;
    bool laptop_dirty = false;
    bool laptop_on_deploy = false;
    bool robot_online = false;
    bool robot_repo_ok = false;
    bool robot_helpers_ok = false;
    bool robot_recv_ok = false;
    bool robot_dirty = false;
    /// Banner should show (actionable laptop/robot software update).
    bool offer_update = false;
    /// Laptop is current; robot was unreachable this check.
    bool robot_pending = false;
    /// Helpers or receive.denyCurrentBranch need install_deploy_helpers.sh.
    bool needs_prepare = false;
    QString headline;
    QString detail;
};

class RepoSyncManager : public QObject {
    Q_OBJECT
public:
    enum Level { LevelOk = 0, LevelWarn = 1, LevelBad = 2 };

    enum class Reachability { Unknown, Online, Offline };

    explicit RepoSyncManager(QObject* parent = nullptr);

    void setRobotHost(const QString& host) { robotHost_ = host.trimmed(); }
    void setRobotUser(const QString& user) { robotUser_ = user.trimmed(); }
    void setBranch(const QString& branch);
    QString branch() const { return branch_; }
    bool isBusy() const { return busy_; }
    bool isMutating() const { return busy_ && mode_ != Mode::Check; }
    RepoSyncSnapshot lastSnapshot() const { return snapshot_; }

    static bool isSafeBranchName(const QString& branch);
    static bool isSafeHost(const QString& host);
    /// prefix → colcon packages. Lock-step with rebuild_affected.sh.
    static QStringList packagesForChangedFiles(const QStringList& changedFiles);

    void setRobotReachable(bool online) {
        reach_ = online ? Reachability::Online : Reachability::Offline;
    }
    /// Probe disarmed / Idle — SSH decides; do not treat as offline.
    void clearRobotReachable() { reach_ = Reachability::Unknown; }

public slots:
    void checkOnly();
    void syncNow();
    void switchBranch(const QString& branch);
    /// Copies helpers + sets receive.denyCurrentBranch=updateInstead.
    void prepareRobot();

signals:
    void syncStarted(const QString& stage);
    void syncFinished(int level, const QString& headline, const QString& detail);
    void branchChanged(const QString& branch);
    /// Fired after every checkOnly() (and after a mutating run). AppShell
    /// uses offer_update for the banner; failures stay silent on the
    /// background timer unless they are a real mismatch/update.
    void snapshotReady(const f2c_cpp::RepoSyncSnapshot& snap);

private slots:
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessError(QProcess::ProcessError error);
    void onWatchdogTimeout();

private:
    enum class Mode { None, Check, Sync, Switch, Prepare };
    enum class Stage {
        Idle,
        LaptopState,
        Fetch,
        RemoteState,
        Checkout,
        Merge,
        Diff,
        LaptopBuild,
        RobotState,
        Push,
        RobotSwitch,
        RobotBuild,
        RobotVerify,
        Prepare,
    };

    void begin(Mode mode, const QString& firstStageLabel);
    void startStage(Stage s);
    void finish(Level level, const QString& headline, const QString& detail = QString());
    void runBash(const QString& cmd, int timeoutMs);
    QString sshPrefix(int connectTimeoutSec) const;

    void reconcileRobot();
    void beginRobotPhase();
    void finishRobotDeferred(const QString& why);
    void reportCheck();
    void fillSnapshot(Level level, const QString& headline, const QString& detail);

    QString workspacePath() const;
    QString robotRepoUrl() const;
    QString robotDeployDir() const;
    QString helperInstallScript() const;
    QStringList workspacePackageNames() const;
    QStringList affectedPackages(const QStringList& changedFiles) const;

    Mode mode_ = Mode::None;
    Stage stage_ = Stage::Idle;
    QString robotHost_;
    QString robotUser_ = QStringLiteral("roofus");
    QString branch_;

    QString laptopBranch_;
    bool laptopDirty_ = false;
    QString laptopHeadBefore_;
    QString laptopHeadAfter_;

    bool originOffline_ = false;
    QString originSha_;
    QString localSha_;
    int behind_ = 0;
    int ahead_ = 0;

    Reachability reach_ = Reachability::Unknown;
    bool robotOnline_ = false;
    QString robotBranch_;
    QString robotHead_;
    bool robotDirty_ = false;
    QString robotRecvCfg_;
    bool robotRepoOk_ = false;
    bool robotHelpersOk_ = false;
    bool robotNeedsSwitch_ = false;

    QStringList changedFiles_;
    RepoSyncSnapshot snapshot_;

    QProcess* proc_ = nullptr;
    QTimer* watchdog_ = nullptr;
    bool busy_ = false;
    QString stdOut_;
    QString stdErr_;
};

}  // namespace f2c_cpp
