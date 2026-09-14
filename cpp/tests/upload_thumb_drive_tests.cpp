/**
 * @file upload_thumb_drive_tests.cpp
 * @brief The thumb-drive upload path's pure pieces: the local data-root
 *        walk (`UploadStateProbe::scanLocalDataRoot`), the script
 *        resolver, and the /proc/mounts device → mount-point lookup.
 */

#include "offload_status.hpp"
#include "thumb_drive_watcher.hpp"
#include "upload_runner.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QTime>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTimer>

using f2c_cpp::OffloadCopyState;
using f2c_cpp::OffloadSnapshot;
using f2c_cpp::StickSync;
using f2c_cpp::ThumbDriveWatcher;
using f2c_cpp::UploadRunner;
using f2c_cpp::UploadStateProbe;
using f2c_cpp::UploadStatus;
using f2c_cpp::UploadTarget;
using f2c_cpp::classifyStickSync;
using f2c_cpp::parseOffloadStatusJson;

namespace {

void writeFile(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly)) << path.toStdString();
    f.write(bytes);
}

// A stick as the robot's offload worker leaves it:
//   <root>/<date>/<building>/{Mission_HHMMSS, Section_N_HHMMSS}
// plus the worker's own hidden `.offload/` bookkeeping.
void layOutStick(const QString& root) {
    const QString b = root + "/September_13_2026/Acme_HQ";
    writeFile(b + "/Section_1_101500/Visual_data/frame_0001.jpg", QByteArray(1000, 'x'));
    writeFile(b + "/Section_1_101500/GPR_scan_data/scan.csv", QByteArray(500, 'y'));
    writeFile(b + "/Section_1_101500/session_config.json",
              R"({"building_name":"Acme HQ","operator_name":"Blake","timestamp":"101500","day_name":"September_13_2026"})");

    // Partially uploaded: 2 of 3 files recorded.
    writeFile(b + "/Section_2_113000/a.bin", QByteArray(10, 'a'));
    writeFile(b + "/Section_2_113000/b.bin", QByteArray(10, 'b'));
    writeFile(b + "/Section_2_113000/c.bin", QByteArray(10, 'c'));
    writeFile(b + "/Section_2_113000/upload_state.json",
              R"({"completed":["a.bin","b.bin"]})");
    writeFile(b + "/Section_2_113000/pause.flag", "");

    // Fully uploaded: manifest present.
    writeFile(b + "/Mission_101400/GNSS_data/rover.ubx", QByteArray(64, 'g'));
    writeFile(b + "/Mission_101400/mission_config.json", "{}");
    writeFile(b + "/Mission_101400/thumbsync_manifest.json", "{}");
    writeFile(b + "/Mission_101400/manifest.json",
              R"({"files":[{"relpath":"GNSS_data/rover.ubx"},{"relpath":"mission_config.json"},{"relpath":"thumbsync_manifest.json"}]})");

    // Noise the walk must ignore.
    writeFile(root + "/.offload/status.json", "{}");
    writeFile(root + "/September_13_2026/Acme_HQ/notes.txt", "not a section");
    writeFile(root + "/September_13_2026/Acme_HQ/Other_folder/x", "x");
}

const UploadTarget* find(const QList<UploadTarget>& all, const QString& section) {
    for (const UploadTarget& t : all) {
        if (t.section_name == section) return &t;
    }
    return nullptr;
}

}  // namespace

TEST(UploadThumbDrive, LocalWalkFindsDepthThreeSectionsAndMissions) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    layOutStick(tmp.path());

    const QList<UploadTarget> all = UploadStateProbe::scanLocalDataRoot(tmp.path());
    ASSERT_EQ(all.size(), 3);

    const UploadTarget* s1 = find(all, "Section_1_101500");
    ASSERT_NE(s1, nullptr);
    EXPECT_EQ(s1->date_folder, "September_13_2026");
    EXPECT_EQ(s1->building_slug, "Acme_HQ");
    EXPECT_EQ(s1->run_id, "September_13_2026/Acme_HQ/Section_1_101500");
    EXPECT_EQ(s1->data_path, tmp.path() + "/September_13_2026/Acme_HQ/Section_1_101500");
    EXPECT_EQ(s1->status, UploadStatus::None);
    EXPECT_EQ(s1->completed_files, 0);
    EXPECT_EQ(s1->total_files, 3);
    EXPECT_GT(s1->total_bytes, 1500);
    EXPECT_FALSE(s1->isMission());
    EXPECT_EQ(s1->building_name, "Acme HQ");
    EXPECT_EQ(s1->operator_name, "Blake");
    ASSERT_TRUE(s1->captured_at.isValid());
    EXPECT_EQ(s1->captured_at.date(), QDate(2026, 9, 13));
    EXPECT_EQ(s1->captured_at.time(), QTime(10, 15, 0));
}

