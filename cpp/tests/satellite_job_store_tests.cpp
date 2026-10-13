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
using f2c_cpp::ScanPreset;

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

TEST(ScanParams, DefaultsAreStandard) {
    const ScanParams p;
    EXPECT_EQ(p.preset, ScanPreset::Standard);
    EXPECT_DOUBLE_EQ(p.coverage_width_m, ScanParams::kWidthDefaultM);
    EXPECT_DOUBLE_EQ(p.scan_speed_mps, ScanParams::kSpeedDefaultMps);
    EXPECT_NEAR(p.coverage_width_m, 0.9144, 1e-12);
    EXPECT_DOUBLE_EQ(p.scan_speed_mps, 0.50);
}

TEST(ScanParams, SnapClampsToTheHalfFootGrid) {
    EXPECT_NEAR(ScanParams::snapWidth(0.25),
                ScanParams::metersFromHalfFeet(2), 1e-12);  // 1 ft
    EXPECT_NEAR(ScanParams::snapWidth(2.5),
                ScanParams::metersFromHalfFeet(14), 1e-12);  // 7 ft
    EXPECT_NEAR(ScanParams::snapWidth(1.0),
                ScanParams::metersFromHalfFeet(7), 1e-12);  // 3.5 ft
    EXPECT_NEAR(ScanParams::snapWidth(std::nan("")), ScanParams::kWidthDefaultM,
                1e-12);

    for (int half = ScanParams::kWidthHalfFeetMin;
         half <= ScanParams::kWidthHalfFeetMax; ++half) {
        const double meters = ScanParams::metersFromHalfFeet(half);
        EXPECT_EQ(ScanParams::halfFeetFromMeters(meters), half);
    }

    EXPECT_DOUBLE_EQ(ScanParams::snapSpeed(0.10), 0.40);
    EXPECT_DOUBLE_EQ(ScanParams::snapSpeed(0.90), 0.60);
    EXPECT_DOUBLE_EQ(ScanParams::snapSpeed(0.46), 0.50);
    EXPECT_DOUBLE_EQ(ScanParams::snapSpeed(0.44), 0.40);
    EXPECT_DOUBLE_EQ(ScanParams::snapSpeed(std::nan("")), 0.50);

    ScanParams non_finite;
    non_finite.preset = ScanPreset::Custom;
    non_finite.coverage_width_m = std::nan("");
    non_finite.scan_speed_mps = 0.60;
    const ScanParams standard = non_finite.resolved();
    EXPECT_EQ(standard.preset, ScanPreset::Standard);
    EXPECT_DOUBLE_EQ(standard.coverage_width_m, ScanParams::kWidthDefaultM);
    EXPECT_DOUBLE_EQ(standard.scan_speed_mps, ScanParams::kSpeedDefaultMps);
}

TEST(ScanParams, NamedPresetsLaunchTheirTableValues) {
    EXPECT_EQ(ScanParams::fromPreset(ScanPreset::Detailed).launchArgs(),
              QStringLiteral(" coverage_width:=0.6096 swath_overlap:=0.0 "
                             "desired_linear_speed:=0.4000"));
    EXPECT_EQ(ScanParams::fromPreset(ScanPreset::Standard).launchArgs(),
              QStringLiteral(" coverage_width:=0.9144 swath_overlap:=0.0 "
                             "desired_linear_speed:=0.5000"));
    EXPECT_EQ(ScanParams::fromPreset(ScanPreset::Quick).launchArgs(),
              QStringLiteral(" coverage_width:=1.5240 swath_overlap:=0.0 "
                             "desired_linear_speed:=0.6000"));

    // A named card ignores a stored width that does not belong to it.
    ScanParams dirty = ScanParams::fromPreset(ScanPreset::Quick);
    dirty.coverage_width_m = 0.10;
    EXPECT_EQ(dirty.launchArgs(),
              ScanParams::fromPreset(ScanPreset::Quick).launchArgs());
}

