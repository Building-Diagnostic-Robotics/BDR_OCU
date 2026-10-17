// OCU mission logs: exact attach to the mission being uploaded, upload
// marks, and space-only retention (uploaded oldest first, current log never).
#include <gtest/gtest.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "ocu_log_store.hpp"

using namespace f2c_cpp::ocu_logs;

namespace {

void write(const QString& path, const QByteArray& data,
           const QDateTime& mtime = QDateTime()) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(data);
    f.close();
    if (mtime.isValid()) {
        ASSERT_TRUE(f.open(QIODevice::ReadWrite));
        f.setFileTime(mtime, QFileDevice::FileModificationTime);
    }
}

QByteArray read(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<missing>");
}

void missionConfig(const QString& mission, const QByteArray& json) {
    write(QDir(mission).filePath("mission_config.json"), json);
}

struct Fixture : ::testing::Test {
    QTemporaryDir tmp;
    QString store;
    QString mission;
    void SetUp() override {
        ASSERT_TRUE(tmp.isValid());
        store = tmp.filePath("store");
        mission = tmp.filePath("stick/October_17_2026/BNY/Mission_100000");
        QDir().mkpath(store);
        QDir().mkpath(mission);
    }
    QString s(const QString& name) const { return QDir(store).filePath(name); }
    QString m(const QString& rel) const { return QDir(mission).filePath(rel); }
};

}  // namespace

// Same table as ros_log_bundle.sanitize_log_id (test_ros_log_bundle.py).
TEST(OcuLogIds, SanitizeMatchesThePythonRule) {
    EXPECT_EQ(sanitizeLogId("mission_20261017_101500"), "mission_20261017_101500");
    EXPECT_EQ(sanitizeLogId("  mission_1  "), "mission_1");
    EXPECT_EQ(sanitizeLogId("../etc/passwd"), "");
    EXPECT_EQ(sanitizeLogId("a/b"), "");
    EXPECT_EQ(sanitizeLogId(".hidden"), "");
    EXPECT_EQ(sanitizeLogId(""), "");
    EXPECT_EQ(sanitizeLogId(QString(129, 'x')), "");
    EXPECT_EQ(sanitizeLogId("a..b"), "");
}

TEST_F(Fixture, IdsComeFromMissionConfigOnlyValidOnes) {
    missionConfig(mission, R"({"ocu_logs": ["mission_1", "../x", 5, "mission_1", "mission_2"]})");
    EXPECT_EQ(idsForMission(mission), (QStringList{"mission_1", "mission_2"}));
}

TEST_F(Fixture, NoConfigOrCorruptConfigMeansNoIds) {
    EXPECT_TRUE(idsForMission(mission).isEmpty());
    missionConfig(mission, "{corrupt");
    EXPECT_TRUE(idsForMission(mission).isEmpty());
    missionConfig(mission, R"({"ocu_logs": "not a list"})");
    EXPECT_TRUE(idsForMission(mission).isEmpty());
}

TEST_F(Fixture, AttachCopiesExactlyTheListedLogs) {
    missionConfig(mission, R"({"ocu_logs": ["mission_A"]})");
    write(s("mission_A.log"), "ocu log A");
    write(s("mission_A.laptop_ros_logs.tar.gz"), "laptop ros A");
    write(s("mission_B.log"), "another mission's log");
    const AttachResult r = attachToMission(mission, store);
    EXPECT_EQ(r.copied, (QStringList{"mission_A.log", "mission_A.laptop_ros_logs.tar.gz"}));
    EXPECT_TRUE(r.error.isEmpty());
    EXPECT_EQ(read(m("logs/ocu/mission_A.log")), "ocu log A");
    EXPECT_EQ(read(m("logs/ocu/mission_A.laptop_ros_logs.tar.gz")), "laptop ros A");
    EXPECT_FALSE(QFileInfo::exists(m("logs/ocu/mission_B.log"))) << "exact match only";
    EXPECT_FALSE(QFileInfo::exists(m("logs/ocu/mission_A.log.part")));
}

