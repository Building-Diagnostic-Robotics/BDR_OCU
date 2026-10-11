/**
 * @file robot_registry_tests.cpp
 * @brief Which robot the Get started screen selects: `paired_robot_id`
 *        when it names a listed robot, the only robot otherwise, a picker
 *        when several are listed, and an error (never a guess) when the
 *        pairing names a robot the file does not list.
 */

#include "robot_registry.hpp"

#include <gtest/gtest.h>

#include <QFile>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>

using f2c_cpp::CaptureIdentityDecision;
using f2c_cpp::PreLaunchIdentityDecision;
using f2c_cpp::ResolvedRobotSshTarget;
using f2c_cpp::RetiredGeneralRobotKeys;
using f2c_cpp::RobotIdentityVerdict;
using f2c_cpp::RobotRegistry;
using f2c_cpp::classifyRobotIdentity;
using f2c_cpp::decideCaptureIdentity;
using f2c_cpp::decidePreLaunchIdentity;
using f2c_cpp::resolveRobotSshTarget;
using f2c_cpp::robotIdentityMatchPrefix;
using f2c_cpp::robotIdentityMismatchText;
using f2c_cpp::robotIdentityProbePrefix;
using f2c_cpp::robotIdentityUnreadableText;
using f2c_cpp::retireLegacyGeneralRobotKeys;

namespace {

const char* const kTwoRobots = R"(
    {"robot_id": "Roofus#0002", "host": "192.168.168.102"},
    {"robot_id": "Roofus#0003", "host": "192.168.168.103"}
)";

RobotRegistry loadJson(const QTemporaryDir& dir, const QString& json) {
    const QString path = dir.filePath(QStringLiteral("robots.json"));
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(json.toUtf8());
    f.close();
    RobotRegistry registry;
    QString error;
    EXPECT_TRUE(registry.loadFromFile(path, &error)) << error.toStdString();
    return registry;
}

}  // namespace

