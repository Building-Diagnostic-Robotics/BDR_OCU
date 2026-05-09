/**
 * @file main.cpp
 * @brief Entry point for F2C Coverage Planner GUI
 * 
 * C++ version of the Fields2Cover coverage planner GUI.
 * Equivalent to f2c_gui.py but with native performance.
 */

#include <QApplication>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QIcon>
#include <QObject>
#include <QProcess>
#include <QSettings>
#include <QStringList>
#include <QStyleFactory>
#include <QThread>
#include <QTimer>
#include <cstdlib>
#include <iostream>
#include <string>
#include <rclcpp/rclcpp.hpp>

#include "app_shell.hpp"
#include "settings_constants.hpp"
#include "update/update_lockfile.hpp"
#include "update/update_log.hpp"
#include "update/update_settings.hpp"
#include "update/update_state.hpp"
#include "version_info.hpp"

namespace {

// Phase 9 watchdog deadline: how long after construction we wait for the
// `bootHealthy` signal before declaring the new install unhealthy and
// triggering rollback. 60 s = comfortable middle ground (locked Q2=B).
constexpr int kHealthProbeTimeoutMs = 60 * 1000;

// Synchronous timeout for `sudo bdr-apply-update recover` invoked at
// startup when we find a stale dpkg_running marker. dpkg --configure -a
// on a single package is bounded by postinst runtime; 120 s is generous.
constexpr int kRecoverTimeoutMs = 120 * 1000;

// Run `sudo -n /usr/bin/bdr-apply-update recover` synchronously and
// return its exit code (or -1 on QProcess failure / timeout). Best-effort
// — Phase 9 does not block startup on recover failure; the operator can
// still attempt to use the OCU and re-trigger the OTA path next time.
int runRecoverSync() {
    f2c_cpp::update::log::info(
        "main", QStringLiteral("running bdr-apply-update recover (sync)"));
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(QStringLiteral("sudo"),
            QStringList()
                << QStringLiteral("-n")
                << QStringLiteral("/usr/bin/bdr-apply-update")
                << QStringLiteral("recover"));
    if (!p.waitForFinished(kRecoverTimeoutMs)) {
        f2c_cpp::update::log::error(
            "main", QStringLiteral("recover timed out after %1 s")
                        .arg(kRecoverTimeoutMs / 1000));
        if (p.state() != QProcess::NotRunning) p.kill();
        return -1;
    }
    const int rc = p.exitCode();
    f2c_cpp::update::log::info(
        "main", QStringLiteral("recover finished rc=%1").arg(rc));
    return rc;
}

// Resolve the bdr-update-runner binary path. Mirrors the OCU's existing
// resolution order in handoffToUpdateRunner: dev build sibling first,
// then the .deb-installed location.
QString resolveRunnerPath() {
    const QString sibling =
        QCoreApplication::applicationDirPath()
        + QStringLiteral("/bdr-update-runner");
    if (QFileInfo::exists(sibling)) return sibling;
    const QString system = QStringLiteral("/usr/bin/bdr-update-runner");
    if (QFileInfo::exists(system)) return system;
    return {};
}

// Spawn the runner in --rollback mode and spin-wait for it to acquire
// the lockfile. Returns true on successful handoff (caller MUST exit
// without running app.exec()); false if we couldn't hand off (caller
// should fall back to "show banner and continue" path).
bool handoffToRollbackRunner(const QString& prev_deb,
                             bool dark_mode) {
    if (prev_deb.isEmpty() || !QFileInfo::exists(prev_deb)) {
        f2c_cpp::update::log::warn(
            "main",
            QStringLiteral("handoffToRollbackRunner: prev_deb missing or "
                           "empty: '%1' — cannot hand off")
                .arg(prev_deb));
        return false;
    }
    const QString runner_path = resolveRunnerPath();
    if (runner_path.isEmpty()) {
        f2c_cpp::update::log::error(
            "main",
            QStringLiteral("handoffToRollbackRunner: bdr-update-runner not "
                           "found in app dir or /usr/bin"));
        return false;
    }

    QStringList runner_args;
    runner_args << QStringLiteral("--ocu-binary")
                << QCoreApplication::applicationFilePath();
    runner_args << QStringLiteral("--rollback") << prev_deb;
    if (dark_mode) runner_args << QStringLiteral("--dark");

    f2c_cpp::update::log::info(
        "main",
        QStringLiteral("spawning rollback runner: %1 %2")
            .arg(runner_path).arg(runner_args.join(QLatin1Char(' '))));

    if (!QProcess::startDetached(runner_path, runner_args)) {
        f2c_cpp::update::log::error(
            "main",
            QStringLiteral("startDetached failed for rollback runner"));
        return false;
    }

    // Spin-wait up to 2 s for the runner to acquire the lockfile. The
    // event loop hasn't started yet, so we can't use QTimer — direct
    // sleep is fine because we're about to exit anyway.
    constexpr int kPollMs = 50;
    constexpr int kMaxWaitMs = 2000;
    for (int waited = 0; waited < kMaxWaitMs; waited += kPollMs) {
        if (f2c_cpp::update::isRunnerLockfileHeld()) {
            f2c_cpp::update::log::info(
                "main",
                QStringLiteral("rollback runner acquired lockfile after %1 ms")
                    .arg(waited));
            return true;
        }
        QThread::msleep(kPollMs);
    }
    f2c_cpp::update::log::error(
        "main",
        QStringLiteral("rollback runner failed to acquire lockfile within "
                       "%1 ms — falling back to banner-only path")
            .arg(kMaxWaitMs));
    return false;
}

enum class StartupAction {
    Normal,                      ///< no marker, or marker == Done
    NormalWithProbe,             ///< marker InstalledPendingProbe → run watchdog
    NormalWithBanner,            ///< marker RolledBack → show advisory banner
    HandoffToRollbackRunner,     ///< marker ProbingHealth → previous launch crashed
};

struct StartupContext {
    StartupAction action = StartupAction::Normal;
    f2c_cpp::update::UpdateStateData marker;  ///< raw marker for context
};

// Read the marker file and determine what the OCU should do at startup.
// Side effects:
//   - DpkgRunning: invokes recover synchronously, clears marker.
//   - InstalledPendingProbe: rewrites marker to ProbingHealth so the
//     NEXT launch sees ProbingHealth and treats it as "previous launch
//     crashed mid-probe" — the crash-loop detection seam.
StartupContext determineStartupAction() {
    StartupContext ctx;
    if (!f2c_cpp::update::readUpdateState(&ctx.marker)) {
        // No marker (or unreadable). Normal startup.
        return ctx;
    }

    using f2c_cpp::update::UpdateStage;
    switch (ctx.marker.stage) {
        case UpdateStage::DpkgRunning: {
            f2c_cpp::update::log::warn(
                "main",
                QStringLiteral("startup: dpkg_running marker found — running "
                               "recover before continuing"));
            runRecoverSync();
            f2c_cpp::update::clearUpdateState();
            ctx.action = StartupAction::Normal;
            return ctx;
        }

        case UpdateStage::InstalledPendingProbe: {
            // First launch after a successful install. Rewrite to
            // ProbingHealth — so that if THIS launch crashes before the
            // 60 s probe completes, the next launch sees ProbingHealth
            // (not InstalledPendingProbe) and triggers rollback.
            f2c_cpp::update::UpdateStateData next = ctx.marker;
            next.stage = UpdateStage::ProbingHealth;
            f2c_cpp::update::writeUpdateState(next);
            f2c_cpp::update::log::info(
                "main",
                QStringLiteral("startup: installed_pending_probe → "
                               "probing_health (watchdog will run)"));
            ctx.marker = next;
            ctx.action = StartupAction::NormalWithProbe;
            return ctx;
        }

        case UpdateStage::ProbingHealth: {
            f2c_cpp::update::log::error(
                "main",
                QStringLiteral("startup: probing_health marker on a fresh "
                               "launch — previous probe never completed, "
                               "rolling back to %1")
                    .arg(ctx.marker.previousDebPath.isEmpty()
                             ? QStringLiteral("(no previous .deb)")
                             : ctx.marker.previousDebPath));
            ctx.action = StartupAction::HandoffToRollbackRunner;
            return ctx;
        }

        case UpdateStage::RolledBack: {
            f2c_cpp::update::log::info(
                "main",
                QStringLiteral("startup: rolled_back marker — showing "
                               "advisory banner"));
            ctx.action = StartupAction::NormalWithBanner;
            return ctx;
        }

        case UpdateStage::Done:
        case UpdateStage::None:
        case UpdateStage::Downloading:
        default:
            return ctx;
    }
}

}  // namespace