TEST_F(Fixture, AttachIsRepeatableAndRefreshesAChangedFile) {
    missionConfig(mission, R"({"ocu_logs": ["mission_A"]})");
    write(s("mission_A.log"), "v1");
    attachToMission(mission, store);
    AttachResult again = attachToMission(mission, store);
    EXPECT_EQ(again.present, QStringList{"mission_A.log"});
    EXPECT_TRUE(again.copied.isEmpty());
    write(s("mission_A.log"), "v2 longer");
    again = attachToMission(mission, store);
    EXPECT_EQ(again.copied, QStringList{"mission_A.log"});
    EXPECT_EQ(read(m("logs/ocu/mission_A.log")), "v2 longer");
}

TEST_F(Fixture, LogFromAnotherLaptopIsReportedMissing) {
    missionConfig(mission, R"({"ocu_logs": ["mission_elsewhere"]})");
    const AttachResult r = attachToMission(mission, store);
    EXPECT_EQ(r.missing, QStringList{"mission_elsewhere"});
    EXPECT_FALSE(QFileInfo::exists(m("logs/ocu")));
}

TEST_F(Fixture, OldMissionWithoutLinkAttachesNothing) {
    missionConfig(mission, R"({"sections": []})");
    write(s("mission_A.log"), "x");
    const AttachResult r = attachToMission(mission, store);
    EXPECT_TRUE(r.ids.isEmpty());
    EXPECT_FALSE(QFileInfo::exists(m("logs")));
}

TEST_F(Fixture, StaleUploaderPartOnStickIsReplaced) {
    missionConfig(mission, R"({"ocu_logs": ["mission_A"]})");
    write(s("mission_A.log"), "full");
    write(m("logs/ocu/mission_A.log.part"), "half");
    attachToMission(mission, store);
    EXPECT_EQ(read(m("logs/ocu/mission_A.log")), "full");
    EXPECT_FALSE(QFileInfo::exists(m("logs/ocu/mission_A.log.part")));
}

TEST_F(Fixture, AlreadyUploadedMissionIsDetected) {
    EXPECT_FALSE(missionAlreadyUploaded(mission));
    write(m("manifest.json"), "{}");
    EXPECT_TRUE(missionAlreadyUploaded(mission));
}

TEST_F(Fixture, MarkUploadedOnlyForThisLaptopsLogs) {
    missionConfig(mission, R"({"ocu_logs": ["mission_A", "mission_other"]})");
    write(s("mission_A.log"), "x");
    attachToMission(mission, store);
    EXPECT_EQ(markUploaded(mission, store), QStringList{"mission_A"});
    EXPECT_TRUE(QFileInfo::exists(s("mission_A.uploaded")));
    EXPECT_FALSE(QFileInfo::exists(s("mission_other.uploaded")));
}

TEST_F(Fixture, MarkUploadedNeedsTheLogToHaveGoneUp) {
    missionConfig(mission, R"({"ocu_logs": ["mission_A"]})");
    write(s("mission_A.log"), "x");
    EXPECT_TRUE(markUploaded(mission, store).isEmpty()) << "attach failed: not on the stick";
    attachToMission(mission, store);
    write(s("mission_A.log"), "grew after the copy");
    EXPECT_TRUE(markUploaded(mission, store).isEmpty()) << "stale copy went up";
    EXPECT_FALSE(QFileInfo::exists(s("mission_A.uploaded")));
}

// ---------------------------------------------------------- retention ---

namespace {
struct Disk {
    qint64 free;
    QString store;
    // Each deleted file "frees" 100 bytes: measure by counting files gone.
    int initial_files = 0;
    qint64 operator()() const {
        const int now = QDir(store).entryList(QDir::Files).size();
        return free + 100LL * (initial_files - now);
    }
};
}  // namespace

