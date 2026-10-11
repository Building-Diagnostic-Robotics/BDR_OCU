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
#include <QTemporaryDir>

using f2c_cpp::RobotRegistry;

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