TEST(ScanParams, LaunchArgsPinOverlapToZero) {
    ScanParams p;
    p.preset = ScanPreset::Custom;
    p.coverage_width_m = 0.75;  // snaps to 2.5 ft
    p.scan_speed_mps = 0.6;
    EXPECT_EQ(p.launchArgs(),
              QStringLiteral(" coverage_width:=0.7620 swath_overlap:=0.0 "
                             "desired_linear_speed:=0.6000"));
    p.coverage_width_m = 9.0;
    p.scan_speed_mps = 0.0;
    EXPECT_EQ(p.launchArgs(),
              QStringLiteral(" coverage_width:=2.1336 swath_overlap:=0.0 "
                             "desired_linear_speed:=0.4000"));
}

TEST(ScanParams, LaunchArgsAlwaysCarryADecimalPoint) {
    // Every value here reaches a strictly-typed double ROS parameter. An
    // integer-looking token (e.g. "0" or "1") is inferred as an int by
    // launch_ros and kills the director node in its constructor.
    QVector<ScanParams> cases;
    cases.append(ScanParams::fromPreset(ScanPreset::Detailed));
    cases.append(ScanParams::fromPreset(ScanPreset::Standard));
    cases.append(ScanParams::fromPreset(ScanPreset::Quick));
    for (int half = ScanParams::kWidthHalfFeetMin;
         half <= ScanParams::kWidthHalfFeetMax; ++half) {
        ScanParams custom;
        custom.preset = ScanPreset::Custom;
        custom.coverage_width_m = ScanParams::metersFromHalfFeet(half);
        custom.scan_speed_mps = ScanParams::kSpeedMinMps;
        cases.append(custom);
    }
    for (const ScanParams& params : cases) {
        const QStringList tokens =
            params.launchArgs().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (const QString& token : tokens) {
            const QString value = token.section(QStringLiteral(":="), 1);
            ASSERT_FALSE(value.isEmpty()) << token.toStdString();
            EXPECT_TRUE(value.contains(QLatin1Char('.')))
                << token.toStdString();
        }
    }
}

TEST(ScanParams, CustomStaysCustomWhenItLandsOnAPreset) {
    ScanParams custom;
    custom.preset = ScanPreset::Custom;
    custom.coverage_width_m = ScanParams::kWidthDefaultM;
    custom.scan_speed_mps = ScanParams::kSpeedDefaultMps;
    const ScanParams snapped = custom.snapped();
    EXPECT_EQ(snapped.preset, ScanPreset::Custom);
    EXPECT_DOUBLE_EQ(snapped.coverage_width_m, ScanParams::kWidthDefaultM);
}