TEST(UploadThumbDrive, LocalWalkClassifiesPartialFromStateFile) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    layOutStick(tmp.path());

    const QList<UploadTarget> all = UploadStateProbe::scanLocalDataRoot(tmp.path());
    const UploadTarget* s2 = find(all, "Section_2_113000");
    ASSERT_NE(s2, nullptr);
    EXPECT_EQ(s2->status, UploadStatus::Partial);
    EXPECT_EQ(s2->completed_files, 2);
    // Sentinels (upload_state.json, pause.flag) are not data files.
    EXPECT_EQ(s2->total_files, 3);
}

TEST(UploadThumbDrive, LocalWalkClassifiesDoneFromManifest) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    layOutStick(tmp.path());

    const QList<UploadTarget> all = UploadStateProbe::scanLocalDataRoot(tmp.path());
    const UploadTarget* m = find(all, "Mission_101400");
    ASSERT_NE(m, nullptr);
    EXPECT_TRUE(m->isMission());
    EXPECT_EQ(m->status, UploadStatus::Done);
    EXPECT_EQ(m->completed_files, 3);
    // The robot's thumbsync_manifest.json is ordinary data to the uploader.
    EXPECT_EQ(m->total_files, 3);
}

TEST(UploadThumbDrive, LocalWalkOnMissingRootIsEmpty) {
    EXPECT_TRUE(UploadStateProbe::scanLocalDataRoot("/nonexistent/RDATA_EXT").isEmpty());
}

TEST(UploadThumbDrive, ScriptResolverHonoursOverrideAndRejectsMissing) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString script = tmp.path() + "/uploader.py";
    writeFile(script, "#!/usr/bin/env python3\n");
    EXPECT_EQ(f2c_cpp::UploadRunner::resolveLocalScriptPath(script),
              QFileInfo(script).canonicalFilePath());
    // A bad override falls through to the install locations. Whether one
    // exists depends on the machine (installed .deb), so only demand
    // that a non-empty answer is a real file.
    const QString fallback =
        f2c_cpp::UploadRunner::resolveLocalScriptPath(tmp.path() + "/nope.py");
    EXPECT_TRUE(fallback.isEmpty() || QFileInfo(fallback).isFile());
    EXPECT_NE(fallback, tmp.path() + "/nope.py");
}

TEST(UploadThumbDrive, MountsLookupDecodesKernelEscapes) {
    const QString mounts =
        "sysfs /sys sysfs rw 0 0\n"
        "/dev/sda2 / ext4 rw 0 0\n"
        "/dev/sdb1 /media/ops/RDATA_EXT exfat rw 0 0\n"
        "/dev/sdc1 /media/ops/New\\040Volume vfat rw 0 0\n";
    EXPECT_EQ(ThumbDriveWatcher::mountPointForDevice("/dev/sdb1", mounts),
              "/media/ops/RDATA_EXT");
    EXPECT_EQ(ThumbDriveWatcher::mountPointForDevice("/dev/sdc1", mounts),
              "/media/ops/New Volume");
    EXPECT_TRUE(ThumbDriveWatcher::mountPointForDevice("/dev/sdd1", mounts).isEmpty());
    EXPECT_TRUE(ThumbDriveWatcher::mountPointForDevice("", mounts).isEmpty());
}

