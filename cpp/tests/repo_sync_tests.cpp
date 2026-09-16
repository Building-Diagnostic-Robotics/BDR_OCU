/**
 * @file repo_sync_tests.cpp
 * @brief Pure-function surface of RepoSyncManager: branch/host
 *        sanitizers (values interpolated into /bin/bash -c) and the
 *        prefix → colcon package map (lock-step with rebuild_affected.sh).
 */

#include "repo_sync_manager.hpp"
#include "settings_constants.hpp"

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>

using f2c_cpp::RepoSyncManager;
using f2c_cpp::kRobotDeployBranch;

TEST(RobotDeployBranch, CompiledInTargetIsCliffOnAutonomy) {
    EXPECT_STREQ(kRobotDeployBranch, "cliff-on-autonomy");
    EXPECT_TRUE(RepoSyncManager::isSafeBranchName(
        QString::fromLatin1(kRobotDeployBranch)));
}

TEST(IsSafeBranchName, AcceptsRealBranches) {
    EXPECT_TRUE(RepoSyncManager::isSafeBranchName("cliff-on-autonomy"));
    EXPECT_TRUE(RepoSyncManager::isSafeBranchName("main"));
    EXPECT_TRUE(RepoSyncManager::isSafeBranchName("feature/ocu-satellite-roi"));
}

TEST(IsSafeBranchName, RejectsShellAndGitTraps) {
    EXPECT_FALSE(RepoSyncManager::isSafeBranchName(QString()));
    EXPECT_FALSE(RepoSyncManager::isSafeBranchName("foo;rm"));
    EXPECT_FALSE(RepoSyncManager::isSafeBranchName("foo$(id)"));
    EXPECT_FALSE(RepoSyncManager::isSafeBranchName("../escape"));
    EXPECT_FALSE(RepoSyncManager::isSafeBranchName("foo..bar"));
    EXPECT_FALSE(RepoSyncManager::isSafeBranchName("evil.lock"));
    EXPECT_FALSE(RepoSyncManager::isSafeBranchName("trailing/"));
}

TEST(IsSafeHost, AcceptsIpv4AndUsernames) {
    EXPECT_TRUE(RepoSyncManager::isSafeHost("192.168.168.105"));
    EXPECT_TRUE(RepoSyncManager::isSafeHost("10.0.0.1"));
    EXPECT_TRUE(RepoSyncManager::isSafeHost("127.0.0.1"));
    EXPECT_TRUE(RepoSyncManager::isSafeHost("roofus"));
    EXPECT_TRUE(RepoSyncManager::isSafeHost("robot-01"));
}

TEST(IsSafeHost, RejectsShellMetacharacters) {
    EXPECT_FALSE(RepoSyncManager::isSafeHost(QString()));
    EXPECT_FALSE(RepoSyncManager::isSafeHost("192.168.168.105;rm"));
    EXPECT_FALSE(RepoSyncManager::isSafeHost("host$(id)"));
    EXPECT_FALSE(RepoSyncManager::isSafeHost("host`id`"));
    EXPECT_FALSE(RepoSyncManager::isSafeHost("host && id"));
    EXPECT_FALSE(RepoSyncManager::isSafeHost("host'x"));
}

TEST(PackagesForChangedFiles, MapsBuildAffectingPrefixes) {
    EXPECT_EQ(RepoSyncManager::packagesForChangedFiles(
                  {QStringLiteral("src/pilot_control/src/foo.cpp")}),
              QStringList{QStringLiteral("pilot_control")});
    EXPECT_EQ(RepoSyncManager::packagesForChangedFiles(
                  {QStringLiteral("src/FAST_LIO/CMakeLists.txt")}),
              QStringList{QStringLiteral("fast_lio")});

    QStringList odrive = RepoSyncManager::packagesForChangedFiles(
        {QStringLiteral("src/ros_odrive/odrive_can/src/x.cpp")});
    odrive.sort();
    EXPECT_EQ(odrive, (QStringList{QStringLiteral("odrive_botwheel_explorer"),
                                   QStringLiteral("odrive_can"),
                                   QStringLiteral("odrive_ros2_control")}));

    QStringList livox = RepoSyncManager::packagesForChangedFiles(
        {QStringLiteral("src/Livox-SDK2/sdk_core/src/x.cpp")});
    livox.sort();
    EXPECT_EQ(livox, (QStringList{QStringLiteral("fast_lio"),
                                  QStringLiteral("livox_ros_driver2")}));
}

TEST(PackagesForChangedFiles, IgnoresPythonAndUnmappedPaths) {
    EXPECT_TRUE(RepoSyncManager::packagesForChangedFiles(
                    {QStringLiteral("src/pilot_control/scripts/foo.py")})
                    .isEmpty());
    EXPECT_TRUE(RepoSyncManager::packagesForChangedFiles(
                    {QStringLiteral("README.md")})
                    .isEmpty());
}

TEST(NeedsFullWorkspaceBuild, MappedCompileStaysScoped) {
    EXPECT_FALSE(RepoSyncManager::needsFullWorkspaceBuild(
        {QStringLiteral("src/pilot_control/src/foo.cpp")}));
    EXPECT_FALSE(RepoSyncManager::needsFullWorkspaceBuild(
        {QStringLiteral("src/pilot_control/CMakeLists.txt"),
         QStringLiteral("src/pilot_control/scripts/foo.py")}));
}

TEST(NeedsFullWorkspaceBuild, UnmappedCompileForcesFull) {
    EXPECT_TRUE(RepoSyncManager::needsFullWorkspaceBuild(
        {QStringLiteral("src/new_pkg/src/foo.cpp")}));
    EXPECT_TRUE(RepoSyncManager::needsFullWorkspaceBuild(
        {QStringLiteral("CMakeLists.txt")}));
}

TEST(NeedsFullWorkspaceBuild, PythonAloneDoesNot) {
    EXPECT_FALSE(RepoSyncManager::needsFullWorkspaceBuild(
        {QStringLiteral("src/pilot_control/scripts/foo.py"),
         QStringLiteral("README.md")}));
}

TEST(ParseNameStatus, CollectsPathsAndFlagsPackageXml) {
    const auto parsed = RepoSyncManager::parseNameStatus(QStringLiteral(
        "M\tsrc/pilot_control/src/foo.cpp\n"
        "A\tsrc/pilot_control/scripts/bar.py\n"));
    EXPECT_FALSE(parsed.package_xml_added_or_removed);
    EXPECT_EQ(parsed.files.size(), 2);

    const auto added = RepoSyncManager::parseNameStatus(
        QStringLiteral("A\tsrc/new_pkg/package.xml\n"));
    EXPECT_TRUE(added.package_xml_added_or_removed);
    EXPECT_EQ(added.files, QStringList{QStringLiteral("src/new_pkg/package.xml")});

    const auto removed = RepoSyncManager::parseNameStatus(
        QStringLiteral("D\tsrc/old_pkg/package.xml\n"));
    EXPECT_TRUE(removed.package_xml_added_or_removed);

    const auto renamed = RepoSyncManager::parseNameStatus(
        QStringLiteral("R100\tsrc/old/package.xml\tsrc/new/package.xml\n"));
    EXPECT_TRUE(renamed.package_xml_added_or_removed);
    EXPECT_EQ(renamed.files.size(), 2);
}