int main(int argc, char* argv[])
{
    // Keep app ROS graph behavior consistent with manual teleop launch:
    // an inherited custom Cyclone profile can break zenoh ros2dds discovery.
    if (const char* cdds_uri = std::getenv("CYCLONEDDS_URI")) {
        const std::string uri(cdds_uri);
        if (uri.find("rf_cyclonedds.xml") != std::string::npos) {
            std::cout << "[BDR] Clearing CYCLONEDDS_URI for app ROS context: "
                      << uri << std::endl;
            unsetenv("CYCLONEDDS_URI");
        }
    }

    rclcpp::init(argc, argv);

    // Create Qt application
    QApplication app(argc, argv);
    
    // Set application metadata
    app.setApplicationName("BDR Coverage Planner");
    app.setApplicationDisplayName("BDR Coverage Planner");
    app.setApplicationVersion(QString::fromLatin1(f2c_cpp::version::kAppSemver));
    app.setOrganizationName("PilotControl");

    // OTA log: tag this process so the merged update.log is debuggable.
    // Cross-process safety relies on each binary calling setProcessTag()
    // before any info/warn/error (concern #1, locked Phase 7).
    f2c_cpp::update::log::setProcessTag("ocu");

    // Set application/window icon (embedded via Qt resources)
    app.setWindowIcon(QIcon(":/assets/bdr_logo.png"));
    
    // Use Fusion style for modern look
    app.setStyle(QStyleFactory::create("Fusion"));
    
    // Set default font
    QFont font("Segoe UI", 10);
    QFontDatabase fontDb;
    if (fontDb.families().filter("Segoe UI").isEmpty()) {
        font = QFont("Sans Serif", 10);
    }
    app.setFont(font);
    
    // Print startup info
    std::cout << "=== BDR Coverage Planner (C++) ===" << std::endl;
    std::cout << "Version: " << f2c_cpp::version::kAppSemver
              << " (" << f2c_cpp::version::kAppCommitSha << ")"
              << " built " << f2c_cpp::version::kAppBuildDate << std::endl;
    std::cout << "Qt Version: " << QT_VERSION_STR << std::endl;
#ifdef HAVE_FIELDS2COVER
    std::cout << "Fields2Cover: Available" << std::endl;
#else
    std::cout << "Fields2Cover: NOT FOUND (coverage features disabled)" << std::endl;
#endif
#ifdef HAVE_CGAL
    std::cout << "CGAL: Available (alphashape enabled)" << std::endl;
#else
    std::cout << "CGAL: NOT FOUND (using convex hull fallback)" << std::endl;
#endif
    std::cout << "==================================" << std::endl;

    // Phase 9 startup-time marker dispatch. Runs BEFORE AppShellWindow
    // construction so that:
    //   - DpkgRunning: we get a synchronous `recover` before the OCU
    //     touches anything that the half-installed package owns.
    //   - ProbingHealth: we hand off to the rollback runner without
    //     ever showing a UI; the runner takes over the screen.
    StartupContext startup = determineStartupAction();
    if (startup.action == StartupAction::HandoffToRollbackRunner) {
        // Record the bad SHA in the rollback denylist BEFORE handing off.
        // The handoff exec's us out of the process, so any work after this
        // point may not run. QSettings::sync() in addToDenylist guarantees
        // the entry is on disk before exec.
        const QString bad_sha = f2c_cpp::update::shortShaFromDebPath(
            startup.marker.currentDebPath);
        if (!bad_sha.isEmpty()) {
            f2c_cpp::update::addToDenylist(bad_sha);
            f2c_cpp::update::log::warn(
                "main",
                QStringLiteral("crash-loop rollback: denylisting %1")
                    .arg(bad_sha));
        } else {
            f2c_cpp::update::log::warn(
                "main",
                QStringLiteral("crash-loop rollback: could not parse SHA "
                               "from current_deb_path '%1' — skipping "
                               "denylist add (rollback will still proceed)")
                    .arg(startup.marker.currentDebPath));
        }

        QSettings settings(QString::fromLatin1(f2c_cpp::kSettingsOrgName),
                           QString::fromLatin1(f2c_cpp::kSettingsAppName));
        const bool dark_mode = settings.value("ui/dark_mode", false).toBool();
        if (handoffToRollbackRunner(startup.marker.previousDebPath,
                                    dark_mode)) {
            // Runner is alive and holding the lockfile. Quit OCU; the
            // runner will execv us back when rollback finishes.
            f2c_cpp::update::log::info(
                "main",
                QStringLiteral("handoff successful, OCU exiting"));
            rclcpp::shutdown();
            return 0;
        }
        // Handoff failed (no prev.deb, runner missing, or didn't acquire
        // lock). Fall through to "show banner" path so the operator at
        // least sees that something went wrong on the previous launch.
        f2c_cpp::update::UpdateStateData rb;
        rb.stage = f2c_cpp::update::UpdateStage::RolledBack;
        rb.currentDebPath.clear();
        rb.previousDebPath = startup.marker.previousDebPath;
        f2c_cpp::update::writeUpdateState(rb);
        startup.action = StartupAction::NormalWithBanner;
    }

    // Create and show app shell (stage router)
    f2c_cpp::AppShellWindow shell;

    // Phase 9 watchdog wiring. We only attach the timer + Done-marker
    // signal in NormalWithProbe — the legitimate first-launch-after-
    // install case. Crash-loop launches (NormalWithBanner) and clean
    // launches (Normal) skip this entirely.
    if (startup.action == StartupAction::NormalWithProbe) {
        auto* probe_timer = new QTimer(&shell);
        probe_timer->setSingleShot(true);
        probe_timer->setInterval(kHealthProbeTimeoutMs);

        const auto current_deb = startup.marker.currentDebPath;
        const auto previous_deb = startup.marker.previousDebPath;

        // Healthy path: bootHealthy fires before the deadline. Stop the
        // timer and write the Done marker so the NEXT launch starts
        // clean (no banner, no probe).
        QObject::connect(
            &shell, &f2c_cpp::AppShellWindow::bootHealthy,
            probe_timer, &QTimer::stop);
        QObject::connect(
            &shell, &f2c_cpp::AppShellWindow::bootHealthy,
            [current_deb, previous_deb]() {
                f2c_cpp::update::UpdateStateData done;
                done.stage = f2c_cpp::update::UpdateStage::Done;
                done.currentDebPath = current_deb;
                done.previousDebPath = previous_deb;
                f2c_cpp::update::writeUpdateState(done);
                f2c_cpp::update::log::info(
                    "main",
                    QStringLiteral("watchdog: bootHealthy received → done"));
            });

        // Unhealthy path: timer fires before bootHealthy. Hand off to
        // the rollback runner. We're inside the event loop now, so the
        // handoff polls and quit() works.
        QObject::connect(
            probe_timer, &QTimer::timeout,
            [current_deb, previous_deb]() {
                f2c_cpp::update::log::error(
                    "main",
                    QStringLiteral("watchdog: 60 s deadline expired without "
                                   "bootHealthy — rolling back"));

                // Denylist the bad SHA before handoff (mirrors the
                // crash-loop seam in determineStartupAction).
                const QString bad_sha =
                    f2c_cpp::update::shortShaFromDebPath(current_deb);
                if (!bad_sha.isEmpty()) {
                    f2c_cpp::update::addToDenylist(bad_sha);
                    f2c_cpp::update::log::warn(
                        "main",
                        QStringLiteral("watchdog rollback: denylisting %1")
                            .arg(bad_sha));
                }

                QSettings settings(QString::fromLatin1(f2c_cpp::kSettingsOrgName),
                                   QString::fromLatin1(f2c_cpp::kSettingsAppName));
                const bool dark_mode = settings.value("ui/dark_mode", false).toBool();
                if (handoffToRollbackRunner(previous_deb, dark_mode)) {
                    qApp->quit();
                } else {
                    // No prev.deb / runner missing. Best we can do is
                    // log and let the OCU keep running on the broken
                    // version; operator can manually re-install. The
                    // denylist add above still stands, so future polls
                    // won't re-offer this SHA.
                    f2c_cpp::update::log::error(
                        "main",
                        QStringLiteral("watchdog: rollback handoff FAILED — "
                                       "OCU continuing on current install"));
                }
            });
        probe_timer->start();
    }

    if (startup.action == StartupAction::NormalWithBanner) {
        shell.showRolledBackBanner();
    }

    shell.show();

    int exit_code = app.exec();

    rclcpp::shutdown();
    return exit_code;
}