// The local launch end to end against a stub uploader.py that speaks the
// stdout contract: argv/env reach the script, per-file lines become
// signals, a stale pause.flag is cleared before launch, and the queue
// finishes clean.
TEST(UploadThumbDrive, LocalRunnerLaunchesScriptWithEnvAndParsesStdout) {
    int argc = 1;
    char arg0[] = "upload_thumb_drive_tests";
    char* argv[] = {arg0, nullptr};
    QCoreApplication app(argc, argv);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    layOutStick(tmp.path());
    const QList<UploadTarget> all = UploadStateProbe::scanLocalDataRoot(tmp.path());
    const UploadTarget* s2 = find(all, "Section_2_113000");
    ASSERT_NE(s2, nullptr);
    ASSERT_TRUE(QFile::exists(s2->data_path + "/pause.flag"));

    const QString stub = tmp.path() + "/stub_uploader.py";
    writeFile(stub, R"PY(
import os, sys
root, robot_id, run_id = sys.argv[1:4]
assert os.environ["BDR_CLOUD_API_BASE"] == "https://api.example", os.environ
assert os.environ["BDR_CLOUD_CLIENT_ID"] == "client_x"
assert os.environ["BDR_CLOUD_DEVICE_TOKEN"] == "tok_y"
assert not os.path.exists(os.path.join(root, "pause.flag")), "pause.flag not cleared"
print(f"Resuming upload. 0 files already completed. {robot_id} {run_id}", flush=True)
print("Skipping already uploaded: a.bin", flush=True)
print("\u2713 Uploaded: b.bin", flush=True)
print("\u2713 Uploaded: c.bin", flush=True)
print("Upload complete: {}", flush=True)
)PY");

    UploadRunner runner;
    runner.setSource(f2c_cpp::UploadSource::ThumbDrive);
    runner.setLocalScriptPath(stub);
    runner.setCloudAuth("https://api.example/", "client_x", "tok_y");
    runner.setRobotId("Roofus#0001");
    runner.setQueue({*s2});

    QStringList uploaded, skipped, logs, failures;
    int completed = 0;
    bool finished = false, cancelled = true;
    QObject::connect(&runner, &UploadRunner::fileUploaded,
                     [&](const UploadTarget&, const QString& rel) { uploaded << rel; });
    QObject::connect(&runner, &UploadRunner::fileSkipped,
                     [&](const UploadTarget&, const QString& rel) { skipped << rel; });
    QObject::connect(&runner, &UploadRunner::logLine,
                     [&](const QString& l) { logs << l; });
    QObject::connect(&runner, &UploadRunner::targetFailed,
                     [&](const UploadTarget&, const QString& e) { failures << e; });
    QObject::connect(&runner, &UploadRunner::targetCompleted,
                     [&](const UploadTarget&) { ++completed; });
    QEventLoop loop;
    QObject::connect(&runner, &UploadRunner::queueFinished, [&](bool c) {
        finished = true;
        cancelled = c;
        loop.quit();
    });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);

    runner.start();
    loop.exec();

    ASSERT_TRUE(finished) << "runner did not finish; logs: "
                          << logs.join(" | ").toStdString();
    EXPECT_FALSE(cancelled);
    EXPECT_TRUE(failures.isEmpty()) << failures.join(" | ").toStdString();
    EXPECT_EQ(completed, 1);
    EXPECT_EQ(uploaded, QStringList({"b.bin", "c.bin"}));
    EXPECT_EQ(skipped, QStringList({"a.bin"}));
    EXPECT_FALSE(runner.isBusy());
    EXPECT_FALSE(QFile::exists(s2->data_path + "/pause.flag"));
    // The robot id and run id travelled as argv[2..3].
    bool saw_args = false;
    for (const QString& l : logs) {
        if (l.contains("Roofus#0001 September_13_2026/Acme_HQ/Section_2_113000")) {
            saw_args = true;
        }
    }
    EXPECT_TRUE(saw_args) << logs.join(" | ").toStdString();
}

TEST(UploadThumbDrive, LocalRunnerReportsMissingInterpreterAsFailure) {
    int argc = 1;
    char arg0[] = "upload_thumb_drive_tests";
    char* argv[] = {arg0, nullptr};
    QCoreApplication app(argc, argv);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    layOutStick(tmp.path());
    const QList<UploadTarget> all = UploadStateProbe::scanLocalDataRoot(tmp.path());
    ASSERT_FALSE(all.isEmpty());

    // A script path that resolves (so start() passes preflight) but a
    // PATH with no python3, so QProcess fails to start.
    const QString stub = tmp.path() + "/stub.py";
    writeFile(stub, "print('never runs')\n");
    qputenv("PATH", tmp.path().toUtf8());

    UploadRunner runner;
    runner.setLocalScriptPath(stub);
    runner.setCloudAuth("https://api.example", "c", "t");
    runner.setRobotId("R");
    runner.setQueue({all.first()});

    QString failure;
    bool finished = false;
    QEventLoop loop;
    QObject::connect(&runner, &UploadRunner::targetFailed,
                     [&](const UploadTarget&, const QString& e) { failure = e; });
    QObject::connect(&runner, &UploadRunner::queueFinished, [&](bool) {
        finished = true;
        loop.quit();
    });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    runner.start();
    loop.exec();

    EXPECT_TRUE(finished);
    EXPECT_FALSE(runner.isBusy());
    EXPECT_TRUE(failure.contains("python3")) << failure.toStdString();
}

TEST(UploadThumbDrive, MountsLookupLastMountWins) {
    const QString mounts =
        "/dev/sdb1 /mnt/first exfat rw 0 0\n"
        "/dev/sdb1 /media/ops/RDATA_EXT exfat rw 0 0\n";
    EXPECT_EQ(ThumbDriveWatcher::mountPointForDevice("/dev/sdb1", mounts),
              "/media/ops/RDATA_EXT");
}

TEST(OffloadStatus, ParsesIdleAndTreatsMissingQueuedAsDone) {
    const OffloadSnapshot snap = parseOffloadStatusJson(
        R"({"state":"idle","queued":0,"updated_at":"2026-09-13T12:00:00Z"})");
    ASSERT_TRUE(snap.parse_ok);
    EXPECT_EQ(snap.state, OffloadCopyState::Idle);
    EXPECT_FALSE(snap.copyIncomplete());
}

