/**
 * @file satellite_job_store_tests.cpp
 * @brief Plan lifecycle on disk: remove() takes the assets folder with the
 *        plan, and pruneCompleted() keeps only the newest N completed plans
 *        while never touching a PLANNED one.
 */

#include "satellite_job_model.hpp"

#include <gtest/gtest.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cmath>

using f2c_cpp::Job;
using f2c_cpp::JobStore;
using f2c_cpp::ScanParams;

namespace {

Job makeJob(const QString& id, int completed_days_ago) {
    Job job;
    job.id = id;
    job.name = id;
    job.created = job.updated = QDateTime::currentDateTime();
    if (completed_days_ago >= 0) {
        job.last_executed_at =
            QDateTime::currentDateTime().addDays(-completed_days_ago);
    }
    return job;
}

void touchAsset(const JobStore& store, const QString& id) {
    QDir().mkpath(store.assetsDir(id) + QStringLiteral("/tiles"));
    QFile f(store.assetsDir(id) + QStringLiteral("/imagery.json"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("{}");
}

}  // namespace

TEST(JobStore, RemoveDeletesPlanAndAssets) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    JobStore store(tmp.path());
    ASSERT_TRUE(store.save(makeJob("a", -1)));
    touchAsset(store, "a");
    ASSERT_TRUE(QDir(store.assetsDir("a")).exists());

    EXPECT_TRUE(store.remove("a"));
    EXPECT_FALSE(QFile::exists(tmp.path() + "/a.json"));
    EXPECT_FALSE(QDir(store.assetsDir("a")).exists());
    EXPECT_TRUE(store.loadAll().isEmpty());
    EXPECT_FALSE(store.remove("a"));  // already gone
    EXPECT_FALSE(store.remove(QString()));
}

TEST(JobStore, PruneKeepsNewestCompletedAndAllPlanned) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    JobStore store(tmp.path());
    // Two PLANNED plans and seven COMPLETED, completed 0..6 days ago.
    ASSERT_TRUE(store.save(makeJob("planned_1", -1)));
    ASSERT_TRUE(store.save(makeJob("planned_2", -1)));
    for (int d = 0; d < 7; ++d) {
        ASSERT_TRUE(store.save(makeJob(QStringLiteral("done_%1").arg(d), d)));
        touchAsset(store, QStringLiteral("done_%1").arg(d));
    }

    QStringList pruned = store.pruneCompleted(JobStore::kCompletedPlansKept);
    std::sort(pruned.begin(), pruned.end());
    EXPECT_EQ(pruned, (QStringList{"done_5", "done_6"}));

    int planned = 0, completed = 0;
    for (const Job& job : store.loadAll()) {
        (job.executed() ? completed : planned) += 1;
        EXPECT_NE(job.id, "done_5");
        EXPECT_NE(job.id, "done_6");
    }
    EXPECT_EQ(planned, 2);
    EXPECT_EQ(completed, JobStore::kCompletedPlansKept);
    EXPECT_FALSE(QDir(store.assetsDir("done_6")).exists());
    EXPECT_TRUE(QDir(store.assetsDir("done_0")).exists());

    // Idempotent: a second pass has nothing left to remove.
    EXPECT_TRUE(store.pruneCompleted(JobStore::kCompletedPlansKept).isEmpty());
}

// ---------------------------------------------------------------------------
// ScanParams — operator knobs: snap grid, defaults, launch args, persistence.
// ---------------------------------------------------------------------------

TEST(ScanParams, DefaultsMatchDirectorDefaults) {
    const ScanParams p;
    EXPECT_DOUBLE_EQ(p.coverage_width_m, 0.50);
    EXPECT_DOUBLE_EQ(p.scan_speed_mps, 0.40);
}

TEST(ScanParams, SnapClampsAndRoundsToGrid) {
    EXPECT_DOUBLE_EQ(ScanParams::snapWidth(0.10), 0.30);   // below floor
    EXPECT_DOUBLE_EQ(ScanParams::snapWidth(2.70), 2.00);   // above ceiling
    EXPECT_DOUBLE_EQ(ScanParams::snapWidth(0.52), 0.50);   // nearest 0.05
    EXPECT_DOUBLE_EQ(ScanParams::snapWidth(0.53), 0.55);
    EXPECT_DOUBLE_EQ(ScanParams::snapWidth(std::nan("")), 0.50);

    EXPECT_DOUBLE_EQ(ScanParams::snapSpeed(0.10), 0.40);
    EXPECT_DOUBLE_EQ(ScanParams::snapSpeed(0.90), 0.60);
    EXPECT_DOUBLE_EQ(ScanParams::snapSpeed(0.46), 0.50);   // nearest 0.1
    EXPECT_DOUBLE_EQ(ScanParams::snapSpeed(0.44), 0.40);
    EXPECT_DOUBLE_EQ(ScanParams::snapSpeed(std::nan("")), 0.40);
}