TEST_F(Fixture, NothingDeletedAboveTheFloor) {
    write(s("mission_1.log"), "x");
    const PruneResult r = prune(store, 1000, [] { return 5000LL; });
    EXPECT_TRUE(r.deleted_uploaded.isEmpty());
    EXPECT_TRUE(QFileInfo::exists(s("mission_1.log")));
}

TEST_F(Fixture, UnknownFreeSpaceNeverDeletes) {
    write(s("mission_1.log"), "x");
    prune(store, 1000, [] { return -1LL; });
    EXPECT_TRUE(QFileInfo::exists(s("mission_1.log")));
}

TEST_F(Fixture, UploadedOldestGoFirstAndOnlyUntilTheFloor) {
    const QDateTime t0 = QDateTime::currentDateTime().addDays(-10);
    write(s("mission_old_up.log"), "x", t0);
    write(s("mission_old_up.uploaded"), "x");
    write(s("mission_new_up.log"), "x", t0.addDays(5));
    write(s("mission_new_up.laptop_ros_logs.tar.gz"), "x", t0.addDays(5));
    write(s("mission_new_up.uploaded"), "x");
    write(s("mission_oldest_not_up.log"), "x", t0.addDays(-5));
    Disk disk{850, store, static_cast<int>(QDir(store).entryList(QDir::Files).size())};
    const PruneResult r = prune(store, 1000, std::cref(disk));
    // 2 files of the oldest uploaded group free 200: 1050 >= 1000, stop.
    EXPECT_EQ(r.deleted_uploaded, QStringList{"mission_old_up"});
    EXPECT_TRUE(r.deleted_not_uploaded.isEmpty());
    EXPECT_TRUE(r.floor_reached);
    EXPECT_FALSE(QFileInfo::exists(s("mission_old_up.log")));
    EXPECT_FALSE(QFileInfo::exists(s("mission_old_up.uploaded")));
    EXPECT_TRUE(QFileInfo::exists(s("mission_new_up.log")));
    EXPECT_TRUE(QFileInfo::exists(s("mission_oldest_not_up.log")))
        << "not uploaded is kept while uploaded ones can free the space";
}

TEST_F(Fixture, NotUploadedDeletedOnlyWhenNothingElseIsLeft) {
    const QDateTime t0 = QDateTime::currentDateTime().addDays(-10);
    write(s("mission_up.log"), "x", t0.addDays(3));
    write(s("mission_up.uploaded"), "x");
    write(s("mission_a.log"), "x", t0);
    write(s("mission_b.log"), "x", t0.addDays(1));
    Disk disk{600, store, static_cast<int>(QDir(store).entryList(QDir::Files).size())};
    const PruneResult r = prune(store, 1000, std::cref(disk));
    EXPECT_EQ(r.deleted_uploaded, QStringList{"mission_up"});
    EXPECT_EQ(r.deleted_not_uploaded, (QStringList{"mission_a", "mission_b"}));
    EXPECT_TRUE(r.floor_reached);
}

TEST_F(Fixture, CurrentLogIsNeverDeleted) {
    write(s("mission_now.log"), "x");
    Disk disk{0, store, 1};
    const PruneResult r = prune(store, 1000, std::cref(disk), "mission_now");
    EXPECT_TRUE(QFileInfo::exists(s("mission_now.log")));
    EXPECT_FALSE(r.floor_reached);
}

TEST_F(Fixture, InterruptedBundlePartIsCleanedWithItsGroup) {
    write(s("mission_x.log"), "x");
    write(s("mission_x.uploaded"), "x");
    write(s("mission_x.laptop_ros_logs.tar.gz.part"), "x");
    Disk disk{0, store, 3};
    prune(store, 250, std::cref(disk));
    EXPECT_TRUE(QDir(store).entryList(QDir::Files).isEmpty());
}

TEST_F(Fixture, UnrelatedFilesInTheStoreAreLeftAlone) {
    write(s("notes.txt"), "mine");
    write(s("../weird name.log"), "x");
    Disk disk{0, store, 1};
    prune(store, 1000, std::cref(disk));
    EXPECT_TRUE(QFileInfo::exists(s("notes.txt")));
}