TEST(JobStore, ScanParamsRoundTripAndLegacyDefault) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    JobStore store(tmp.path());

    Job job = makeJob("knobs", -1);
    job.scan.preset = ScanPreset::Custom;
    job.scan.coverage_width_m = ScanParams::metersFromHalfFeet(7);  // 3.5 ft
    job.scan.scan_speed_mps = 0.60;
    ASSERT_TRUE(store.save(job));
    const QVector<Job> loaded = store.loadAll();
    ASSERT_EQ(loaded.size(), 1);
    EXPECT_EQ(loaded[0].scan.preset, ScanPreset::Custom);
    EXPECT_NEAR(loaded[0].scan.coverage_width_m,
                ScanParams::metersFromHalfFeet(7), 1e-9);
    EXPECT_DOUBLE_EQ(loaded[0].scan.scan_speed_mps, 0.60);
    EXPECT_EQ(job.toJson().value("schema").toInt(), 7);
    EXPECT_EQ(job.toJson().value("scan").toObject().value("preset").toString(),
              QStringLiteral("custom"));

    // Schema 7 named card: the table wins over a stored width.
    QJsonObject named = job.toJson();
    QJsonObject named_scan = named.value("scan").toObject();
    named_scan["preset"] = QStringLiteral("standard");
    named_scan["coverage_width_m"] = 2.0;
    named["scan"] = named_scan;
    const Job standard = Job::fromJson(named);
    EXPECT_EQ(standard.scan.preset, ScanPreset::Standard);
    EXPECT_DOUBLE_EQ(standard.scan.coverage_width_m, ScanParams::kWidthDefaultM);
    EXPECT_DOUBLE_EQ(standard.scan.scan_speed_mps, ScanParams::kSpeedDefaultMps);

    // Schema 6 exact match (2 ft at 0.4 m/s) becomes Detailed.
    QJsonObject exact = job.toJson();
    exact["schema"] = 6;
    QJsonObject exact_scan = exact.value("scan").toObject();
    exact_scan.remove("preset");
    exact_scan["coverage_width_m"] = ScanParams::metersFromHalfFeet(4);
    exact_scan["scan_speed_mps"] = 0.40;
    exact["scan"] = exact_scan;
    const Job detailed = Job::fromJson(exact);
    EXPECT_EQ(detailed.scan.preset, ScanPreset::Detailed);
    EXPECT_NEAR(detailed.scan.coverage_width_m,
                ScanParams::metersFromHalfFeet(4), 1e-9);
    EXPECT_DOUBLE_EQ(detailed.scan.scan_speed_mps, 0.40);

    // Schema 6 value that is not a card (1.0 m snaps to 3.5 ft, Medium).
    QJsonObject odd = exact;
    QJsonObject odd_scan = exact_scan;
    odd_scan["coverage_width_m"] = 1.0;
    odd_scan["scan_speed_mps"] = 0.50;
    odd["scan"] = odd_scan;
    const Job custom = Job::fromJson(odd);
    EXPECT_EQ(custom.scan.preset, ScanPreset::Custom);
    EXPECT_NEAR(custom.scan.coverage_width_m,
                ScanParams::metersFromHalfFeet(7), 1e-9);
    EXPECT_DOUBLE_EQ(custom.scan.scan_speed_mps, 0.50);

    // The old default (0.50 m at 0.40 m/s) is the closest half-foot, Slow.
    // It must not become Standard: that would double the lane width.
    QJsonObject previous = exact;
    QJsonObject previous_scan;
    previous_scan["coverage_width_m"] = 0.50;
    previous_scan["scan_speed_mps"] = 0.40;
    previous["scan"] = previous_scan;
    const Job old_default = Job::fromJson(previous);
    EXPECT_EQ(old_default.scan.preset, ScanPreset::Custom);
    EXPECT_NEAR(old_default.scan.coverage_width_m,
                ScanParams::metersFromHalfFeet(3), 1e-9);
    EXPECT_DOUBLE_EQ(old_default.scan.scan_speed_mps, 0.40);

    // Schema <= 5 has no scan block and loads as Standard.
    QJsonObject legacy = job.toJson();
    legacy.remove("scan");
    legacy["schema"] = 5;
    const Job old = Job::fromJson(legacy);
    EXPECT_EQ(old.scan.preset, ScanPreset::Standard);
    EXPECT_DOUBLE_EQ(old.scan.coverage_width_m, ScanParams::kWidthDefaultM);
    EXPECT_DOUBLE_EQ(old.scan.scan_speed_mps, ScanParams::kSpeedDefaultMps);

    // A hand-edited Custom file cannot push an out-of-range value out.
    QJsonObject edited = job.toJson();
    QJsonObject sp = edited.value("scan").toObject();
    sp["preset"] = QStringLiteral("custom");
    sp["coverage_width_m"] = 5.0;
    sp["scan_speed_mps"] = 1.5;
    edited["scan"] = sp;
    const Job clamped = Job::fromJson(edited);
    EXPECT_EQ(clamped.scan.preset, ScanPreset::Custom);
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