TEST(OffloadStatus, DoneAndSkippedMapToIdle) {
    EXPECT_EQ(parseOffloadStatusJson(R"({"state":"done"})").state,
              OffloadCopyState::Idle);
    EXPECT_EQ(parseOffloadStatusJson(R"({"state":"skipped"})").state,
              OffloadCopyState::Idle);
}

TEST(OffloadStatus, WaitingForDriveIsIncomplete) {
    const OffloadSnapshot snap = parseOffloadStatusJson(
        R"({"state":"waiting_for_drive","queued":1,"mission":null,"jobs":{"a":{"state":"waiting_for_drive"}}})");
    ASSERT_TRUE(snap.parse_ok);
    EXPECT_EQ(snap.state, OffloadCopyState::WaitingForDrive);
    EXPECT_TRUE(snap.copyIncomplete());
}

TEST(OffloadStatus, CopyingAndQueuedAreIncomplete) {
    EXPECT_TRUE(parseOffloadStatusJson(R"({"state":"copying","queued":1})")
                    .copyIncomplete());
    EXPECT_TRUE(parseOffloadStatusJson(R"({"state":"queued","queued":2})")
                    .copyIncomplete());
}

TEST(OffloadStatus, WorkerErrorRollupWithQueuedJobIsQueued) {
    // rdata_offload.py writes top-level state=error for any leftover
    // queue that isn't copying / waiting_for_drive — including jobs
    // that are merely queued.
    const OffloadSnapshot snap = parseOffloadStatusJson(
        R"({"state":"error","queued":1,"jobs":{"a":{"state":"queued"}}})");
    ASSERT_TRUE(snap.parse_ok);
    EXPECT_EQ(snap.state, OffloadCopyState::Queued);
    EXPECT_TRUE(snap.copyIncomplete());
    EXPECT_TRUE(snap.error.isEmpty());
}

TEST(OffloadStatus, PerJobErrorIsErrorAndKeepsMessage) {
    const OffloadSnapshot snap = parseOffloadStatusJson(
        R"({"state":"error","queued":1,"jobs":{"a":{"state":"error","error":"rsync failed"}}})");
    ASSERT_TRUE(snap.parse_ok);
    EXPECT_EQ(snap.state, OffloadCopyState::Error);
    EXPECT_TRUE(snap.copyIncomplete());
    EXPECT_EQ(snap.error, "rsync failed");
}

TEST(OffloadStatus, JobsObjectCountsWhenQueuedMissing) {
    const OffloadSnapshot snap = parseOffloadStatusJson(
        R"({"state":"idle","jobs":{"one":{"state":"queued"},"two":{"state":"queued"}}})");
    ASSERT_TRUE(snap.parse_ok);
    EXPECT_EQ(snap.queued, 2);
    EXPECT_TRUE(snap.copyIncomplete());
}

TEST(OffloadStatus, InvalidJsonIsNotIncomplete) {
    const OffloadSnapshot snap = parseOffloadStatusJson("not-json");
    EXPECT_FALSE(snap.parse_ok);
    EXPECT_FALSE(snap.copyIncomplete());
}

TEST(OffloadStatus, StickClassifyAbsentEmptyIncompleteComplete) {
    EXPECT_EQ(classifyStickSync(QString()), StickSync::Absent);
    EXPECT_EQ(classifyStickSync(QStringLiteral("/no/such/rdata_ext")),
              StickSync::Absent);

    QTemporaryDir empty;
    ASSERT_TRUE(empty.isValid());
    EXPECT_EQ(classifyStickSync(empty.path()), StickSync::Empty);

    // layOutStick writes thumbsync only on Mission_* — that is a
    // finished robot copy. Section_* siblings never get their own file.
    QTemporaryDir copied;
    ASSERT_TRUE(copied.isValid());
    layOutStick(copied.path());
    EXPECT_EQ(classifyStickSync(copied.path()), StickSync::Complete);

    QTemporaryDir mid_copy;
    ASSERT_TRUE(mid_copy.isValid());
    layOutStick(mid_copy.path());
    ASSERT_TRUE(QFile::remove(
        mid_copy.path() +
        "/September_13_2026/Acme_HQ/Mission_101400/thumbsync_manifest.json"));
    EXPECT_EQ(classifyStickSync(mid_copy.path()), StickSync::Incomplete);

    QTemporaryDir orphans;
    ASSERT_TRUE(orphans.isValid());
    writeFile(orphans.path() +
                  "/September_13_2026/Acme_HQ/Section_1_101500/a.bin",
              "x");
    EXPECT_EQ(classifyStickSync(orphans.path()), StickSync::Incomplete);
}
