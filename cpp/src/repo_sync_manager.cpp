/**
 * @file repo_sync_manager.cpp
 * @brief Implementation of RepoSyncManager (laptop-side orchestration).
 *
 * All laptop-side git/colcon/ssh orchestration lives here, compiled into the
 * OCU binary, so branch switches cannot change or remove the code that is
 * performing them. Robot-side helpers are invoked over SSH from
 * ~/pilot_deploy/ (outside git).
 *
 * See repo_sync_manager.hpp for the safety contract this file implements.
 */

#include "repo_sync_manager.hpp"
#include "settings_constants.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QList>
#include <QRegularExpression>
#include <QTimer>

namespace f2c_cpp {

namespace {

// Shell fragments used to source the ROS environment inside /bin/bash -c.
// No `set -u` here: the ROS setup scripts reference undefined variables.
const QString kSourceRos =
    QStringLiteral("source /opt/ros/humble/setup.bash 2>/dev/null; ");
const QString kSourceWs =
    QStringLiteral("source $HOME/pilot_ws/install/setup.bash 2>/dev/null; ");

// Per-stage watchdogs. A hung ssh must never leave the OCU busy forever, but
// builds legitimately take minutes, so the budget is per stage rather than
// global.
constexpr int kTimeoutProbeMs   = 30 * 1000;
constexpr int kTimeoutNetworkMs = 3 * 60 * 1000;
constexpr int kTimeoutBuildMs   = 45 * 60 * 1000;

// ssh exits 255 for its own failures (connection refused, timeout, no route,
// auth). Any other non-zero code came from the remote command itself, which
// means the robot IS reachable and something is actually wrong.
constexpr int kSshFailureExit = 255;

/// Pull "key=value" out of a probe script's stdout.
QString kv(const QString& text, const QString& key) {
    const QString prefix = key + QLatin1Char('=');
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        const QString t = line.trimmed();
        if (t.startsWith(prefix)) {
            return t.mid(prefix.size()).trimmed();
        }
    }
    return QString();
}

QString shortSha(const QString& sha) { return sha.left(12); }

/// Hex SHA from `git rev-parse`. Interpolated into /bin/bash -c.
bool isSafeSha(const QString& sha) {
    static const QRegularExpression re(QStringLiteral("^[0-9a-fA-F]{7,40}$"));
    return re.match(sha).hasMatch();
}

bool isPackageXmlPath(const QString& path) {
    return path == QStringLiteral("package.xml") ||
           path.endsWith(QStringLiteral("/package.xml"));
}

bool isBuildAffecting(const QString& f) {
    static const QStringList exts = {
        QStringLiteral(".cpp"), QStringLiteral(".hpp"), QStringLiteral(".h"),
        QStringLiteral(".hh"), QStringLiteral(".c"), QStringLiteral(".cc"),
        QStringLiteral(".cxx"), QStringLiteral(".inl"), QStringLiteral(".msg"),
        QStringLiteral(".srv"), QStringLiteral(".action"), QStringLiteral(".idl"),
    };
    for (const QString& e : exts) {
        if (f.endsWith(e)) return true;
    }
    const QString base = f.section(QLatin1Char('/'), -1);
    return base == QStringLiteral("CMakeLists.txt") ||
           base == QStringLiteral("package.xml") ||
           base == QStringLiteral("setup.py") ||
           base == QStringLiteral("setup.cfg");
}

struct Mapping { QString prefix; QStringList pkgs; };

const QList<Mapping>& packageMappings() {
    static const QList<Mapping> mappings = {
        { QStringLiteral("src/pilot_control/"), { QStringLiteral("pilot_control") } },
        { QStringLiteral("src/ros_odrive/"), { QStringLiteral("odrive_can"),
                                               QStringLiteral("odrive_ros2_control"),
                                               QStringLiteral("odrive_botwheel_explorer") } },
        { QStringLiteral("src/livox_ros_driver2/"), { QStringLiteral("livox_ros_driver2") } },
        { QStringLiteral("src/FAST_LIO/"), { QStringLiteral("fast_lio") } },
        { QStringLiteral("src/walabot_driver/"), { QStringLiteral("walabot_driver") } },
        { QStringLiteral("src/Livox-SDK2/"), { QStringLiteral("livox_ros_driver2"),
                                               QStringLiteral("fast_lio") } },
    };
    return mappings;
}

bool pathHasKnownPrefix(const QString& f) {
    for (const Mapping& m : packageMappings()) {
        if (f.startsWith(m.prefix)) return true;
    }
    return false;
}

/// Last few lines of output, for the failure detail field.
QString tail(const QString& text, int lines = 6) {
    const QStringList all = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (all.size() <= lines) return all.join(QLatin1Char('\n'));
    return all.mid(all.size() - lines).join(QLatin1Char('\n'));
}

}  // namespace

bool RepoSyncManager::isSafeBranchName(const QString& branch) {
    // Branch names come from `git ls-remote origin`, i.e. from the remote, and
    // are interpolated into shell commands. Accept only the conservative set
    // that real branches use, and reject the git refname traps.
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._/+-]{0,199}$"));
    if (!re.match(branch).hasMatch()) return false;
    if (branch.contains(QStringLiteral(".."))) return false;
    if (branch.endsWith(QLatin1Char('/')) || branch.endsWith(QStringLiteral(".lock"))) return false;
    return true;
}

bool RepoSyncManager::isSafeHost(const QString& host) {
    // Hostname or IPv4 literal. Deliberately excludes quotes, spaces, ';',
    // '$' and '&' — this string is interpolated into a /bin/bash -c command.
    static const QRegularExpression re(
        QStringLiteral("^[A-Za-z0-9_]([A-Za-z0-9._-]{0,253}[A-Za-z0-9_])?$"));
    return re.match(host).hasMatch();
}