TEST(ScanParams, LaunchArgsPinOverlapToZero) {
    ScanParams p;
    p.coverage_width_m = 0.75;
    p.scan_speed_mps = 0.6;
    EXPECT_EQ(p.launchArgs(),
              QStringLiteral(" coverage_width:=0.75 swath_overlap:=0.0 "
                             "desired_linear_speed:=0.60"));
    // Out-of-range input is snapped before it reaches the robot.
    p.coverage_width_m = 9.0;
    p.scan_speed_mps = 0.0;
    EXPECT_EQ(p.launchArgs(),
              QStringLiteral(" coverage_width:=2.00 swath_overlap:=0.0 "
                             "desired_linear_speed:=0.40"));
}

TEST(ScanParams, LaunchArgsAlwaysCarryADecimalPoint) {
    // Every value here reaches a strictly-typed double ROS parameter. An
    // integer-looking token (e.g. "0" or "1") is inferred as an int by
    // launch_ros and kills the director node in its constructor.
    for (const double width : {0.30, 0.50, 1.00, 2.00}) {
        for (const double speed : {0.40, 0.50, 0.60}) {
            ScanParams p;
            p.coverage_width_m = width;
            p.scan_speed_mps = speed;
            const QStringList tokens =
                p.launchArgs().split(QLatin1Char(' '), Qt::SkipEmptyParts);
            for (const QString& token : tokens) {
                const QString value = token.section(QStringLiteral(":="), 1);
                ASSERT_FALSE(value.isEmpty()) << token.toStdString();
                EXPECT_TRUE(value.contains(QLatin1Char('.')))
                    << token.toStdString();
            }
        }
    }
}

TEST(JobStore, ScanParamsRoundTripAndLegacyDefault) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    JobStore store(tmp.path());

    Job job = makeJob("knobs", -1);
    job.scan.coverage_width_m = 1.25;
    job.scan.scan_speed_mps = 0.5;
    ASSERT_TRUE(store.save(job));
    const QVector<Job> loaded = store.loadAll();
    ASSERT_EQ(loaded.size(), 1);
    EXPECT_DOUBLE_EQ(loaded[0].scan.coverage_width_m, 1.25);
    EXPECT_DOUBLE_EQ(loaded[0].scan.scan_speed_mps, 0.5);

    // A schema <= 5 plan has no "scan" block: it ran the director defaults,
    // and must keep doing so after the upgrade.
    QJsonObject legacy = job.toJson();
    legacy.remove("scan");
    legacy["schema"] = 5;
    const Job old = Job::fromJson(legacy);
    EXPECT_DOUBLE_EQ(old.scan.coverage_width_m, ScanParams::kWidthDefaultM);
    EXPECT_DOUBLE_EQ(old.scan.scan_speed_mps, ScanParams::kSpeedDefaultMps);

    // A hand-edited file cannot push an out-of-range value onto the robot.
    QJsonObject edited = job.toJson();
    QJsonObject sp = edited.value("scan").toObject();
    sp["coverage_width_m"] = 5.0;
    sp["scan_speed_mps"] = 1.5;
    edited["scan"] = sp;
    const Job clamped = Job::fromJson(edited);
    EXPECT_DOUBLE_EQ(clamped.scan.coverage_width_m, ScanParams::kWidthMaxM);
    EXPECT_DOUBLE_EQ(clamped.scan.scan_speed_mps, ScanParams::kSpeedMaxMps);
}

TEST(JobStore, PruneUnderCapIsNoop) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    JobStore store(tmp.path());
    ASSERT_TRUE(store.save(makeJob("done_0", 0)));
    ASSERT_TRUE(store.save(makeJob("done_1", 1)));
    EXPECT_TRUE(store.pruneCompleted(JobStore::kCompletedPlansKept).isEmpty());
    EXPECT_EQ(store.loadAll().size(), 2);
}