TEST(PairedRobotId, NamedRobotThatIsListedIsSelected) {
    QTemporaryDir dir;
    const RobotRegistry registry = loadJson(
        dir, QStringLiteral(R"({"paired_robot_id": "Roofus#0003", "robots": [%1]})")
                 .arg(QLatin1String(kTwoRobots)));
    QString error;
    EXPECT_EQ(registry.pairedRobotId(&error), QStringLiteral("Roofus#0003"));
    EXPECT_TRUE(error.isEmpty());
}

TEST(PairedRobotId, NamedRobotThatIsNotListedIsAnErrorNotAGuess) {
    QTemporaryDir dir;
    const RobotRegistry registry = loadJson(
        dir, QStringLiteral(R"({"paired_robot_id": "Roofus#0009", "robots": [%1]})")
                 .arg(QLatin1String(kTwoRobots)));
    QString error;
    EXPECT_TRUE(registry.pairedRobotId(&error).isEmpty());
    EXPECT_TRUE(error.contains(QStringLiteral("Roofus#0009")));
}

TEST(PairedRobotId, PairingIsAnExactIdMatch) {
    QTemporaryDir dir;
    const RobotRegistry registry = loadJson(
        dir, QStringLiteral(R"({"paired_robot_id": "ROOFUS#0003", "robots": [%1]})")
                 .arg(QLatin1String(kTwoRobots)));
    QString error;
    EXPECT_TRUE(registry.pairedRobotId(&error).isEmpty());
    EXPECT_FALSE(error.isEmpty());
}

TEST(PairedRobotId, OnlyRobotIsSelectedWithoutAPairing) {
    QTemporaryDir dir;
    const RobotRegistry registry = loadJson(
        dir, QStringLiteral(
                 R"({"robots": [{"robot_id": "Roofus#0002", "host": "192.168.168.102"}]})"));
    QString error;
    EXPECT_EQ(registry.pairedRobotId(&error), QStringLiteral("Roofus#0002"));
    EXPECT_TRUE(error.isEmpty());
}

TEST(PairedRobotId, SeveralRobotsWithoutAPairingLeaveTheChoiceToTheOperator) {
    QTemporaryDir dir;
    const RobotRegistry registry =
        loadJson(dir, QStringLiteral(R"({"robots": [%1]})").arg(QLatin1String(kTwoRobots)));
    QString error;
    EXPECT_TRUE(registry.pairedRobotId(&error).isEmpty());
    EXPECT_TRUE(error.isEmpty());
}

TEST(PairedRobotId, BareArrayFileHasNoPairing) {
    QTemporaryDir dir;
    const RobotRegistry registry =
        loadJson(dir, QStringLiteral("[%1]").arg(QLatin1String(kTwoRobots)));
    QString error;
    EXPECT_TRUE(registry.pairedRobotId(&error).isEmpty());
    EXPECT_TRUE(error.isEmpty());
}

TEST(ResolveRobotSshTarget, ReleaseIgnoresAStoredOverride) {
    QTemporaryDir dir;
    const RobotRegistry registry = loadJson(
        dir, QStringLiteral(
                 R"({"robots":[{"robot_id":"Roofus#0003","host":"192.168.168.109"}]})"));
    ResolvedRobotSshTarget out;
    QString error;
    ASSERT_TRUE(resolveRobotSshTarget(registry, QStringLiteral("Roofus#0003"),
                                      QStringLiteral("192.168.168.105"), false,
                                      &out, &error));
    EXPECT_EQ(out.host, QStringLiteral("192.168.168.109"));
    EXPECT_EQ(out.ssh_user, QStringLiteral("roofus"));
    EXPECT_EQ(out.robot_id, QStringLiteral("Roofus#0003"));
    EXPECT_TRUE(error.isEmpty());
}

TEST(ResolveRobotSshTarget, DevOverrideWinsWithoutARegistry) {
    ResolvedRobotSshTarget out;
    QString error;
    ASSERT_TRUE(resolveRobotSshTarget(RobotRegistry(), QStringLiteral("Roofus#0003"),
                                      QStringLiteral("  10.0.0.5  "), true, &out,
                                      &error));
    EXPECT_EQ(out.host, QStringLiteral("10.0.0.5"));
    EXPECT_EQ(out.ssh_user, QStringLiteral("roofus"));
    EXPECT_EQ(out.robot_id, QStringLiteral("Roofus#0003"));
}

TEST(ResolveRobotSshTarget, ReleaseErrorDoesNotOfferTheOverride) {
    ResolvedRobotSshTarget out;
    QString error;
    EXPECT_FALSE(resolveRobotSshTarget(RobotRegistry(), QString(),
                                       QStringLiteral("192.168.168.105"), false,
                                       &out, &error));
    EXPECT_TRUE(out.host.isEmpty());
    EXPECT_FALSE(error.contains(QStringLiteral("robot_ip")));
}

TEST(ResolveRobotSshTarget, DevErrorNamesTheOverride) {
    ResolvedRobotSshTarget out;
    QString error;
    EXPECT_FALSE(resolveRobotSshTarget(RobotRegistry(), QString(), QString(), true,
                                       &out, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("robot_ip")));
}

TEST(RetiredGeneralRobotKeys, ReleaseRemovesOnlyTheGeneralKeys) {
    QTemporaryDir dir;
    QSettings settings(dir.filePath(QStringLiteral("ocu.ini")), QSettings::IniFormat);
    settings.setValue(QStringLiteral("robot_ip"), QStringLiteral("192.168.168.105"));
    settings.setValue(QStringLiteral("robot_id"), QStringLiteral("Roofus#0001"));
    settings.setValue(QStringLiteral("setup/robot_id"), QStringLiteral("Roofus#0003"));
    settings.setValue(QStringLiteral("planner/mission_planner/abc/robot_id"),
                      QStringLiteral("Roofus#0002"));
    settings.sync();

    const RetiredGeneralRobotKeys kept = retireLegacyGeneralRobotKeys(settings, false);
    EXPECT_TRUE(kept.robot_ip.isEmpty());
    EXPECT_TRUE(settings.contains(QStringLiteral("robot_ip")));

    const RetiredGeneralRobotKeys retired = retireLegacyGeneralRobotKeys(settings, true);
    EXPECT_EQ(retired.robot_ip, QStringLiteral("192.168.168.105"));
    EXPECT_EQ(retired.robot_id, QStringLiteral("Roofus#0001"));
    EXPECT_FALSE(settings.contains(QStringLiteral("robot_ip")));
    EXPECT_FALSE(settings.contains(QStringLiteral("robot_id")));
    EXPECT_EQ(settings.value(QStringLiteral("setup/robot_id")).toString(),
              QStringLiteral("Roofus#0003"));
    EXPECT_EQ(settings.value(QStringLiteral("planner/mission_planner/abc/robot_id")).toString(),
              QStringLiteral("Roofus#0002"));
}

TEST(RobotIdentity, ExactMatch) {
    const auto report = classifyRobotIdentity(
        QStringLiteral("Roofus#0003"),
        QStringLiteral("noise\nBDR_ROBOT_ID:Roofus#0003\nlater\n"));
    EXPECT_EQ(report.verdict, RobotIdentityVerdict::Match);
    EXPECT_EQ(report.reported, QStringLiteral("Roofus#0003"));
}

TEST(RobotIdentity, TrimsSpacesAndTakesTheFirstMarker) {
    const auto report = classifyRobotIdentity(
        QStringLiteral("Roofus#0003"),
        QStringLiteral("  BDR_ROBOT_ID:  Roofus#0003  \r\nBDR_ROBOT_ID:Roofus#0002\n"));
    EXPECT_EQ(report.verdict, RobotIdentityVerdict::Match);
    EXPECT_EQ(report.reported, QStringLiteral("Roofus#0003"));
}

TEST(RobotIdentity, LeadingZerosAndCaseAreDifferent) {
    const auto zeros = classifyRobotIdentity(
        QStringLiteral("Roofus#0001"), QStringLiteral("BDR_ROBOT_ID:Roofus#001\n"));
    EXPECT_EQ(zeros.verdict, RobotIdentityVerdict::Mismatch);
    EXPECT_EQ(zeros.reported, QStringLiteral("Roofus#001"));

    const auto lower = classifyRobotIdentity(
        QStringLiteral("Roofus#0003"), QStringLiteral("BDR_ROBOT_ID:roofus#0003\n"));
    EXPECT_EQ(lower.verdict, RobotIdentityVerdict::Mismatch);
}

TEST(RobotIdentity, MissingMarkerOrEmptyPayloadIsUnreadable) {
    EXPECT_EQ(classifyRobotIdentity(QStringLiteral("Roofus#0003"),
                                    QStringLiteral("ssh banner\n")).verdict,
              RobotIdentityVerdict::Unreadable);
    EXPECT_EQ(classifyRobotIdentity(QStringLiteral("Roofus#0003"),
                                    QStringLiteral("BDR_ROBOT_ID:   \n")).verdict,
              RobotIdentityVerdict::Unreadable);
    EXPECT_TRUE(classifyRobotIdentity(QStringLiteral("Roofus#0003"),
                                      QStringLiteral("BDR_ROBOT_ID:\n"))
                    .reported.isEmpty());
}

TEST(RobotIdentity, EmptyExpectedIsNotAMatch) {
    const auto report = classifyRobotIdentity(
        QString(), QStringLiteral("BDR_ROBOT_ID:Roofus#0003\n"));
    EXPECT_EQ(report.verdict, RobotIdentityVerdict::Mismatch);
}

TEST(RobotIdentity, MessagesNameBothIds) {
    const QString mismatch = robotIdentityMismatchText(QStringLiteral("Roofus#0003"),
                                                       QStringLiteral("Roofus#0002"));
    EXPECT_TRUE(mismatch.contains(QStringLiteral("Roofus#0002")));
    EXPECT_TRUE(mismatch.contains(QStringLiteral("Roofus#0003")));

    const QString unread =
        robotIdentityUnreadableText(QStringLiteral("Roofus#0003"));
    EXPECT_TRUE(unread.contains(QStringLiteral("/etc/pilot_robot_id")));
    EXPECT_TRUE(unread.contains(QStringLiteral("Roofus#0003")));
    EXPECT_TRUE(robotIdentityUnreadableText(QString()).contains(
        QStringLiteral("/etc/pilot_robot_id")));
}

TEST(RobotIdentity, PreLaunchDecisionBlocksAMismatchEvenWhenSshFailed) {
    EXPECT_EQ(decidePreLaunchIdentity(RobotIdentityVerdict::Mismatch, 0),
              PreLaunchIdentityDecision::Block);
    EXPECT_EQ(decidePreLaunchIdentity(RobotIdentityVerdict::Mismatch, 255),
              PreLaunchIdentityDecision::Block);
    EXPECT_EQ(decidePreLaunchIdentity(RobotIdentityVerdict::Unreadable, 255),
              PreLaunchIdentityDecision::SweepFailed);
    EXPECT_EQ(decidePreLaunchIdentity(RobotIdentityVerdict::Unreadable, 0),
              PreLaunchIdentityDecision::WarnThenSpawn);
    EXPECT_EQ(decidePreLaunchIdentity(RobotIdentityVerdict::Match, 0),
              PreLaunchIdentityDecision::Spawn);
    EXPECT_EQ(decidePreLaunchIdentity(RobotIdentityVerdict::Match, -1),
              PreLaunchIdentityDecision::SweepFailed);
}

TEST(RobotIdentity, CaptureDecision) {
    EXPECT_EQ(decideCaptureIdentity(RobotIdentityVerdict::Mismatch, false),
              CaptureIdentityDecision::Block);
    EXPECT_EQ(decideCaptureIdentity(RobotIdentityVerdict::Mismatch, true),
              CaptureIdentityDecision::Block);
    EXPECT_EQ(decideCaptureIdentity(RobotIdentityVerdict::Unreadable, false),
              CaptureIdentityDecision::WarnAndRecollect);
    EXPECT_EQ(decideCaptureIdentity(RobotIdentityVerdict::Unreadable, true),
              CaptureIdentityDecision::ReadManifest);
    EXPECT_EQ(decideCaptureIdentity(RobotIdentityVerdict::Match, false),
              CaptureIdentityDecision::ReadManifest);
}

namespace {

struct ShellRun {
    int rc = -1;
    QString out;
};

ShellRun runBash(const QString& script) {
    QProcess process;
    process.start(QStringLiteral("bash"),
                  {QStringLiteral("-c"), script});
    ShellRun run;
    if (!process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished(1000);
        return run;
    }
    run.rc = process.exitCode();
    run.out = QString::fromUtf8(process.readAllStandardOutput());
    return run;
}

QString writeId(const QTemporaryDir& dir, const QByteArray& bytes) {
    const QString path = dir.filePath(QStringLiteral("pilot_robot_id"));
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(bytes);
    file.close();
    return path;
}

}  // namespace

TEST(RobotIdentityShell, ProbeFallsThroughOnAMatchAndStopsOnAMismatch) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeId(dir, "Roofus#0003\n");
    const QString matched =
        robotIdentityProbePrefix(QStringLiteral("Roofus#0003"), path) +
        QStringLiteral("printf 'FELL_THROUGH\\n'");
    const ShellRun ok = runBash(matched);
    EXPECT_EQ(ok.rc, 0);
    EXPECT_TRUE(ok.out.contains(QStringLiteral("FELL_THROUGH")));
    EXPECT_EQ(classifyRobotIdentity(QStringLiteral("Roofus#0003"), ok.out).verdict,
              RobotIdentityVerdict::Match);
    EXPECT_FALSE(robotIdentityProbePrefix(QStringLiteral("Roofus#0003"))
                     .contains(QStringLiteral("BDR_IDENTITY_OK")));

    const QString mismatched =
        robotIdentityProbePrefix(QStringLiteral("Roofus#0003"), path) +
        QStringLiteral("printf 'FELL_THROUGH\\n'");
    // Rewrite the file to the other robot. The prefix above was built against
    // the same path, so the next run sees the new contents.
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("Roofus#0002\n");
    }
    const ShellRun blocked = runBash(mismatched);
    EXPECT_EQ(blocked.rc, 0);
    EXPECT_FALSE(blocked.out.contains(QStringLiteral("FELL_THROUGH")));
    const auto report =
        classifyRobotIdentity(QStringLiteral("Roofus#0003"), blocked.out);
    EXPECT_EQ(report.verdict, RobotIdentityVerdict::Mismatch);
    EXPECT_EQ(report.reported, QStringLiteral("Roofus#0002"));
}

TEST(RobotIdentityShell, MissingFileWarnsForTheSweepAndBlocksArming) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("missing"));
    const QString probe =
        robotIdentityProbePrefix(QStringLiteral("Roofus#0003"), path) +
        QStringLiteral("printf 'FELL_THROUGH\\n'");
    const ShellRun sweep = runBash(probe);
    EXPECT_EQ(sweep.rc, 0);
    EXPECT_TRUE(sweep.out.contains(QStringLiteral("FELL_THROUGH")));
    EXPECT_EQ(classifyRobotIdentity(QStringLiteral("Roofus#0003"), sweep.out).verdict,
              RobotIdentityVerdict::Unreadable);

    const QString match =
        robotIdentityMatchPrefix(QStringLiteral("Roofus#0003"), path) +
        QStringLiteral("printf 'FELL_THROUGH\\n'");
    const ShellRun armed = runBash(match);
    EXPECT_EQ(armed.rc, 0);
    EXPECT_FALSE(armed.out.contains(QStringLiteral("FELL_THROUGH")));
    EXPECT_FALSE(armed.out.contains(QStringLiteral("BDR_IDENTITY_OK")));
}

TEST(RobotIdentityShell, MatchPrefixTrimsAndFallsThroughOnlyOnAnExactId) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeId(dir, "  Roofus#0003 \r\n");
    const QString script =
        robotIdentityMatchPrefix(QStringLiteral("Roofus#0003"), path) +
        QStringLiteral("printf 'FELL_THROUGH\\n'");
    const ShellRun run = runBash(script);
    EXPECT_EQ(run.rc, 0);
    EXPECT_TRUE(run.out.contains(QStringLiteral("BDR_IDENTITY_OK")));
    EXPECT_TRUE(run.out.contains(QStringLiteral("FELL_THROUGH")));
    EXPECT_EQ(classifyRobotIdentity(QStringLiteral("Roofus#0003"), run.out).verdict,
              RobotIdentityVerdict::Match);

    const QString zeros =
        robotIdentityMatchPrefix(QStringLiteral("Roofus#0001"),
                                 writeId(dir, "Roofus#001\n")) +
        QStringLiteral("printf 'FELL_THROUGH\\n'");
    const ShellRun different = runBash(zeros);
    EXPECT_FALSE(different.out.contains(QStringLiteral("FELL_THROUGH")));
    EXPECT_EQ(classifyRobotIdentity(QStringLiteral("Roofus#0001"), different.out).verdict,
              RobotIdentityVerdict::Mismatch);
}

TEST(RobotIdentityShell, AQuoteInTheIdStillMatches) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString id = QStringLiteral("a'b");
    const QString path = writeId(dir, "a'b\n");
    const QString script = robotIdentityMatchPrefix(id, path) +
                           QStringLiteral("printf 'FELL_THROUGH\\n'");
    const ShellRun run = runBash(script);
    EXPECT_EQ(run.rc, 0);
    EXPECT_TRUE(run.out.contains(QStringLiteral("FELL_THROUGH")));
    EXPECT_EQ(classifyRobotIdentity(id, run.out).verdict, RobotIdentityVerdict::Match);
}
