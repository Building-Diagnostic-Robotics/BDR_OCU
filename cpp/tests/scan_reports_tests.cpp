// Scan reports: what the OCU hands scan_report.py, and the send scheduler.

#include "scan_reports.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace f2c_cpp::scan_reports;

namespace {

QCoreApplication* app() {
    static int argc = 1;
    static char name[] = "scan_reports_tests";
    static char* argv[] = {name, nullptr};
    static QCoreApplication* instance =
        QCoreApplication::instance() ? QCoreApplication::instance() : new QCoreApplication(argc, argv);
    return instance;
}

CollectRequest request() {
    CollectRequest r;
    r.log_id = QStringLiteral("mission_20261017_131000");
    r.client_id = QStringLiteral("Easton_roofing_KC");
    r.robot_id = QStringLiteral("Roofus#0002");
    r.host = QStringLiteral("192.168.168.105");
    r.logs_dir = QStringLiteral("/home/op/.local/share/PilotControl/BDRCoveragePlanner/mission_logs");
    r.building = QStringLiteral("1185 Obrien Dr");
    r.operator_name = QStringLiteral("Joe");
    r.ocu_version = QStringLiteral("2.3.0");
    return r;
}

QString valueAfter(const QStringList& args, const QString& flag) {
    const int i = args.indexOf(flag);
    return i >= 0 && i + 1 < args.size() ? args.at(i + 1) : QString();
}

bool waitFor(const std::function<bool()>& done, int ms = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return done();
}

}  // namespace

TEST(ScanReports, CollectArgsCarryTheMissionAndRobot) {
    const QStringList args = collectArgs(QStringLiteral("/x/scan_report.py"), QStringLiteral("/o"), request());
    EXPECT_EQ(args.at(0), QStringLiteral("/x/scan_report.py"));
    EXPECT_EQ(args.at(1), QStringLiteral("collect"));
    EXPECT_EQ(valueAfter(args, "--outbox"), QStringLiteral("/o"));
    EXPECT_EQ(valueAfter(args, "--log-id"), QStringLiteral("mission_20261017_131000"));
    EXPECT_EQ(valueAfter(args, "--client-id"), QStringLiteral("Easton_roofing_KC"));
    EXPECT_EQ(valueAfter(args, "--robot-id"), QStringLiteral("Roofus#0002"));
    EXPECT_EQ(valueAfter(args, "--host"), QStringLiteral("192.168.168.105"));
    EXPECT_EQ(valueAfter(args, "--user"), QStringLiteral("roofus"));
    EXPECT_EQ(valueAfter(args, "--data-root"), QStringLiteral("/R_DATA"));
    EXPECT_EQ(valueAfter(args, "--building"), QStringLiteral("1185 Obrien Dr"));
    EXPECT_EQ(valueAfter(args, "--ocu-version"), QStringLiteral("2.3.0"));
}

TEST(ScanReports, CollectNeedsALogIdAClientAndAHost) {
    EXPECT_TRUE(collectBlocker(request()).isEmpty());
    CollectRequest r = request();
    r.client_id.clear();
    EXPECT_TRUE(collectBlocker(r).contains(QStringLiteral("cloud_client_id")));
    r = request();
    r.log_id = QStringLiteral("  ");
    EXPECT_FALSE(collectBlocker(r).isEmpty());
    r = request();
    r.host.clear();
    EXPECT_FALSE(collectBlocker(r).isEmpty());
}

TEST(ScanReports, SendSkipsTheRobotWhileAScanIsUp) {
    const QStringList idle = sendArgs(QStringLiteral("s.py"), QStringLiteral("/o"), QStringLiteral("Roofus#0002"),
                                      QStringLiteral("2.3.0"), false);
    EXPECT_EQ(idle.at(1), QStringLiteral("send"));
    EXPECT_EQ(valueAfter(idle, "--robot-id"), QStringLiteral("Roofus#0002"));
    EXPECT_FALSE(idle.contains(QStringLiteral("--no-fetch")));
    const QStringList busy = sendArgs(QStringLiteral("s.py"), QStringLiteral("/o"), QStringLiteral("Roofus#0002"),
                                      QStringLiteral("2.3.0"), true);
    EXPECT_TRUE(busy.contains(QStringLiteral("--no-fetch")));
}

TEST(ScanReports, SendEnvironmentIsWhatUploaderReads) {
    QProcessEnvironment base;
    base.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin"));
    const QProcessEnvironment env = sendEnvironment(base, QStringLiteral(" https://api "), QStringLiteral("c"),
                                                    QStringLiteral("t"));
    EXPECT_EQ(env.value(QStringLiteral("PATH")), QStringLiteral("/usr/bin"));
    EXPECT_EQ(env.value(QStringLiteral("BDR_CLOUD_API_BASE")), QStringLiteral("https://api"));
    EXPECT_EQ(env.value(QStringLiteral("BDR_CLOUD_CLIENT_ID")), QStringLiteral("c"));
    EXPECT_EQ(env.value(QStringLiteral("BDR_CLOUD_DEVICE_TOKEN")), QStringLiteral("t"));
    // An empty base keeps uploader.py's own default endpoint.
    EXPECT_FALSE(sendEnvironment(base, QString(), QStringLiteral("c"), QStringLiteral("t"))
                     .contains(QStringLiteral("BDR_CLOUD_API_BASE")));
}

TEST(ScanReports, SchedulerNeverOverlapsRunsAndReportsTheExitCode) {
    app();
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString marker = dir.filePath(QStringLiteral("runs"));
    int provided = 0;
    ScanReportScheduler scheduler([&]() {
        ++provided;
        SendRun run;
        run.valid = true;
        run.program = QStringLiteral("/bin/sh");
        // Slow enough that the second runNow() lands while it is still going.
        run.args = QStringList{QStringLiteral("-c"),
                               QStringLiteral("echo x >> '%1'; sleep 0.5; echo done; exit 3").arg(marker)};
        run.env = QProcessEnvironment::systemEnvironment();
        return run;
    });
    QList<int> codes;
    QStringList summaries;
    QObject::connect(&scheduler, &ScanReportScheduler::runFinished, [&](int code, const QString& summary) {
        codes << code;
        summaries << summary;
    });
    scheduler.runNow();
    ASSERT_TRUE(waitFor([&] { return scheduler.running(); }));
    scheduler.runNow();  // ignored: one run at a time
    ASSERT_TRUE(waitFor([&] { return !codes.isEmpty(); }));
    EXPECT_EQ(provided, 1);
    EXPECT_EQ(codes.first(), 3);
    EXPECT_TRUE(summaries.first().contains(QStringLiteral("done")));
    QFile runs(marker);
    ASSERT_TRUE(runs.open(QIODevice::ReadOnly));
    EXPECT_EQ(runs.readAll().count('x'), 1);
}

TEST(ScanReports, SchedulerReportsWhyThereIsNoRun) {
    app();
    ScanReportScheduler scheduler([]() {
        SendRun run;
        run.reason = QStringLiteral("No robot selected.");
        return run;
    });
    QString reason;
    QObject::connect(&scheduler, &ScanReportScheduler::runFinished,
                     [&](int, const QString& summary) { reason = summary; });
    scheduler.runNow();
    EXPECT_FALSE(scheduler.running());
    EXPECT_EQ(reason, QStringLiteral("No robot selected."));
}