void RepoSyncManager::setBranch(const QString& branch) {
    const QString b = branch.trimmed();
    if (isSafeBranchName(b)) {
        branch_ = b;
    }
}

RepoSyncManager::RepoSyncManager(QObject* parent)
    : QObject(parent)
    , branch_(QString::fromLatin1(kRobotDeployBranch))
{
    proc_ = new QProcess(this);
    connect(proc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &RepoSyncManager::onProcessFinished);
    connect(proc_, &QProcess::errorOccurred,
            this, &RepoSyncManager::onProcessError);

    watchdog_ = new QTimer(this);
    watchdog_->setSingleShot(true);
    connect(watchdog_, &QTimer::timeout, this, &RepoSyncManager::onWatchdogTimeout);
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

void RepoSyncManager::begin(Mode mode, const QString& firstStageLabel) {
    mode_ = mode;
    busy_ = true;
    if (!isSafeHost(robotHost_) || !isSafeHost(robotUser_)) {
        finish(LevelBad, QStringLiteral("Invalid robot address"),
               QStringLiteral("'%1@%2' is not a usable host. Fix the Robot IP field.")
                   .arg(robotUser_, robotHost_));
        return;
    }
    originOffline_ = false;
    robotOnline_ = false;
    robotNeedsSwitch_ = false;
    robotRepoOk_ = false;
    robotHelpersOk_ = false;
    robotDirty_ = false;
    robotBranch_.clear();
    robotHead_.clear();
    robotRecvCfg_.clear();
    behind_ = 0;
    ahead_ = 0;
    changedFiles_.clear();
    switchFromHead_.clear();
    fullBuild_ = false;
    affectedPkgs_.clear();
    emit syncStarted(firstStageLabel);
    if (mode == Mode::Prepare) {
        startStage(Stage::Prepare);
        return;
    }
    startStage(Stage::LaptopState);
}

void RepoSyncManager::checkOnly() {
    if (busy_) return;
    begin(Mode::Check, QStringLiteral("Checking status…"));
}

void RepoSyncManager::syncNow() {
    if (busy_) return;
    begin(Mode::Sync, QStringLiteral("Checking for updates…"));
}

void RepoSyncManager::switchBranch(const QString& branch) {
    if (busy_) return;
    const QString b = branch.trimmed();
    if (b.isEmpty()) return;
    if (!isSafeBranchName(b)) {
        busy_ = true;   // finish() clears it and emits the failure.
        finish(LevelBad, QStringLiteral("Invalid branch name"),
               QStringLiteral("Refusing to use '%1' in a shell command.").arg(b));
        return;
    }
    branch_ = b;
    begin(Mode::Switch, QStringLiteral("Switching to %1…").arg(branch_));
}

void RepoSyncManager::prepareRobot() {
    if (busy_) return;
    begin(Mode::Prepare, QStringLiteral("Preparing robot…"));
}

void RepoSyncManager::finish(Level level, const QString& headline, const QString& detail) {
    // Order matters: clearing stage_ first makes any straggler signal from a
    // killed QProcess a no-op, so we can never finish twice.
    stage_ = Stage::Idle;
    mode_ = Mode::None;
    busy_ = false;
    if (watchdog_) watchdog_->stop();
    fillSnapshot(level, headline, detail);
    emit syncFinished(static_cast<int>(level), headline, detail);
    emit snapshotReady(snapshot_);
}

// ---------------------------------------------------------------------------
// Process plumbing
// ---------------------------------------------------------------------------

QString RepoSyncManager::workspacePath() const {
    return QDir::homePath() + QStringLiteral("/pilot_ws");
}

QString RepoSyncManager::robotRepoUrl() const {
    return QStringLiteral("ssh://%1@%2/home/%3/pilot_ws")
        .arg(robotUser_, robotHost_, robotUser_);
}

QString RepoSyncManager::robotDeployDir() const {
    return QStringLiteral("/home/%1/pilot_deploy").arg(robotUser_);
}

QString RepoSyncManager::helperInstallScript() const {
    return workspacePath() +
           QStringLiteral("/src/pilot_control/scripts/deploy/install_deploy_helpers.sh");
}

QString RepoSyncManager::sshPrefix(int connectTimeoutSec) const {
    // BatchMode: never sit at a password prompt (that is what used to hang).
    // StrictHostKeyChecking=accept-new: a re-imaged robot must not wedge the
    // flow on an interactive prompt, but a CHANGED key still refuses.
    return QStringLiteral("ssh -o BatchMode=yes -o ConnectTimeout=%1 "
                          "-o StrictHostKeyChecking=accept-new %2@%3 ")
        .arg(QString::number(connectTimeoutSec), robotUser_, robotHost_);
}

void RepoSyncManager::runBash(const QString& cmd, int timeoutMs) {
    stdOut_.clear();
    stdErr_.clear();
    proc_->setWorkingDirectory(workspacePath());
    proc_->start(QStringLiteral("/bin/bash"), QStringList() << QStringLiteral("-c") << cmd);
    if (watchdog_) watchdog_->start(timeoutMs);
}

void RepoSyncManager::onWatchdogTimeout() {
    if (stage_ == Stage::Idle) return;
    const int hung = static_cast<int>(stage_);
    finish(LevelBad, QStringLiteral("Sync timed out"),
           QStringLiteral("Stage %1 exceeded its time budget and was aborted. "
                          "Nothing was forced; re-run when the link is stable.")
               .arg(hung));
    if (proc_ && proc_->state() != QProcess::NotRunning) {
        proc_->kill();
    }
}

void RepoSyncManager::onProcessError(QProcess::ProcessError error) {
    if (stage_ == Stage::Idle) return;   // already finished; ignore stragglers
    if (error == QProcess::FailedToStart) {
        finish(LevelBad, QStringLiteral("Sync failed"),
               QStringLiteral("Could not start /bin/bash (stage %1).")
                   .arg(static_cast<int>(stage_)));
    }
    // Crashed/other errors also raise finished(); let that path handle them so
    // the exit code is available.
}

// ---------------------------------------------------------------------------
// Stage dispatch
// ---------------------------------------------------------------------------

void RepoSyncManager::startStage(Stage s) {
    stage_ = s;
    switch (s) {
    case Stage::LaptopState:
        // The guard makes a missing/broken repo a non-zero exit, so it is
        // reported as such instead of being mistaken for a detached HEAD.
        runBash(QStringLiteral(
                    "git rev-parse --is-inside-work-tree >/dev/null 2>&1 || exit 3; "
                    "printf 'branch=%s\\n' \"$(git rev-parse --abbrev-ref HEAD 2>/dev/null)\"; "
                    "printf 'head=%s\\n' \"$(git rev-parse HEAD 2>/dev/null)\"; "
                    "printf 'dirty=%s\\n' \"$(git status --porcelain --untracked-files=no | head -1)\""),
                kTimeoutProbeMs);
        break;

    case Stage::Fetch:
        emit syncStarted(QStringLiteral("Fetching from origin…"));
        // Fetch only moves remote-tracking refs; it never touches the worktree
        // or a local branch, so it is safe in the read-only check too.
        runBash(QStringLiteral("git fetch --prune origin"), kTimeoutNetworkMs);
        break;

    case Stage::RemoteState:
        runBash(QStringLiteral(
                    "printf 'origin=%s\\n' \"$(git rev-parse --verify --quiet refs/remotes/origin/%1)\"; "
                    "printf 'local=%s\\n' \"$(git rev-parse --verify --quiet refs/heads/%1)\"; "
                    "printf 'counts=%s\\n' \"$(git rev-list --left-right --count "
                    "refs/remotes/origin/%1...refs/heads/%1 2>/dev/null)\"")
                    .arg(branch_),
                kTimeoutProbeMs);
        break;

    case Stage::Checkout:
        emit syncStarted(QStringLiteral("Checking out %1…").arg(branch_));
        // Plain checkout — no -f, no -B. A tracking branch is created only if
        // the local branch does not exist yet. The trailing rev-parse lets the
        // handler VERIFY the switch actually landed rather than assume it.
        runBash(QStringLiteral(
                    "{ git checkout %1 || git checkout -b %1 --track origin/%1; } && "
                    "printf 'branch=%s\\n' \"$(git rev-parse --abbrev-ref HEAD)\" && "
                    "printf 'head=%s\\n' \"$(git rev-parse HEAD)\"")
                    .arg(branch_),
                kTimeoutNetworkMs);
        break;

    case Stage::Merge:
        emit syncStarted(QStringLiteral("Fast-forwarding %1…").arg(branch_));
        // --ff-only is the whole safety story: if it is not a fast-forward,
        // git refuses and nothing moves.
        runBash(QStringLiteral("git merge --ff-only origin/%1 && "
                               "printf 'head=%s\\n' \"$(git rev-parse HEAD)\"")
                    .arg(branch_),
                kTimeoutNetworkMs);
        break;

    case Stage::Diff: {
        const QString from =
            !switchFromHead_.isEmpty() ? switchFromHead_ : laptopHeadBefore_;
        if (!isSafeSha(from) || !isSafeSha(laptopHeadAfter_)) {
            // Cannot name the range safely — rebuild everything rather
            // than interpolate an untrusted string or skip the compile.
            fullBuild_ = true;
            startStage(Stage::LaptopBuild);
            return;
        }
        runBash(QStringLiteral("git diff --name-status %1 %2")
                    .arg(from, laptopHeadAfter_),
                kTimeoutProbeMs);
        break;
    }

    case Stage::LaptopBuild: {
        if (fullBuild_) {
            emit syncStarted(QStringLiteral("Rebuilding laptop (full)…"));
            runBash(kSourceRos + kSourceWs +
                        QStringLiteral("cd $HOME/pilot_ws && colcon build --symlink-install"),
                    kTimeoutBuildMs);
            break;
        }
        emit syncStarted(QStringLiteral("Rebuilding affected packages…"));
        runBash(kSourceRos + kSourceWs +
                    // --packages-above builds the named packages AND
                    // everything that recursively depends on them. (There
                    // is no --packages-above-and-including; colcon exits 2
                    // on an unrecognized flag and builds nothing.)
                    QStringLiteral("cd $HOME/pilot_ws && colcon build --symlink-install "
                                   "--packages-above %1")
                        .arg(affectedPkgs_.join(QLatin1Char(' '))),
                kTimeoutBuildMs);
        break;
    }

    case Stage::RobotState:
    case Stage::RobotVerify:
        emit syncStarted(stage_ == Stage::RobotVerify
                             ? QStringLiteral("Verifying robot…")
                             : QStringLiteral("Checking robot…"));
        // One round trip for everything we need to know. Single quotes keep
        // the laptop shell from expanding it; the remote shell does. Exits 0
        // even when the repo is missing, so exit 255 unambiguously means
        // "could not reach the robot". helpers=ok also requires --no-build
        // in robot_switch_branch.sh: a stale helper is executable but every
        // switch would fail on the unknown arg.
        runBash(sshPrefix(stage_ == Stage::RobotVerify ? 10 : 5) +
                    QStringLiteral(
                        "'cd $HOME/pilot_ws 2>/dev/null || { printf \"repo=missing\\n\"; exit 0; }; "
                        "printf \"repo=ok\\n\"; "
                        "printf \"branch=%s\\n\" \"$(git rev-parse --abbrev-ref HEAD 2>/dev/null)\"; "
                        "printf \"head=%s\\n\" \"$(git rev-parse HEAD 2>/dev/null)\"; "
                        "printf \"dirty=%s\\n\" \"$(git status --porcelain --untracked-files=no | head -1)\"; "
                        "printf \"recv=%s\\n\" \"$(git config --get receive.denyCurrentBranch)\"; "
                        "if [ -x $HOME/pilot_deploy/rebuild_affected.sh ] && "
                        "[ -x $HOME/pilot_deploy/robot_switch_branch.sh ] && "
                        "grep -q -- --no-build $HOME/pilot_deploy/robot_switch_branch.sh; "
                        "then printf \"helpers=ok\\n\"; else printf \"helpers=missing\\n\"; fi'"),
                kTimeoutProbeMs);
        break;

    case Stage::Push:
        emit syncStarted(QStringLiteral("Pushing to robot…"));
        // Explicit refspec, no --force. The robot's branch tip can only move
        // forward; a non-fast-forward is rejected by the receiving repo.
        runBash(QStringLiteral("GIT_SSH_COMMAND='ssh -o BatchMode=yes -o ConnectTimeout=10 "
                               "-o StrictHostKeyChecking=accept-new' "
                               "git push %1 refs/heads/%2:refs/heads/%2")
                    .arg(robotRepoUrl(), branch_),
                kTimeoutNetworkMs);
        break;

    case Stage::RobotSwitch: {
        emit syncStarted(QStringLiteral("Switching robot to %1…").arg(branch_));
        if (!isSafeSha(laptopHeadAfter_)) {
            finish(LevelBad, QStringLiteral("Robot switch failed"),
                   QStringLiteral("Laptop HEAD is not a usable SHA; refusing to run the switch script."));
            return;
        }
        // Always --no-build. rebuild_affected.sh decides scoped vs full
        // from the robot's own before/after range.
        runBash(sshPrefix(10) + QStringLiteral("'%1/robot_switch_branch.sh --no-build %2 %3'")
                                    .arg(robotDeployDir(), branch_, laptopHeadAfter_),
                kTimeoutNetworkMs);
        break;
    }

    case Stage::RobotBuild:
        emit syncStarted(QStringLiteral("Rebuilding robot…"));
        if (!isSafeSha(robotHead_) || !isSafeSha(laptopHeadAfter_)) {
            finish(LevelBad, QStringLiteral("Robot build failed"),
                   QStringLiteral("Missing a usable before/after SHA; refusing to start the rebuild."));
            return;
        }
        runBash(sshPrefix(10) + QStringLiteral("'%1/rebuild_affected.sh %2 %3'")
                                    .arg(robotDeployDir(), robotHead_, laptopHeadAfter_),
                kTimeoutBuildMs);
        break;

    case Stage::Prepare: {
        const QString script = helperInstallScript();
        emit syncStarted(QStringLiteral("Installing robot deploy helpers…"));
        runBash(QStringLiteral("test -x \"%1\" || exit 3; \"%1\" %2@%3")
                    .arg(script, robotUser_, robotHost_),
                kTimeoutNetworkMs);
        break;
    }

    case Stage::Idle:
        break;
    }
}

// ---------------------------------------------------------------------------
// Stage completion
// ---------------------------------------------------------------------------

void RepoSyncManager::onProcessFinished(int exitCode, QProcess::ExitStatus status) {
    if (stage_ == Stage::Idle) return;   // watchdog already finished this run
    if (watchdog_) watchdog_->stop();

    stdOut_ = QString::fromUtf8(proc_->readAllStandardOutput());
    stdErr_ = QString::fromUtf8(proc_->readAllStandardError());
    const bool ok = (status == QProcess::NormalExit && exitCode == 0);
    const QString detail = tail(stdErr_.isEmpty() ? stdOut_ : stdErr_);

    switch (stage_) {

    // ---- laptop -----------------------------------------------------------
    case Stage::LaptopState: {
        if (!ok) {
            finish(LevelBad, QStringLiteral("Not a git workspace"),
                   QStringLiteral("%1 is not a usable git repository.\n%2")
                       .arg(workspacePath(), detail));
            return;
        }
        laptopBranch_ = kv(stdOut_, QStringLiteral("branch"));
        laptopHeadBefore_ = kv(stdOut_, QStringLiteral("head"));
        laptopHeadAfter_ = laptopHeadBefore_;
        laptopDirty_ = !kv(stdOut_, QStringLiteral("dirty")).isEmpty();

        if (laptopBranch_.isEmpty() || laptopBranch_ == QStringLiteral("HEAD")) {
            finish(LevelBad, QStringLiteral("Laptop not on a branch"),
                   QStringLiteral("%1 is in detached HEAD (or has no commits). "
                                  "Check out a branch before syncing. Nothing was changed.")
                       .arg(workspacePath()));
            return;
        }

        if (mode_ == Mode::Switch) {
            if (laptopBranch_ == branch_) {
                // Already there — degrade to a plain sync instead of doing a
                // pointless full rebuild.
                mode_ = Mode::Sync;
            } else if (laptopDirty_) {
                finish(LevelBad, QStringLiteral("Uncommitted changes"),
                       QStringLiteral("The laptop worktree has uncommitted changes on '%1'. "
                                      "Commit or stash them before switching to '%2' — "
                                      "the switch will not carry or discard them.")
                           .arg(laptopBranch_, branch_));
                return;
            }
        }

        // Production always targets kRobotDeployBranch. A laptop sitting
        // on something else is a switch, not a merge-across-branches.
        if (mode_ == Mode::Sync && laptopBranch_ != branch_) {
            if (laptopDirty_) {
                finish(LevelBad, QStringLiteral("Uncommitted changes"),
                       QStringLiteral("The laptop worktree has uncommitted changes on '%1'. "
                                      "Commit or stash them before switching to '%2'.")
                           .arg(laptopBranch_, branch_));
                return;
            }
            mode_ = Mode::Switch;
        }

        if (!isSafeBranchName(branch_)) {
            finish(LevelBad, QStringLiteral("Unsupported branch name"),
                   QStringLiteral("Refusing to operate on '%1'.").arg(branch_));
            return;
        }
        startStage(Stage::Fetch);
        return;
    }

    case Stage::Fetch:
        // No origin (field use, no internet) is not fatal: the laptop<->robot
        // half of the job can still be done against whatever the laptop has.
        originOffline_ = !ok;
        startStage(Stage::RemoteState);
        return;

    case Stage::RemoteState: {
        originSha_ = kv(stdOut_, QStringLiteral("origin"));
        localSha_ = kv(stdOut_, QStringLiteral("local"));
        const QStringList counts =
            kv(stdOut_, QStringLiteral("counts")).split(QRegularExpression(QStringLiteral("\\s+")),
                                                        Qt::SkipEmptyParts);
        behind_ = (counts.size() == 2) ? counts.at(0).toInt() : 0;
        ahead_ = (counts.size() == 2) ? counts.at(1).toInt() : 0;

        if (mode_ == Mode::Check) {
            beginRobotPhase();
            return;
        }

        if (mode_ == Mode::Switch) {
            if (originSha_.isEmpty() && localSha_.isEmpty()) {
                finish(LevelBad, QStringLiteral("Branch not found"),
                       QStringLiteral("'%1' exists neither locally nor on origin%2.")
                           .arg(branch_, originOffline_ ? QStringLiteral(" (origin unreachable)")
                                                        : QString()));
                return;
            }
        }

        // Diverged: both sides have commits the other lacks. There is no
        // non-destructive way to reconcile this automatically, so stop.
        if (!originSha_.isEmpty() && !localSha_.isEmpty() && behind_ > 0 && ahead_ > 0) {
            finish(LevelBad, QStringLiteral("Branch diverged"),
                   QStringLiteral("'%1' is %2 behind and %3 ahead of origin. "
                                  "Rebase or merge it yourself — the OCU will not "
                                  "rewrite history.")
                       .arg(branch_).arg(behind_).arg(ahead_));
            return;
        }

        if (mode_ == Mode::Switch) {
            startStage(Stage::Checkout);
            return;
        }

        // Sync mode: fast-forward only when there is something to fast-forward.
        if (!originSha_.isEmpty() && behind_ > 0) {
            startStage(Stage::Merge);
        } else {
            beginRobotPhase();
        }
        return;
    }

    case Stage::Checkout: {
        const QString nowOn = kv(stdOut_, QStringLiteral("branch"));
        if (!ok || nowOn != branch_) {
            finish(LevelBad, QStringLiteral("Checkout failed"),
                   QStringLiteral("Could not check out '%1' (still on '%2').\n%3")
                       .arg(branch_, nowOn.isEmpty() ? laptopBranch_ : nowOn, detail));
            return;
        }
        if (switchFromHead_.isEmpty()) {
            switchFromHead_ = laptopHeadBefore_;
        }
        laptopBranch_ = branch_;
        laptopHeadBefore_ = kv(stdOut_, QStringLiteral("head"));
        laptopHeadAfter_ = laptopHeadBefore_;
        emit branchChanged(branch_);
        if (!originSha_.isEmpty() && behind_ > 0) {
            startStage(Stage::Merge);
        } else {
            startStage(Stage::Diff);
        }
        return;
    }

    case Stage::Merge:
        if (!ok) {
            finish(LevelBad, QStringLiteral("Fast-forward refused"),
                   QStringLiteral("'%1' could not be fast-forwarded to origin. "
                                  "Nothing was changed.\n%2").arg(branch_, detail));
            return;
        }
        laptopHeadAfter_ = kv(stdOut_, QStringLiteral("head"));
        behind_ = 0;
        localSha_ = laptopHeadAfter_;
        startStage(Stage::Diff);
        return;

    case Stage::Diff: {
        const NameStatusParse parsed = parseNameStatus(stdOut_);
        changedFiles_ = parsed.files;
        affectedPkgs_ = affectedPackages(changedFiles_);
        fullBuild_ = parsed.package_xml_added_or_removed ||
                     needsFullWorkspaceBuild(changedFiles_);
        if (fullBuild_ || !affectedPkgs_.isEmpty()) {
            startStage(Stage::LaptopBuild);
        } else {
            // Python/launch/config only — symlink-install already picked it up.
            beginRobotPhase();
        }
        return;
    }

    case Stage::LaptopBuild:
        if (!ok) {
            finish(LevelBad, QStringLiteral("Laptop build failed"),
                   QStringLiteral("The laptop is on '%1' @ %2 but did not build.\n%3")
                       .arg(branch_, shortSha(laptopHeadAfter_), tail(stdOut_ + stdErr_, 12)));
            return;
        }
        beginRobotPhase();
        return;

    // ---- robot ------------------------------------------------------------
    case Stage::RobotState: {
        if (!ok && exitCode == kSshFailureExit) {
            if (reach_ == Reachability::Online) {
                // It answers ping but not ssh, so this is not a connectivity
                // problem to shrug off — it is sshd, keys, or host-key
                // checking, and it needs the operator's attention.
                finish(LevelBad, QStringLiteral("Robot not reachable over SSH"),
                       QStringLiteral("%1 answers ping but the SSH connection failed. "
                                      "Check sshd on the robot and the key for %2@%1.\n%3")
                           .arg(robotHost_, robotUser_, detail));
                return;
            }
            robotOnline_ = false;
            if (mode_ == Mode::Check) { reportCheck(); return; }
            finishRobotDeferred(QStringLiteral("robot unreachable at %1").arg(robotHost_));
            return;
        }
        if (!ok) {
            finish(LevelBad, QStringLiteral("Robot probe failed"),
                   QStringLiteral("Reached %1 but the status probe failed (exit %2).\n%3")
                       .arg(robotHost_).arg(exitCode).arg(detail));
            return;
        }
        robotOnline_ = true;
        robotRepoOk_ = (kv(stdOut_, QStringLiteral("repo")) == QStringLiteral("ok"));
        robotBranch_ = kv(stdOut_, QStringLiteral("branch"));
        robotHead_ = kv(stdOut_, QStringLiteral("head"));
        robotDirty_ = !kv(stdOut_, QStringLiteral("dirty")).isEmpty();
        robotRecvCfg_ = kv(stdOut_, QStringLiteral("recv"));
        robotHelpersOk_ = (kv(stdOut_, QStringLiteral("helpers")) == QStringLiteral("ok"));

        if (mode_ == Mode::Check) { reportCheck(); return; }
        reconcileRobot();
        return;
    }

    case Stage::Push:
        if (!ok) {
            // Distinguish "could not reach the robot" (deferrable) from "the
            // robot refused the push" (a real problem the operator must see).
            const QString all = stdOut_ + QLatin1Char('\n') + stdErr_;
            // Ask the ping monitor rather than pattern-matching git's stderr.
            // Matching text is fragile — git says "[remote rejected]", not
            // "[rejected]", when updateInstead declines a dirty worktree, and
            // guessing wrong here reports a refused push as "robot offline".
            // The robot was reachable moments ago (we just probed it over
            // ssh), so unless ping now says otherwise this is a real refusal.
            if (reach_ == Reachability::Offline) {
                finishRobotDeferred(QStringLiteral("robot went offline during push"));
            } else {
                finish(LevelBad, QStringLiteral("Robot rejected the push"),
                       QStringLiteral("The robot refused '%1'. Nothing was forced.\n%2")
                           .arg(branch_, tail(all, 10)));
            }
            return;
        }
        if (robotNeedsSwitch_) {
            startStage(Stage::RobotSwitch);
        } else {
            startStage(Stage::RobotBuild);
        }
        return;

    case Stage::RobotSwitch:
        if (!ok) {
            finish(LevelBad, QStringLiteral("Robot switch failed"),
                   QStringLiteral("The robot did not switch to '%1'.\n%2")
                       .arg(branch_, tail(stdOut_ + stdErr_, 12)));
            return;
        }
        // Checkout only. RobotBuild runs rebuild_affected.sh against
        // robotHead_ (pre-switch) → laptopHeadAfter_; that script owns
        // scoped vs full from the robot's range.
        startStage(Stage::RobotBuild);
        return;

    case Stage::RobotBuild:
        if (!ok) {
            finish(LevelBad, QStringLiteral("Robot build failed"),
                   QStringLiteral("The robot is on '%1' but did not build.\n%2")
                       .arg(branch_, tail(stdOut_ + stdErr_, 12)));
            return;
        }
        startStage(Stage::RobotVerify);
        return;

    case Stage::RobotVerify: {
        if (!ok) {
            finish(LevelBad, QStringLiteral("Robot verify failed"),
                   QStringLiteral("Could not read the robot's state back after syncing.\n%1")
                       .arg(detail));
            return;
        }
        const QString vb = kv(stdOut_, QStringLiteral("branch"));
        const QString vh = kv(stdOut_, QStringLiteral("head"));
        robotBranch_ = vb;
        robotHead_ = vh;
        if (vb != branch_ || vh != laptopHeadAfter_) {
            // This is the case that used to be reported as success: the ref
            // moved but the worktree did not, or the switch silently landed
            // somewhere else.
            finish(LevelBad, QStringLiteral("Robot out of sync"),
                   QStringLiteral("Expected %1 @ %2 but the robot is on %3 @ %4.")
                       .arg(branch_, shortSha(laptopHeadAfter_),
                            vb.isEmpty() ? QStringLiteral("(detached)") : vb, shortSha(vh)));
            return;
        }
        localSha_ = laptopHeadAfter_;
        behind_ = 0;
        finish(ahead_ > 0 ? LevelWarn : LevelOk,
               ahead_ > 0 ? QStringLiteral("In sync · %1 unpushed").arg(ahead_)
                          : QStringLiteral("In sync · %1").arg(branch_),
               QStringLiteral("Laptop and robot are both on %1 @ %2.%3\n"
                              "Restart rdata-offload on the robot if its scripts changed.")
                   .arg(branch_, shortSha(laptopHeadAfter_),
                        ahead_ > 0 ? QStringLiteral("\n%1 commit(s) are not on origin yet.")
                                         .arg(ahead_)
                                   : QString()));
        return;
    }

    case Stage::Prepare:
        if (!ok) {
            finish(LevelBad, QStringLiteral("Robot prepare failed"),
                   QStringLiteral("install_deploy_helpers.sh did not succeed.\n%1")
                       .arg(tail(stdOut_ + stdErr_, 12)));
            return;
        }
        finish(LevelOk, QStringLiteral("Robot ready for sync"),
               QStringLiteral("Helpers installed on %1@%2 and "
                              "receive.denyCurrentBranch=updateInstead.")
                   .arg(robotUser_, robotHost_));
        return;

    case Stage::Idle:
        return;
    }
}

// ---------------------------------------------------------------------------
// Robot reconciliation — one path, shared by sync and switch
// ---------------------------------------------------------------------------

void RepoSyncManager::beginRobotPhase() {
    // The OCU already pings the robot every 3s. If that monitor says the robot
    // is down there is nothing to gain from an ssh probe that will just sit in
    // its connect timeout — skip straight to the deferred/offline outcome.
    if (reach_ == Reachability::Offline) {
        robotOnline_ = false;
        if (mode_ == Mode::Check) { reportCheck(); return; }
        finishRobotDeferred(QStringLiteral("%1 is not responding to ping").arg(robotHost_));
        return;
    }
    startStage(Stage::RobotState);
}

void RepoSyncManager::reconcileRobot() {
    if (!robotRepoOk_) {
        finish(LevelBad, QStringLiteral("Robot repo missing"),
               QStringLiteral("/home/%1/pilot_ws does not exist on %2.")
                   .arg(robotUser_, robotHost_));
        return;
    }
    if (!robotHelpersOk_) {
        finish(LevelBad, QStringLiteral("Robot helpers missing"),
               QStringLiteral("~/pilot_deploy helpers are not installed on %1. "
                              "Run scripts/deploy/install_deploy_helpers.sh %2@%1.")
                   .arg(robotHost_, robotUser_));
        return;
    }
    if (robotRecvCfg_ != QStringLiteral("updateInstead")) {
        // Without this the robot either refuses the push, or (worse, with
        // 'warn'/'ignore') accepts the ref while leaving a stale worktree that
        // would then be built and flown.
        finish(LevelBad, QStringLiteral("Robot repo misconfigured"),
               QStringLiteral("receive.denyCurrentBranch is '%1' on %2, expected "
                              "'updateInstead'. Run "
                              "scripts/deploy/install_deploy_helpers.sh %3@%2.")
                   .arg(robotRecvCfg_.isEmpty() ? QStringLiteral("(unset)") : robotRecvCfg_,
                        robotHost_, robotUser_));
        return;
    }
    if (robotDirty_) {
        finish(LevelBad, QStringLiteral("Robot has local changes"),
               QStringLiteral("The robot's worktree has uncommitted changes. "
                              "They will not be discarded — clear them on the robot "
                              "and sync again."));
        return;
    }

    robotNeedsSwitch_ = (robotBranch_ != branch_);

    if (!robotNeedsSwitch_ && robotHead_ == laptopHeadAfter_) {
        finish(LevelOk, QStringLiteral("Up to date · %1").arg(branch_),
               QStringLiteral("Laptop and robot are both on %1 @ %2.")
                   .arg(branch_, shortSha(laptopHeadAfter_)));
        return;
    }
    startStage(Stage::Push);
}

void RepoSyncManager::finishRobotDeferred(const QString& why) {
    // The laptop half succeeded; the robot half is simply postponed. This is a
    // warning, not a failure — the operator can keep working.
    finish(LevelWarn, QStringLiteral("Laptop synced · robot offline"),
           QStringLiteral("Laptop is on %1 @ %2. Robot deferred (%3); it will be "
                          "reconciled on the next sync while connected.")
               .arg(branch_, shortSha(laptopHeadAfter_), why));
}

// ---------------------------------------------------------------------------
// Read-only report
// ---------------------------------------------------------------------------

void RepoSyncManager::reportCheck() {
    QStringList detail;
    const bool on_deploy = (laptopBranch_ == branch_);
    detail << QStringLiteral("Laptop: %1 @ %2%3")
                  .arg(laptopBranch_, shortSha(laptopHeadBefore_),
                       laptopDirty_ ? QStringLiteral(" (uncommitted changes)") : QString());
    if (!on_deploy) {
        detail << QStringLiteral("Deploy branch: %1 (laptop will switch on Sync)")
                      .arg(branch_);
    }

    if (originOffline_) {
        detail << QStringLiteral("Origin: unreachable");
    } else if (originSha_.isEmpty()) {
        detail << QStringLiteral("Origin: no branch '%1'").arg(branch_);
    } else {
        detail << QStringLiteral("Origin/%1: %2 behind / %3 ahead")
                      .arg(branch_).arg(behind_).arg(ahead_);
    }

    if (!robotOnline_) {
        detail << QStringLiteral("Robot: offline (%1)").arg(robotHost_);
    } else if (!robotRepoOk_) {
        detail << QStringLiteral("Robot: ~/pilot_ws missing");
    } else {
        detail << QStringLiteral("Robot: %1 @ %2%3")
                      .arg(robotBranch_.isEmpty() ? QStringLiteral("(detached)") : robotBranch_,
                           shortSha(robotHead_),
                           robotDirty_ ? QStringLiteral(" (uncommitted changes)") : QString());
    }

    const bool originKnown = !originOffline_ && !originSha_.isEmpty();
    const QString deploySha = !localSha_.isEmpty() ? localSha_ : laptopHeadBefore_;
    const bool robotMismatch =
        robotOnline_ && robotRepoOk_ &&
        (robotBranch_ != branch_ || (!deploySha.isEmpty() && robotHead_ != deploySha));
    const bool needsPrepare =
        robotOnline_ && robotRepoOk_ &&
        (!robotHelpersOk_ || robotRecvCfg_ != QStringLiteral("updateInstead"));

    if (originKnown && behind_ > 0 && ahead_ > 0) {
        finish(LevelBad, QStringLiteral("%1 diverged from origin").arg(branch_),
               detail.join(QLatin1Char('\n')));
    } else if (!on_deploy) {
        finish(LevelWarn, QStringLiteral("Laptop on %1, deploy is %2")
                              .arg(laptopBranch_, branch_),
               detail.join(QLatin1Char('\n')));
    } else if (robotMismatch) {
        finish(LevelWarn, QStringLiteral("Robot software out of date"),
               detail.join(QLatin1Char('\n')));
    } else if (originKnown && behind_ > 0) {
        finish(LevelWarn, QStringLiteral("%1 behind origin by %2").arg(branch_).arg(behind_),
               detail.join(QLatin1Char('\n')));
    } else if (needsPrepare) {
        finish(LevelWarn, QStringLiteral("Robot not prepared for sync"),
               detail.join(QLatin1Char('\n')));
    } else if (!robotOnline_) {
        finish(LevelOk, QStringLiteral("Laptop current · robot offline"),
               detail.join(QLatin1Char('\n')));
    } else if (laptopDirty_ || robotDirty_) {
        finish(LevelWarn, QStringLiteral("In sync · uncommitted changes"),
               detail.join(QLatin1Char('\n')));
    } else {
        finish(LevelOk, QStringLiteral("In sync · %1").arg(branch_),
               detail.join(QLatin1Char('\n')));
    }
}

// ---------------------------------------------------------------------------
// Affected-package computation (mirrors the robot-side rebuild_affected.sh)
// ---------------------------------------------------------------------------

QStringList RepoSyncManager::workspacePackageNames() const {
    QStringList names;
    QDirIterator it(workspacePath() + QStringLiteral("/src"),
                    QStringList() << QStringLiteral("package.xml"),
                    QDir::Files, QDirIterator::Subdirectories);
    static const QRegularExpression nameRe(QStringLiteral("<name>([^<]+)</name>"));
    while (it.hasNext()) {
        it.next();
        QFile f(it.filePath());
        if (f.open(QIODevice::ReadOnly)) {
            const QRegularExpressionMatch m = nameRe.match(QString::fromUtf8(f.readAll()));
            if (m.hasMatch()) names << m.captured(1).trimmed();
            f.close();
        }
    }
    names.removeDuplicates();
    return names;
}

void RepoSyncManager::fillSnapshot(Level level, const QString& headline,
                                   const QString& detail) {
    snapshot_ = RepoSyncSnapshot{};
    snapshot_.deploy_branch = branch_;
    snapshot_.laptop_branch = laptopBranch_;
    snapshot_.laptop_sha = laptopHeadAfter_.isEmpty() ? laptopHeadBefore_
                                                      : laptopHeadAfter_;
    snapshot_.origin_sha = originSha_;
    snapshot_.robot_branch = robotBranch_;
    snapshot_.robot_sha = robotHead_;
    snapshot_.behind = behind_;
    snapshot_.ahead = ahead_;
    snapshot_.origin_offline = originOffline_;
    snapshot_.laptop_dirty = laptopDirty_;
    snapshot_.laptop_on_deploy = (laptopBranch_ == branch_);
    snapshot_.robot_online = robotOnline_;
    snapshot_.robot_repo_ok = robotRepoOk_;
    snapshot_.robot_helpers_ok = robotHelpersOk_;
    snapshot_.robot_recv_ok = (robotRecvCfg_ == QStringLiteral("updateInstead"));
    snapshot_.robot_dirty = robotDirty_;
    snapshot_.needs_prepare =
        robotOnline_ && robotRepoOk_ &&
        (!robotHelpersOk_ || robotRecvCfg_ != QStringLiteral("updateInstead"));
    snapshot_.headline = headline;
    snapshot_.detail = detail;

    const QString deploySha = !localSha_.isEmpty() ? localSha_ : snapshot_.laptop_sha;
    const bool robotMismatch =
        robotOnline_ && robotRepoOk_ &&
        (robotBranch_ != branch_ || (!deploySha.isEmpty() && robotHead_ != deploySha));
    const bool laptopBehind = !originOffline_ && !originSha_.isEmpty() && behind_ > 0;
    const bool diverged = laptopBehind && ahead_ > 0;
    snapshot_.offer_update =
        !diverged &&
        (!snapshot_.laptop_on_deploy || laptopBehind || robotMismatch ||
         snapshot_.needs_prepare);
    snapshot_.robot_pending =
        snapshot_.laptop_on_deploy && !laptopBehind && !robotOnline_ &&
        level != LevelBad;
    if (snapshot_.robot_pending) {
        snapshot_.offer_update = false;
    }
    Q_UNUSED(level);
}

RepoSyncManager::NameStatusParse
RepoSyncManager::parseNameStatus(const QString& text) {
    NameStatusParse out;
    const QStringList lines =
        text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& raw : lines) {
        const QStringList parts =
            raw.split(QLatin1Char('\t'), Qt::SkipEmptyParts);
        if (parts.isEmpty()) continue;
        const QChar st = parts.at(0).isEmpty() ? QChar() : parts.at(0).at(0);
        QStringList paths;
        if ((st == QLatin1Char('R') || st == QLatin1Char('C')) &&
            parts.size() >= 3) {
            paths << parts.at(1) << parts.at(2);
        } else if (parts.size() >= 2) {
            paths << parts.at(1);
        } else {
            continue;
        }
        for (const QString& p : paths) {
            if (!out.files.contains(p)) out.files << p;
        }
        if (st == QLatin1Char('A') || st == QLatin1Char('D') ||
            st == QLatin1Char('R') || st == QLatin1Char('C')) {
            for (const QString& p : paths) {
                if (isPackageXmlPath(p)) {
                    out.package_xml_added_or_removed = true;
                }
            }
        }
    }
    return out;
}

bool RepoSyncManager::needsFullWorkspaceBuild(const QStringList& changedFiles) {
    for (const QString& f : changedFiles) {
        if (isBuildAffecting(f) && !pathHasKnownPrefix(f)) return true;
    }
    return false;
}

QStringList RepoSyncManager::packagesForChangedFiles(const QStringList& changedFiles) {
    QStringList toBuild;
    for (const Mapping& m : packageMappings()) {
        bool touched = false;
        for (const QString& f : changedFiles) {
            if (f.startsWith(m.prefix) && isBuildAffecting(f)) {
                touched = true;
                break;
            }
        }
        if (touched) toBuild << m.pkgs;
    }
    toBuild.removeDuplicates();
    return toBuild;
}

QStringList RepoSyncManager::affectedPackages(const QStringList& changedFiles) const {
    QStringList toBuild = packagesForChangedFiles(changedFiles);
    const QStringList present = workspacePackageNames();
    QStringList result;
    for (const QString& p : toBuild) {
        if (present.contains(p)) result << p;
    }
    return result;
}

}  // namespace f2c_cpp
