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
#include <QTemporaryDir>

using f2c_cpp::Job;
using f2c_cpp::JobStore;

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

TEST(JobStore, PruneUnderCapIsNoop) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    JobStore store(tmp.path());
    ASSERT_TRUE(store.save(makeJob("done_0", 0)));
    ASSERT_TRUE(store.save(makeJob("done_1", 1)));
    EXPECT_TRUE(store.pruneCompleted(JobStore::kCompletedPlansKept).isEmpty());
    EXPECT_EQ(store.loadAll().size(), 2);
}
