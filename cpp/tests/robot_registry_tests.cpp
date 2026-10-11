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
#include <QSettings>
#include <QTemporaryDir>

using f2c_cpp::ResolvedRobotSshTarget;
using f2c_cpp::RetiredGeneralRobotKeys;
using f2c_cpp::RobotRegistry;
using f2c_cpp::resolveRobotSshTarget;
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
