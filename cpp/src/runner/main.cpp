/**
 * @file runner/main.cpp
 * @brief Entry point for bdr-update-runner — the external OTA installer.
 *
 * Lifecycle (locked Phase 7 spec):
 *   1. OCU spawns this binary detached via QProcess::startDetached, then
 *      polls for the runner lockfile to be held; once held, OCU quits.
 *   2. main() acquires the lockfile (refuses to start if another runner
 *      already holds it — concern #2).
 *   3. main() parses argv into a RunnerArgs struct (locked Q1=A: CLI args).
 *   4. main() opens the UpdateRunnerWindow, which drives the download +
 *      verify pipeline.
 *   5. On Download Complete the window pauses 2 s and ::execv-s the OCU
 *      back; on terminal failure it stays open with Try Again + Cancel.
 *
 * Phase 8 will add the dpkg invocation between SHA-verify and execv.
 */

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <iostream>

#include "runner/update_runner_window.hpp"
#include "update/update_lockfile.hpp"
#include "update/update_log.hpp"

namespace {

f2c_cpp::RunnerArgs parseArgs(const QCommandLineParser& parser) {
    f2c_cpp::RunnerArgs out;
    out.debUrl       = parser.value(QStringLiteral("deb-url"));
    out.sha256Url    = parser.value(QStringLiteral("sha256-url"));
    out.assetName    = parser.value(QStringLiteral("asset-name"));
    out.tag          = parser.value(QStringLiteral("tag"));
    out.commitSha    = parser.value(QStringLiteral("commit-sha"));
    out.ocuBinaryPath = parser.value(QStringLiteral("ocu-binary"));

    bool size_ok = false;
    const qint64 size_val =
        parser.value(QStringLiteral("size-bytes")).toLongLong(&size_ok);
    out.sizeBytes = size_ok ? size_val : 0;

    out.darkMode = parser.isSet(QStringLiteral("dark"));

    // Phase 9 rollback handoff. If --rollback is provided, the runner
    // skips download/SHA stages and runs dpkg -i on the supplied .deb.
    out.rollbackPrevDeb = parser.value(QStringLiteral("rollback"));
    out.rollback = !out.rollbackPrevDeb.isEmpty();

    return out;
}

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    // Match the OCU's QApplication identity so QStandardPaths resolves
    // to the same on-disk paths (cache dir for update.log,
    // update_state.json, downloaded .deb). The OCU has historically used
    // setApplicationName("BDR Coverage Planner") (with spaces) — the
    // value here MUST be character-for-character identical, otherwise
    // the runner's marker writes land in a different directory than the
    // OCU's reads, silently breaking the Phase 7-9 handoff.
    //
    // QSettings is a separate concern: code that uses QSettings passes
    // kSettingsOrgName/kSettingsAppName explicitly, so settings storage
    // is unaffected by the QApplication-name choice here.
    QCoreApplication::setOrganizationName(QStringLiteral("PilotControl"));
    QCoreApplication::setApplicationName(QStringLiteral("BDR Coverage Planner"));
    QApplication::setApplicationDisplayName(
        QStringLiteral("BDR Coverage Planner — Update"));

    f2c_cpp::update::log::setProcessTag("runner");
    f2c_cpp::update::log::info(
        "runner",
        QStringLiteral("bdr-update-runner started, pid=%1")
            .arg(QCoreApplication::applicationPid()));

    // CLI parsing (locked Q1=A: argv-based handoff).
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("BDR Coverage Planner OTA installer"));
    parser.addHelpOption();
    parser.addOptions({
        {QStringLiteral("deb-url"),
         QStringLiteral("Direct .deb download URL"), QStringLiteral("url")},
        {QStringLiteral("sha256-url"),
         QStringLiteral("URL of the .sha256 sidecar"), QStringLiteral("url")},
        {QStringLiteral("asset-name"),
         QStringLiteral("On-disk filename for the cached .deb"),
         QStringLiteral("name")},
        {QStringLiteral("tag"),
         QStringLiteral("Release tag (display only)"),
         QStringLiteral("tag")},
        {QStringLiteral("commit-sha"),
         QStringLiteral("Release target commit SHA"),
         QStringLiteral("sha")},
        {QStringLiteral("size-bytes"),
         QStringLiteral("Expected .deb size in bytes (0 if unknown)"),
         QStringLiteral("bytes"), QStringLiteral("0")},
        {QStringLiteral("ocu-binary"),
         QStringLiteral("Absolute path to the OCU binary to execv on completion"),
         QStringLiteral("path")},
        {QStringLiteral("dark"),
         QStringLiteral("Render in dark mode (matches operator's OCU theme)")},
        {QStringLiteral("rollback"),
         QStringLiteral("Phase 9 rollback mode: skip download, run dpkg -i "
                        "on the given previous .deb path"),
         QStringLiteral("path")},
    });
    parser.process(app);

    f2c_cpp::RunnerArgs args = parseArgs(parser);
    if (!args.isValid()) {
        f2c_cpp::update::log::error(
            "runner",
            args.rollback
                ? QStringLiteral("missing required CLI args (rollback mode "
                                 "needs --rollback and --ocu-binary)")
                : QStringLiteral("missing required CLI args (deb-url, "
                                 "sha256-url, asset-name, ocu-binary)"));
        std::cerr << "bdr-update-runner: missing required arguments. "
                     "See --help.\n";
        return 2;
    }

    // Lock acquisition (concerns #2 + #3). If another runner is already
    // running, refuse to start — DO NOT contend over dpkg.
    f2c_cpp::update::Lockfile lock;
    if (!lock.tryAcquire()) {
        f2c_cpp::update::log::warn(
            "runner",
            QStringLiteral(
                "another runner already holds %1 — refusing to start")
                .arg(f2c_cpp::update::runnerLockfilePath()));
        std::cerr << "bdr-update-runner: another updater is already running.\n";
        return 3;
    }
    f2c_cpp::update::log::info(
        "runner",
        QStringLiteral("acquired lockfile: %1").arg(lock.path()));

    f2c_cpp::UpdateRunnerWindow window(args);
    window.show();

    const int rc = app.exec();
    f2c_cpp::update::log::info(
        "runner", QStringLiteral("event loop exited rc=%1").arg(rc));
    return rc;
}
