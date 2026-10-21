/**
 * @file upload_thumb_drive_tests.cpp
 * @brief The thumb-drive upload path's pure pieces: the local data-root
 *        walk (`UploadStateProbe::scanLocalDataRoot`), the script
 *        resolver, and the /proc/mounts device → mount-point lookup.
 */

#include "offload_status.hpp"
#include "thumb_drive_watcher.hpp"
#include "upload_open_policy.hpp"
#include "upload_runner.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDate>
#include <QElapsedTimer>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTime>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

using f2c_cpp::OffloadCopyState;
using f2c_cpp::OffloadSnapshot;
using f2c_cpp::StickSync;
using f2c_cpp::ThumbDriveWatcher;
using f2c_cpp::UploadRunner;
using f2c_cpp::UploadStateProbe;
using f2c_cpp::CloudCheck;
using f2c_cpp::UploadBuildingRow;
using f2c_cpp::UploadRowBadge;
using f2c_cpp::UploadStatus;
using f2c_cpp::UploadTarget;
using f2c_cpp::cloudCheckTargets;
using f2c_cpp::fileSha256Hex;
using f2c_cpp::readCloudVerified;
using f2c_cpp::settleCloudVerified;
using f2c_cpp::writeCloudVerified;
using f2c_cpp::groupUploadTargets;
using f2c_cpp::uploadTargetsForRow;
using f2c_cpp::classifyStickSync;
using f2c_cpp::parseOffloadStatusJson;
using f2c_cpp::ThumbCopyInputs;
using f2c_cpp::thumbCopyIsReady;
using f2c_cpp::thumbCopyStaleReportNote;
using f2c_cpp::uploaderArguments;
using f2c_cpp::UploadOpenBlock;
using f2c_cpp::UploadOpenInputs;
using f2c_cpp::uploadOpenBlock;

TEST(UploadOpenPolicy, RunningUploadReopensDespiteEveryOtherBlock) {
    UploadOpenInputs in;
    in.uploading = true;
    in.scan_active = true;
    in.copy_ready = false;
    in.profile_valid = false;
    EXPECT_EQ(uploadOpenBlock(in), UploadOpenBlock::None);
}

TEST(UploadOpenPolicy, NoticeIsNotAnInputSoAnActiveScanIsRefused) {
    // A held notice (drive removed, failed) is deliberately not a flag.
    // Without a running upload, a scan blocks.
    UploadOpenInputs in;
    in.scan_active = true;
    in.copy_ready = true;
    in.profile_valid = true;
    EXPECT_EQ(uploadOpenBlock(in), UploadOpenBlock::ScanActive);
}

TEST(UploadOpenPolicy, IncompleteCopyBlocksAFreshUpload) {
    UploadOpenInputs in;
    in.copy_ready = false;
    in.profile_valid = true;
    EXPECT_EQ(uploadOpenBlock(in), UploadOpenBlock::CopyNotReady);
}

TEST(UploadOpenPolicy, MissingProfileBlocks) {
    UploadOpenInputs in;
    in.copy_ready = true;
    in.profile_valid = false;
    EXPECT_EQ(uploadOpenBlock(in), UploadOpenBlock::ProfileInvalid);
}

TEST(UploadOpenPolicy, ScanBeatsCopyAndCopyBeatsProfile) {
    UploadOpenInputs scan;
    scan.scan_active = true;
    scan.copy_ready = false;
    scan.profile_valid = false;
    EXPECT_EQ(uploadOpenBlock(scan), UploadOpenBlock::ScanActive);

    UploadOpenInputs copy;
    copy.copy_ready = false;
    copy.profile_valid = false;
    EXPECT_EQ(uploadOpenBlock(copy), UploadOpenBlock::CopyNotReady);
}

TEST(UploadOpenPolicy, SkippedProfileReadStillChecksScanAndCopy) {
    // The open dialog passes profile_valid=true instead of re-reading
    // robots.json on every refresh. The scan and copy checks still run.
    UploadOpenInputs scan;
    scan.scan_active = true;
    scan.copy_ready = true;
    scan.profile_valid = true;
    EXPECT_EQ(uploadOpenBlock(scan), UploadOpenBlock::ScanActive);

    UploadOpenInputs copy;
    copy.copy_ready = false;
    copy.profile_valid = true;
    EXPECT_EQ(uploadOpenBlock(copy), UploadOpenBlock::CopyNotReady);
}

TEST(UploadOpenPolicy, ReadyInputsOpen) {
    UploadOpenInputs in;
    in.copy_ready = true;
    in.profile_valid = true;
    EXPECT_EQ(uploadOpenBlock(in), UploadOpenBlock::None);
}

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
    // Atomic-write leftovers are not data. A capture file that happens
    // to end in .tmp still is.
    writeFile(b + "/Section_2_113000/upload_state.json.tmp", "");
    writeFile(b + "/Section_2_113000/manifest.json.tmp", QByteArray(4, 't'));
    writeFile(b + "/Section_2_113000/notes.tmp", QByteArray(3, 'n'));

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
    // Bookkeeping sentinels (including the atomic-write .tmp siblings)
    // are not data. notes.tmp is a capture file and still counts.
    EXPECT_EQ(s2->total_files, 4);
}

TEST(UploadThumbDrive, ManifestBesideStateIsNotConfirmed) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString section = tmp.path() + "/September_13_2026/Acme_HQ/Section_3_120000";
    writeFile(section + "/a.bin", QByteArray(8, 'a'));
    writeFile(section + "/manifest.json",
              R"({"files":[{"relpath":"a.bin","size_bytes":8}]})");
    writeFile(section + "/upload_state.json",
              R"({"completed":["a.bin"],"uploaded":{"a.bin":{"size":8,"md5":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}}})");

    const QList<UploadTarget> all = UploadStateProbe::scanLocalDataRoot(tmp.path());
    const UploadTarget* s3 = find(all, "Section_3_120000");
    ASSERT_NE(s3, nullptr);
    EXPECT_EQ(s3->status, UploadStatus::Partial);
    EXPECT_EQ(s3->completed_files, 1);
    EXPECT_EQ(s3->total_files, 1);
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

TEST(UploadThumbDrive, MountPointRequiresARealMount) {
    const QString mounts =
        "/dev/sdb1 /media/ops/RDATA_EXT exfat rw 0 0\n"
        "/dev/sdc1 /media/ops/New\\040Volume vfat rw 0 0\n";
    EXPECT_TRUE(ThumbDriveWatcher::isMountPoint("/media/ops/RDATA_EXT", mounts));
    EXPECT_TRUE(ThumbDriveWatcher::isMountPoint("/media/ops/New Volume", mounts));
    EXPECT_TRUE(ThumbDriveWatcher::isMountPoint("/media/ops/RDATA_EXT/", mounts));
    EXPECT_FALSE(ThumbDriveWatcher::isMountPoint("/media/ops/RDATA_EXT/extra", mounts));
    EXPECT_FALSE(ThumbDriveWatcher::isMountPoint("/tmp/RDATA_EXT", mounts));
    EXPECT_FALSE(ThumbDriveWatcher::isMountPoint("", mounts));
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

TEST(UploadThumbDrive, ForceArgOnlyWhenRequested) {
    const QStringList plain =
        uploaderArguments("s.py", "/data", "R", "run/a", false);
    EXPECT_EQ(plain, (QStringList{"-u", "s.py", "/data", "R", "run/a"}));
    const QStringList forced =
        uploaderArguments("s.py", "/data", "R", "run/a", true);
    EXPECT_EQ(forced.mid(0, 5), plain);
    EXPECT_EQ(forced.last(), "--force");
}

TEST(UploadThumbDrive, LocalRunnerPassesForceOnlyForThatTarget) {
    int argc = 1;
    char arg0[] = "upload_thumb_drive_tests";
    char* argv[] = {arg0, nullptr};
    QCoreApplication app(argc, argv);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    layOutStick(tmp.path());
    const QList<UploadTarget> all = UploadStateProbe::scanLocalDataRoot(tmp.path());
    const UploadTarget* s1 = find(all, "Section_1_101500");
    const UploadTarget* s2 = find(all, "Section_2_113000");
    ASSERT_NE(s1, nullptr);
    ASSERT_NE(s2, nullptr);
    UploadTarget forced = *s1;
    forced.force_reupload = true;
    UploadTarget plain = *s2;

    const QString stub = tmp.path() + "/stub_force.py";
    writeFile(stub, R"PY(
import sys
flag = "yes" if "--force" in sys.argv else "no"
print(f"force={flag} {sys.argv[3]}", flush=True)
print("Upload complete: {}", flush=True)
)PY");

    UploadRunner runner;
    runner.setLocalScriptPath(stub);
    runner.setCloudAuth("https://api.example", "c", "t");
    runner.setRobotId("Roofus#0001");
    runner.setQueue({forced, plain});

    QStringList logs;
    QEventLoop loop;
    QObject::connect(&runner, &UploadRunner::logLine,
                     [&](const QString& l) { logs << l; });
    QObject::connect(&runner, &UploadRunner::queueFinished,
                     [&](bool) { loop.quit(); });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    runner.start();
    loop.exec();

    const QString joined = logs.join("\n");
    EXPECT_TRUE(joined.contains(
        "force=yes September_13_2026/Acme_HQ/Section_1_101500"))
        << joined.toStdString();
    EXPECT_TRUE(joined.contains(
        "force=no September_13_2026/Acme_HQ/Section_2_113000"))
        << joined.toStdString();
    const int yes = joined.indexOf("force=yes");
    const int no = joined.indexOf("force=no");
    EXPECT_GT(no, yes);
}

TEST(UploadThumbDrive, RetryAfterForceDoesNotPassForceAgain) {
    int argc = 1;
    char arg0[] = "upload_thumb_drive_tests";
    char* argv[] = {arg0, nullptr};
    QCoreApplication app(argc, argv);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    layOutStick(tmp.path());
    const QList<UploadTarget> all = UploadStateProbe::scanLocalDataRoot(tmp.path());
    const UploadTarget* s1 = find(all, "Section_1_101500");
    ASSERT_NE(s1, nullptr);
    UploadTarget forced = *s1;
    forced.force_reupload = true;

    const QString stub = tmp.path() + "/stub_retry.py";
    writeFile(stub, R"PY(
import os, sys
root = sys.argv[1]
marker = os.path.join(root, "launches.txt")
n = int(open(marker).read()) if os.path.exists(marker) else 0
open(marker, "w").write(str(n + 1))
print("force=" + ("yes" if "--force" in sys.argv else "no"), flush=True)
if n == 0:
    print("Connection error: blip", flush=True)
    print("Auto-pausing. You can resume later.", flush=True)
else:
    print("Upload complete: {}", flush=True)
)PY");

    UploadRunner runner;
    runner.setLocalScriptPath(stub);
    runner.setCloudAuth("https://api.example", "c", "t");
    runner.setRobotId("Roofus#0001");
    runner.setQueue({forced});

    QStringList logs;
    bool finished = false;
    QEventLoop loop;
    QObject::connect(&runner, &UploadRunner::logLine,
                     [&](const QString& l) { logs << l; });
    QObject::connect(&runner, &UploadRunner::queueFinished, [&](bool) {
        finished = true;
        loop.quit();
    });
    QTimer::singleShot(15000, &loop, &QEventLoop::quit);
    runner.start();
    loop.exec();

    const QString joined = logs.join("\n");
    EXPECT_TRUE(finished) << joined.toStdString();
    EXPECT_TRUE(joined.contains("force=yes")) << joined.toStdString();
    EXPECT_TRUE(joined.contains("force=no")) << joined.toStdString();
    EXPECT_LT(joined.indexOf("force=yes"), joined.indexOf("force=no"));
}

// Quit during a retry wait must not sit out the 5 s backoff. requestPause
// is a no-op with no process, and cancelling from the retry signal used
// to free the timer and then start it.
TEST(UploadThumbDrive, CancelDuringRetryWaitFinishesImmediately) {
    int argc = 1;
    char arg0[] = "upload_thumb_drive_tests";
    char* argv[] = {arg0, nullptr};
    QCoreApplication app(argc, argv);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    layOutStick(tmp.path());
    const QList<UploadTarget> all = UploadStateProbe::scanLocalDataRoot(tmp.path());
    const UploadTarget* s1 = find(all, "Section_1_101500");
    ASSERT_NE(s1, nullptr);

    const QString stub = tmp.path() + "/stub_cancel_retry.py";
    writeFile(stub, R"PY(
import sys
print("Connection error: blip", flush=True)
print("Auto-pausing. You can resume later.", flush=True)
)PY");

    UploadRunner runner;
    runner.setLocalScriptPath(stub);
    runner.setCloudAuth("https://api.example", "c", "t");
    runner.setRobotId("Roofus#0001");
    runner.setQueue({*s1});

    bool saw_retry = false;
    bool finished = false;
    bool cancelled = false;
    qint64 cancel_ms = -1;
    QObject::connect(
        &runner, &UploadRunner::targetRetryScheduled,
        [&](const UploadTarget&, int, int, const QString&) {
            EXPECT_TRUE(runner.isBusy());
            EXPECT_TRUE(runner.isRetryScheduled());
            runner.requestPause();
            EXPECT_TRUE(runner.isBusy());
            EXPECT_TRUE(runner.isRetryScheduled());
            QElapsedTimer timer;
            timer.start();
            runner.requestCancel();
            cancel_ms = timer.elapsed();
            saw_retry = true;
        });
    QEventLoop loop;
    QObject::connect(&runner, &UploadRunner::queueFinished, [&](bool c) {
        finished = true;
        cancelled = c;
        loop.quit();
    });
    // First retry waits 5 s. If cancel does not land, this fires first.
    QTimer::singleShot(1500, &loop, &QEventLoop::quit);
    runner.start();
    loop.exec();

    EXPECT_TRUE(saw_retry);
    EXPECT_TRUE(finished);
    EXPECT_TRUE(cancelled);
    EXPECT_GE(cancel_ms, 0);
    EXPECT_LT(cancel_ms, 1500);
    EXPECT_FALSE(runner.isBusy());
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
    // Restored on exit: later tests in this binary launch python3.
    const QByteArray saved_path = qgetenv("PATH");
    struct RestorePath {
        QByteArray value;
        ~RestorePath() { qputenv("PATH", value); }
    } restore{saved_path};
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

namespace {

UploadTarget folder(const char* date, const char* slug, const char* section,
                    UploadStatus status, const QDateTime& when,
                    int done_files = 0, int total_files = 0) {
    UploadTarget target;
    target.date_folder = QString::fromLatin1(date);
    target.building_slug = QString::fromLatin1(slug);
    target.section_name = QString::fromLatin1(section);
    target.run_id = target.date_folder + QLatin1Char('/') + target.building_slug +
                    QLatin1Char('/') + target.section_name;
    target.building_name = target.building_slug;
    target.captured_at = when;
    target.status = status;
    target.completed_files = done_files;
    target.total_files = total_files;
    target.total_bytes = total_files;
    return target;
}

QDateTime at(int year, int month, int day, int hour, int minute) {
    return QDateTime(QDate(year, month, day), QTime(hour, minute));
}

const UploadBuildingRow* findRow(const QList<UploadBuildingRow>& rows, const QString& key) {
    for (const UploadBuildingRow& row : rows) {
        if (row.key == key) return &row;
    }
    return nullptr;
}

}  // namespace

TEST(UploadBuildingRows, OneRowPerBuildingAcrossDates) {
    QHash<QString, UploadTarget> targets;
    const UploadTarget oct_mission =
        folder("October_09_2026", "BNY_white_roof", "Mission_223126",
               UploadStatus::Done, at(2026, 10, 9, 22, 31));
    UploadTarget oct_section =
        folder("October_09_2026", "BNY_white_roof", "Section_1_223200",
               UploadStatus::Done, at(2026, 10, 9, 22, 32));
    oct_section.operator_name = QStringLiteral("andrew");
    UploadTarget dec_mission =
        folder("December_10_2026", "BNY_white_roof", "Mission_140000",
               UploadStatus::None, at(2026, 12, 10, 14, 0));
    dec_mission.operator_name = QStringLiteral("blake");
    dec_mission.total_bytes = 1800000;
    UploadTarget dec_section =
        folder("December_10_2026", "BNY_white_roof", "Section_1_142000",
               UploadStatus::None, at(2026, 12, 10, 14, 20), 0, 10);
    dec_section.building_name = QStringLiteral("BNY White Roof");
    const UploadTarget other =
        folder("October_09_2026", "BNY_white_roof_cliff1", "Section_1_103112",
               UploadStatus::None, at(2026, 10, 9, 22, 31), 0, 4);

    for (const UploadTarget& target :
         {oct_mission, oct_section, dec_mission, dec_section, other}) {
        targets.insert(target.run_id, target);
    }

    const QList<UploadBuildingRow> rows = groupUploadTargets(targets);
    ASSERT_EQ(rows.size(), 2);
    // Newest scan first. The December rescan is the newest for this slug.
    EXPECT_EQ(rows.at(0).key, "BNY_white_roof");
    EXPECT_EQ(rows.at(0).building_label, "BNY White Roof");
    EXPECT_EQ(rows.at(0).operator_name, "blake");
    EXPECT_EQ(rows.at(0).newest, at(2026, 12, 10, 14, 20));
    EXPECT_EQ(rows.at(0).badge, UploadRowBadge::Pending);
    EXPECT_EQ(rows.at(0).badge_text, "Pending");
    EXPECT_TRUE(rows.at(0).selectable);
    EXPECT_TRUE(rows.at(0).tooltip.contains("Section_1_142000"));
    EXPECT_TRUE(rows.at(0).tooltip.contains("Mission_223126"));
    EXPECT_EQ(rows.at(0).run_ids.size(), 4);
    EXPECT_GT(rows.at(0).total_bytes, dec_mission.total_bytes);

    const QList<UploadTarget> queued = uploadTargetsForRow(targets, rows.at(0));
    ASSERT_EQ(queued.size(), 2);
    EXPECT_EQ(queued.at(0).section_name, "Mission_140000");
    EXPECT_EQ(queued.at(1).section_name, "Section_1_142000");
    EXPECT_FALSE(queued.at(0).force_reupload);
    EXPECT_FALSE(queued.at(1).force_reupload);

    const UploadBuildingRow* cliff = findRow(rows, "BNY_white_roof_cliff1");
    ASSERT_NE(cliff, nullptr);
    EXPECT_EQ(cliff->run_ids.size(), 1);
}

TEST(UploadBuildingRows, PartialPercentIgnoresUploadedFolders) {
    QHash<QString, UploadTarget> targets;
    UploadTarget done =
        folder("October_09_2026", "Acme", "Section_1_100000",
               UploadStatus::Done, at(2026, 10, 9, 10, 0), 1000, 1000);
    done.total_bytes = 500;
    UploadTarget partial =
        folder("December_10_2026", "Acme", "Section_1_140000",
               UploadStatus::Partial, at(2026, 12, 10, 14, 0), 5, 10);
    partial.total_bytes = 50;
    for (const UploadTarget& target : {done, partial}) {
        targets.insert(target.run_id, target);
    }

    const QList<UploadBuildingRow> rows = groupUploadTargets(targets);
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows.at(0).badge, UploadRowBadge::Partial);
    EXPECT_EQ(rows.at(0).partial_percent, 50);
    EXPECT_EQ(rows.at(0).badge_text, "Partial · 50%");
    EXPECT_EQ(rows.at(0).total_bytes, 550);
    EXPECT_TRUE(rows.at(0).selectable);

    const QList<UploadTarget> queued = uploadTargetsForRow(targets, rows.at(0));
    ASSERT_EQ(queued.size(), 1);
    EXPECT_EQ(queued.at(0).section_name, "Section_1_140000");
}

TEST(UploadBuildingRows, MissingBeatsPartialAndForcesReupload) {
    QHash<QString, UploadTarget> targets;
    UploadTarget missing =
        folder("October_09_2026", "Acme", "Mission_100000",
               UploadStatus::Done, at(2026, 10, 9, 10, 0));
    missing.cloud_check = CloudCheck::Missing;
    missing.force_reupload = false;
    const UploadTarget partial =
        folder("October_09_2026", "Acme", "Section_1_103000",
               UploadStatus::Partial, at(2026, 10, 9, 10, 30), 1, 4);
    targets.insert(missing.run_id, missing);
    targets.insert(partial.run_id, partial);

    const QList<UploadBuildingRow> rows = groupUploadTargets(targets);
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows.at(0).badge, UploadRowBadge::Missing);
    EXPECT_EQ(rows.at(0).badge_text, "Missing in cloud");
    EXPECT_TRUE(rows.at(0).selectable);

    const QList<UploadTarget> queued = uploadTargetsForRow(targets, rows.at(0));
    ASSERT_EQ(queued.size(), 2);
    EXPECT_EQ(queued.at(0).section_name, "Mission_100000");
    EXPECT_TRUE(queued.at(0).force_reupload);
    EXPECT_EQ(queued.at(1).section_name, "Section_1_103000");
    EXPECT_FALSE(queued.at(1).force_reupload);
}

TEST(UploadBuildingRows, VerifiedOnlyWhenEveryFolderMatches) {
    QHash<QString, UploadTarget> targets;
    UploadTarget mission =
        folder("October_09_2026", "Acme", "Mission_100000",
               UploadStatus::Done, at(2026, 10, 9, 10, 0));
    UploadTarget section =
        folder("October_09_2026", "Acme", "Section_1_103000",
               UploadStatus::Done, at(2026, 10, 9, 10, 30));
    mission.cloud_check = CloudCheck::Verified;
    section.cloud_check = CloudCheck::Unchecked;
    targets.insert(mission.run_id, mission);
    targets.insert(section.run_id, section);

    QList<UploadBuildingRow> rows = groupUploadTargets(targets);
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows.at(0).badge, UploadRowBadge::Uploaded);
    EXPECT_FALSE(rows.at(0).selectable);
    EXPECT_TRUE(uploadTargetsForRow(targets, rows.at(0)).isEmpty());

    targets[section.run_id].cloud_check = CloudCheck::Verified;
    rows = groupUploadTargets(targets);
    EXPECT_EQ(rows.at(0).badge, UploadRowBadge::Verified);
    EXPECT_EQ(rows.at(0).badge_text, "Uploaded · Verified");
    EXPECT_FALSE(rows.at(0).selectable);

    targets[section.run_id].cloud_check = CloudCheck::Checking;
    rows = groupUploadTargets(targets);
    EXPECT_EQ(rows.at(0).badge, UploadRowBadge::Checking);
    EXPECT_FALSE(rows.at(0).selectable);
}

TEST(UploadBuildingRows, UploadOrderKeepsLaterVisitBehindItsPredecessor) {
    QHash<QString, UploadTarget> targets;
    const UploadTarget first_mission =
        folder("October_09_2026", "Acme", "Mission_100000",
               UploadStatus::None, at(2026, 10, 9, 10, 0));
    const UploadTarget first_section =
        folder("October_09_2026", "Acme", "Section_1_103000",
               UploadStatus::None, at(2026, 10, 9, 10, 30));
    const UploadTarget second_mission =
        folder("October_09_2026", "Acme", "Mission_140000",
               UploadStatus::None, at(2026, 10, 9, 14, 0));
    const UploadTarget second_section =
        folder("October_09_2026", "Acme", "Section_2_143000",
               UploadStatus::None, at(2026, 10, 9, 14, 30));
    // Same minute: mission still precedes the section.
    const UploadTarget tied_mission =
        folder("November_01_2026", "Acme", "Mission_090000",
               UploadStatus::None, at(2026, 11, 1, 9, 0));
    const UploadTarget tied_section =
        folder("November_01_2026", "Acme", "Section_1_090000",
               UploadStatus::None, at(2026, 11, 1, 9, 0));
    for (const UploadTarget& target : {second_section, first_section, tied_section,
                                      second_mission, tied_mission, first_mission}) {
        targets.insert(target.run_id, target);
    }

    const QList<UploadBuildingRow> rows = groupUploadTargets(targets);
    ASSERT_EQ(rows.size(), 1);
    const QStringList& ids = rows.at(0).run_ids;
    ASSERT_EQ(ids.size(), 6);
    EXPECT_TRUE(ids.at(0).endsWith("Mission_100000"));
    EXPECT_TRUE(ids.at(1).endsWith("Section_1_103000"));
    EXPECT_TRUE(ids.at(2).endsWith("Mission_140000"));
    EXPECT_TRUE(ids.at(3).endsWith("Section_2_143000"));
    EXPECT_TRUE(ids.at(4).endsWith("Mission_090000"));
    EXPECT_TRUE(ids.at(5).endsWith("Section_1_090000"));
}

TEST(UploadBuildingRows, EmptySlugDoesNotMerge) {
    QHash<QString, UploadTarget> targets;
    UploadTarget a = folder("October_09_2026", "", "Section_1_100000",
                            UploadStatus::None, at(2026, 10, 9, 10, 0));
    UploadTarget b = folder("October_09_2026", "", "Section_2_110000",
                            UploadStatus::None, at(2026, 10, 9, 11, 0));
    a.building_slug.clear();
    b.building_slug.clear();
    a.run_id = QStringLiteral("October_09_2026//Section_1_100000");
    b.run_id = QStringLiteral("October_09_2026//Section_2_110000");
    targets.insert(a.run_id, a);
    targets.insert(b.run_id, b);

    const QList<UploadBuildingRow> rows = groupUploadTargets(targets);
    EXPECT_EQ(rows.size(), 2);

    UploadTarget blank;
    targets.insert(QString(), blank);
    EXPECT_EQ(groupUploadTargets(targets).size(), 2);
}

TEST(UploadBuildingRows, CloudCheckReachesEveryFinishedFolderOldestFirst) {
    QHash<QString, UploadTarget> targets;
    QStringList expected;
    // Twelve finished folders, oldest first. Past the old cap of ten.
    // One is already proved, and one pending folder is not finished.
    for (int day = 1; day <= 12; ++day) {
        UploadTarget target = folder("January_01_2026", "site", "Section_1_120000",
                                     UploadStatus::Done, at(2026, 1, day, 12, 0));
        target.building_slug = QStringLiteral("b%1").arg(day);
        target.run_id = QStringLiteral("January_%1_2026/b%1/Section_1_120000").arg(day);
        target.date_folder = QStringLiteral("January_%1_2026").arg(day);
        if (day == 4) target.cloud_check = CloudCheck::Verified;
        targets.insert(target.run_id, target);
        if (day != 4) expected << target.run_id;
    }
    UploadTarget pending = folder("January_01_2026", "pending", "Section_1_120000",
                                  UploadStatus::None, at(2026, 1, 2, 8, 0));
    pending.run_id = QStringLiteral("January_02_2026/pending/Section_1_120000");
    targets.insert(pending.run_id, pending);

    // Same minute: the mission folder is checked before the section.
    UploadTarget mission = folder("March_01_2026", "tied", "Mission_090000",
                                  UploadStatus::Done, at(2026, 3, 1, 9, 0));
    UploadTarget section = folder("March_01_2026", "tied", "Section_1_090000",
                                  UploadStatus::Done, at(2026, 3, 1, 9, 0));
    targets.insert(mission.run_id, mission);
    targets.insert(section.run_id, section);
    expected << mission.run_id << section.run_id;

    const QList<UploadTarget> checked = cloudCheckTargets(targets);
    QStringList ids;
    for (const UploadTarget& target : checked) ids << target.run_id;
    EXPECT_EQ(ids, expected);
    EXPECT_GT(ids.size(), 10);
}

TEST(CloudVerified, MatchWritesTheManifestHashAndMissingDeletesIt) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString folder = tmp.path() + "/section";
    const QByteArray manifest = QByteArray("{\"files\":[{\"relpath\":\"a.bin\"}]}");
    writeFile(folder + "/manifest.json", manifest);
    writeFile(folder + "/cloud_verified.json.tmp", "torn");

    ASSERT_TRUE(settleCloudVerified(folder, true, "client", "Roofus#0001", 3));
    EXPECT_FALSE(QFileInfo::exists(folder + "/cloud_verified.json.tmp"));

    const QByteArray expect = QCryptographicHash::hash(
        manifest, QCryptographicHash::Sha256).toHex();
    EXPECT_EQ(fileSha256Hex(folder + "/manifest.json"), expect);

    const f2c_cpp::CloudVerifiedProof proof = readCloudVerified(folder);
    EXPECT_TRUE(proof.valid);
    EXPECT_EQ(proof.manifest_sha256, QString::fromLatin1(expect));

    QFile written(folder + "/cloud_verified.json");
    ASSERT_TRUE(written.open(QIODevice::ReadOnly));
    const QJsonObject obj = QJsonDocument::fromJson(written.readAll()).object();
    written.close();
    EXPECT_EQ(obj.value(QStringLiteral("schema")).toInt(), 1);
    EXPECT_EQ(obj.value(QStringLiteral("manifest_sha256")).toString(),
              QString::fromLatin1(expect));
    EXPECT_EQ(obj.value(QStringLiteral("client_id")).toString(), "client");
    EXPECT_EQ(obj.value(QStringLiteral("robot_id")).toString(), "Roofus#0001");
    EXPECT_EQ(obj.value(QStringLiteral("files")).toInt(), 3);

    // The robot lowercases the stored hash. Upper case still counts.
    QJsonObject upper = obj;
    upper.insert(QStringLiteral("manifest_sha256"),
                 QString::fromLatin1(expect).toUpper());
    writeFile(folder + "/cloud_verified.json",
              QJsonDocument(upper).toJson(QJsonDocument::Compact));
    EXPECT_TRUE(readCloudVerified(folder).valid);

    upper.insert(QStringLiteral("manifest_sha256"), QString(63, QLatin1Char('a')));
    writeFile(folder + "/cloud_verified.json",
              QJsonDocument(upper).toJson(QJsonDocument::Compact));
    EXPECT_FALSE(readCloudVerified(folder).valid);

    ASSERT_TRUE(writeCloudVerified(folder, "client", "Roofus#0001", 3));
    writeFile(folder + "/manifest.json", manifest + " ");
    EXPECT_FALSE(readCloudVerified(folder).valid);

    ASSERT_TRUE(writeCloudVerified(folder, "client", "Roofus#0001", 3));
    writeFile(folder + "/upload_state.json", "{}");
    EXPECT_FALSE(readCloudVerified(folder).valid);
    QFile::remove(folder + "/upload_state.json");

    ASSERT_TRUE(settleCloudVerified(folder, false, QString(), QString(), 0));
    EXPECT_FALSE(QFileInfo::exists(folder + "/cloud_verified.json"));
    EXPECT_FALSE(QFileInfo::exists(folder + "/cloud_verified.json.tmp"));
    EXPECT_FALSE(readCloudVerified(folder).valid);
}

TEST(CloudVerified, ProbeMarksAValidProofVerifiedAndIgnoresAChangedManifest) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString folder =
        tmp.path() + "/September_13_2026/Acme_HQ/Mission_101400";
    writeFile(folder + "/a.bin", "data");
    writeFile(folder + "/manifest.json",
              QByteArray("{\"files\":[{\"relpath\":\"a.bin\"}]}"));
    ASSERT_TRUE(writeCloudVerified(folder, "client", "Roofus#0001", 1));
    writeFile(folder + "/cloud_verified.json.tmp", "torn");

    const QList<UploadTarget> proved =
        UploadStateProbe::scanLocalDataRoot(tmp.path());
    const UploadTarget* row = find(proved, "Mission_101400");
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->status, UploadStatus::Done);
    EXPECT_EQ(row->cloud_check, CloudCheck::Verified);
    // a.bin only. manifest.json, cloud_verified.json and its .tmp are
    // bookkeeping, so they stay out of the file count.
    EXPECT_EQ(row->total_files, 1);

    writeFile(folder + "/manifest.json",
              QByteArray("{\"files\":[{\"relpath\":\"a.bin\"},{\"relpath\":\"b.bin\"}]}"));
    const QList<UploadTarget> stale =
        UploadStateProbe::scanLocalDataRoot(tmp.path());
    const UploadTarget* changed = find(stale, "Mission_101400");
    ASSERT_NE(changed, nullptr);
    EXPECT_EQ(changed->status, UploadStatus::Done);
    EXPECT_EQ(changed->cloud_check, CloudCheck::Unchecked);
    EXPECT_EQ(changed->total_files, 1);
}

TEST(OffloadStatus, StickFullIsIncompleteAndKeepsTheJobError) {
    // Shape rdata_offload._write_status writes: no top-level error.
    const OffloadSnapshot snap = parseOffloadStatusJson(
        R"({"state":"stick_full","queued":1,"mission":null,"jobs":{"a":{"state":"stick_full","error":"stick full: need 12 GB, 3 GB free after deleting cloud-confirmed missions"}}})");
    ASSERT_TRUE(snap.parse_ok);
    EXPECT_EQ(snap.state, OffloadCopyState::StickFull);
    EXPECT_TRUE(snap.copyIncomplete());
    EXPECT_EQ(snap.error,
              "stick full: need 12 GB, 3 GB free after deleting "
              "cloud-confirmed missions");
}

TEST(OffloadStatus, UnpluggedFullStickStaysStickFull) {
    // Stick pulled for the laptop: job flips to waiting_for_drive but
    // keeps the stick-full error. Upload must stay reachable.
    const OffloadSnapshot snap = parseOffloadStatusJson(
        R"({"state":"waiting_for_drive","queued":1,"mission":null,"jobs":{"a":{"state":"waiting_for_drive","error":"stick full: need 12 GB, 3 GB free"}}})");
    ASSERT_TRUE(snap.parse_ok);
    EXPECT_EQ(snap.state, OffloadCopyState::StickFull);
    EXPECT_EQ(snap.error, "stick full: need 12 GB, 3 GB free");

    const OffloadSnapshot plain = parseOffloadStatusJson(
        R"({"state":"waiting_for_drive","queued":1,"jobs":{"a":{"state":"waiting_for_drive","error":null}}})");
    EXPECT_EQ(plain.state, OffloadCopyState::WaitingForDrive);

    const OffloadSnapshot copying = parseOffloadStatusJson(
        R"({"state":"copying","queued":2,"mission":"a","jobs":{"a":{"state":"copying"},"b":{"state":"stick_full","error":"stick full: x"}}})");
    EXPECT_EQ(copying.state, OffloadCopyState::Copying);
}

ThumbCopyInputs copyInputs(bool reachable, bool incomplete, bool stick_full,
                           StickSync stick) {
    ThumbCopyInputs in;
    in.robot_reachable = reachable;
    in.robot_copy_incomplete = incomplete;
    in.stick_full = stick_full;
    in.stick = stick;
    return in;
}

TEST(ThumbCopyGate, UnreachableRobotWithACompleteStickCanUpload) {
    const ThumbCopyInputs in =
        copyInputs(false, true, false, StickSync::Complete);
    EXPECT_TRUE(thumbCopyIsReady(in));
    EXPECT_FALSE(thumbCopyStaleReportNote(in).isEmpty());
}

TEST(ThumbCopyGate, UnreachableRobotWithAnIncompleteOrMissingStickBlocks) {
    for (const StickSync stick :
         {StickSync::Incomplete, StickSync::Absent, StickSync::Empty}) {
        const ThumbCopyInputs in = copyInputs(false, true, false, stick);
        EXPECT_FALSE(thumbCopyIsReady(in)) << static_cast<int>(stick);
        EXPECT_TRUE(thumbCopyStaleReportNote(in).isEmpty());
    }
}

TEST(ThumbCopyGate, ReachableRobotStillCopyingBlocksACompleteStick) {
    const ThumbCopyInputs in = copyInputs(true, true, false, StickSync::Complete);
    EXPECT_FALSE(thumbCopyIsReady(in));
    EXPECT_TRUE(thumbCopyStaleReportNote(in).isEmpty());
}

TEST(ThumbCopyGate, FullStickStaysUploadableWithoutTheStaleNote) {
    const ThumbCopyInputs in = copyInputs(false, true, true, StickSync::Absent);
    EXPECT_TRUE(thumbCopyIsReady(in));
    EXPECT_TRUE(thumbCopyStaleReportNote(in).isEmpty());
}

TEST(ThumbCopyGate, FinishedReportNeedsNoNote) {
    const ThumbCopyInputs in = copyInputs(false, false, false, StickSync::Complete);
    EXPECT_TRUE(thumbCopyIsReady(in));
    EXPECT_TRUE(thumbCopyStaleReportNote(in).isEmpty());
}

TEST(OffloadStatus, ByteFieldsAreOptional) {
    const OffloadSnapshot missing = parseOffloadStatusJson(
        R"({"state":"queued","queued":2})");
    ASSERT_TRUE(missing.parse_ok);
    EXPECT_FALSE(missing.robot_free_bytes.has_value());
    EXPECT_FALSE(missing.robot_floor_bytes.has_value());
    EXPECT_EQ(scansAtRiskOfDeletion(missing), 0);

    const OffloadSnapshot nulls = parseOffloadStatusJson(
        R"({"state":"queued","queued":2,"robot_free_bytes":null,"robot_floor_bytes":null})");
    EXPECT_FALSE(nulls.robot_free_bytes.has_value());
    EXPECT_EQ(scansAtRiskOfDeletion(nulls), 0);
}

TEST(OffloadStatus, UncopiedScansAtRiskOnlyUnderTwiceTheFloor) {
    const OffloadSnapshot low = parseOffloadStatusJson(
        R"({"state":"queued","queued":3,"robot_free_bytes":199,"robot_floor_bytes":100})");
    EXPECT_EQ(low.robot_free_bytes, 199);
    EXPECT_EQ(low.robot_floor_bytes, 100);
    EXPECT_EQ(scansAtRiskOfDeletion(low), 3);

    const OffloadSnapshot at_twice = parseOffloadStatusJson(
        R"({"state":"queued","queued":3,"robot_free_bytes":200,"robot_floor_bytes":100})");
    EXPECT_EQ(scansAtRiskOfDeletion(at_twice), 0);

    const OffloadSnapshot roomy = parseOffloadStatusJson(
        R"({"state":"idle","queued":3,"robot_free_bytes":500,"robot_floor_bytes":100})");
    EXPECT_EQ(scansAtRiskOfDeletion(roomy), 0);

    const OffloadSnapshot nothing_queued = parseOffloadStatusJson(
        R"({"state":"idle","queued":0,"robot_free_bytes":10,"robot_floor_bytes":100})");
    EXPECT_EQ(scansAtRiskOfDeletion(nothing_queued), 0);

    const OffloadSnapshot bad = parseOffloadStatusJson(QByteArray("not json"));
    EXPECT_FALSE(bad.parse_ok);
    EXPECT_EQ(scansAtRiskOfDeletion(bad), 0);
}

// ------------------------------------------- OCU logs ride with the mission

namespace {

struct OcuLogRig {
    QTemporaryDir tmp;
    QString store;
    UploadTarget mission;
    QString stub;
    QString ran_marker;

    explicit OcuLogRig(const QByteArray& stub_body = QByteArray()) {
        store = tmp.path() + "/store";
        QDir().mkpath(store);
        mission.date_folder = "September_13_2026";
        mission.building_slug = "Acme_HQ";
        mission.section_name = "Mission_120000";
        mission.run_id = "September_13_2026/Acme_HQ/Mission_120000";
        mission.data_path = tmp.path() + "/stick/" + mission.run_id;
        writeFile(mission.data_path + "/mission_config.json",
                  R"({"ocu_logs": ["mission_20260913_115900"]})");
        writeFile(mission.data_path + "/thumbsync_manifest.json", "{}");
        writeFile(store + "/mission_20260913_115900.log", "ocu log body");
        writeFile(store + "/mission_20260913_115900.laptop_ros_logs.tar.gz",
                  QByteArray(64, 'z'));
        ran_marker = tmp.path() + "/script_ran";
        stub = tmp.path() + "/stub_mission.py";
        writeFile(stub, stub_body.isEmpty() ? QByteArray(R"PY(
import os, sys
root = sys.argv[1]
ocu = os.path.join(root, "logs", "ocu")
names = sorted(os.listdir(ocu)) if os.path.isdir(ocu) else []
open(os.environ["RAN_MARKER"], "w").write(",".join(names))
print("Upload complete: {}", flush=True)
)PY") : stub_body);
        qputenv("RAN_MARKER", ran_marker.toUtf8());
    }

    void configure(UploadRunner& runner) const {
        runner.setSource(f2c_cpp::UploadSource::ThumbDrive);
        runner.setLocalScriptPath(stub);
        runner.setCloudAuth("https://api.example", "c", "t");
        runner.setRobotId("Roofus#0003");
        runner.setOcuLogStoreDir(store);
        runner.setQueue({mission});
    }
};

QByteArray readAll(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

}  // namespace

TEST(UploadOcuLogs, MissionUploadCarriesItsOcuLogsAndMarksThem) {
    int argc = 1;
    char arg0[] = "upload_thumb_drive_tests";
    char* argv[] = {arg0, nullptr};
    QCoreApplication app(argc, argv);
    OcuLogRig rig;
    UploadRunner runner;
    rig.configure(runner);
    QStringList logs;
    int completed = 0;
    QObject::connect(&runner, &UploadRunner::logLine,
                     [&](const QString& l) { logs << l; });
    QObject::connect(&runner, &UploadRunner::targetCompleted,
                     [&](const UploadTarget&) { ++completed; });
    QEventLoop loop;
    QObject::connect(&runner, &UploadRunner::queueFinished, [&](bool) { loop.quit(); });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    runner.start();
    loop.exec();

    EXPECT_EQ(completed, 1) << logs.join(" | ").toStdString();
    // The script saw both files already in place when it started.
    EXPECT_EQ(readAll(rig.ran_marker),
              "mission_20260913_115900.laptop_ros_logs.tar.gz,"
              "mission_20260913_115900.log");
    EXPECT_EQ(readAll(rig.mission.data_path + "/logs/ocu/mission_20260913_115900.log"),
              "ocu log body");
    EXPECT_TRUE(QFile::exists(rig.store + "/mission_20260913_115900.uploaded"));
    EXPECT_TRUE(logs.join(" | ").contains("OCU logs added")) << logs.join(" | ").toStdString();
}

TEST(UploadOcuLogs, AlreadyUploadedMissionGetsNothingAdded) {
    int argc = 1;
    char arg0[] = "upload_thumb_drive_tests";
    char* argv[] = {arg0, nullptr};
    QCoreApplication app(argc, argv);
    OcuLogRig rig;
    writeFile(rig.mission.data_path + "/manifest.json", "{}");
    UploadRunner runner;
    rig.configure(runner);
    QEventLoop loop;
    QObject::connect(&runner, &UploadRunner::queueFinished, [&](bool) { loop.quit(); });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    runner.start();
    loop.exec();
    EXPECT_EQ(readAll(rig.ran_marker), "");
    EXPECT_TRUE(QFile::exists(rig.ran_marker)) << "the upload itself still runs";
    EXPECT_FALSE(QFile::exists(rig.mission.data_path + "/logs"));
}

TEST(UploadOcuLogs, CancelWhileAttachingNeverStartsTheUploader) {
    int argc = 1;
    char arg0[] = "upload_thumb_drive_tests";
    char* argv[] = {arg0, nullptr};
    QCoreApplication app(argc, argv);
    OcuLogRig rig;
    UploadRunner runner;
    rig.configure(runner);
    bool finished = false, cancelled = false;
    int paused = 0;
    QObject::connect(&runner, &UploadRunner::targetPaused,
                     [&](const UploadTarget&, const QString&) { ++paused; });
    QObject::connect(&runner, &UploadRunner::queueFinished, [&](bool c) {
        finished = true;
        cancelled = c;
    });
    runner.start();           // attach is now on a worker thread
    EXPECT_TRUE(runner.isBusy());
    runner.requestCancel();   // before its result is delivered
    EXPECT_TRUE(finished);
    EXPECT_TRUE(cancelled);
    EXPECT_EQ(paused, 1);
    EXPECT_FALSE(runner.isBusy());
    // Let the worker's result arrive: it must be dropped.
    QEventLoop loop;
    QTimer::singleShot(1500, &loop, &QEventLoop::quit);
    loop.exec();
    EXPECT_FALSE(QFile::exists(rig.ran_marker));
    EXPECT_FALSE(runner.isBusy());
}

TEST(UploadOcuLogs, PauseWhileAttachingStopsBeforeTheUploaderAndResumes) {
    int argc = 1;
    char arg0[] = "upload_thumb_drive_tests";
    char* argv[] = {arg0, nullptr};
    QCoreApplication app(argc, argv);
    OcuLogRig rig;
    UploadRunner runner;
    rig.configure(runner);
    int paused = 0, completed = 0, finishes = 0;
    bool last_cancelled = true;
    QObject::connect(&runner, &UploadRunner::targetPaused,
                     [&](const UploadTarget&, const QString&) { ++paused; });
    QObject::connect(&runner, &UploadRunner::targetCompleted,
                     [&](const UploadTarget&) { ++completed; });
    QEventLoop loop;
    QObject::connect(&runner, &UploadRunner::queueFinished, [&](bool c) {
        ++finishes;
        last_cancelled = c;
        loop.quit();
    });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    runner.start();
    runner.requestPause();
    loop.exec();
    EXPECT_EQ(paused, 1);
    EXPECT_EQ(finishes, 1);
    EXPECT_FALSE(last_cancelled);
    EXPECT_FALSE(runner.isBusy());
    EXPECT_FALSE(QFile::exists(rig.ran_marker));

    // Upload again: same target, logs already there, script runs.
    QEventLoop again;
    QObject::connect(&runner, &UploadRunner::queueFinished, [&](bool) { again.quit(); });
    QTimer::singleShot(10000, &again, &QEventLoop::quit);
    runner.start();
    again.exec();
    EXPECT_EQ(completed, 1);
    EXPECT_TRUE(QFile::exists(rig.ran_marker));
}

TEST(UploadOcuLogs, MissingLogIsReportedAndTheUploadStillRuns) {
    int argc = 1;
    char arg0[] = "upload_thumb_drive_tests";
    char* argv[] = {arg0, nullptr};
    QCoreApplication app(argc, argv);
    OcuLogRig rig;
    QFile::remove(rig.store + "/mission_20260913_115900.log");
    QFile::remove(rig.store + "/mission_20260913_115900.laptop_ros_logs.tar.gz");
    UploadRunner runner;
    rig.configure(runner);
    QStringList logs;
    int completed = 0;
    QObject::connect(&runner, &UploadRunner::logLine,
                     [&](const QString& l) { logs << l; });
    QObject::connect(&runner, &UploadRunner::targetCompleted,
                     [&](const UploadTarget&) { ++completed; });
    QEventLoop loop;
    QObject::connect(&runner, &UploadRunner::queueFinished, [&](bool) { loop.quit(); });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    runner.start();
    loop.exec();
    EXPECT_EQ(completed, 1);
    EXPECT_TRUE(logs.join(" | ").contains("is not on this laptop")) << logs.join(" | ").toStdString();
    EXPECT_FALSE(QFile::exists(rig.store + "/mission_20260913_115900.uploaded"));
}

// ---------------------------------------------------------------------------
// Stick copy check: robot record vs. what is on the stick
// ---------------------------------------------------------------------------

namespace {

QByteArray sha256Hex(const QByteArray& bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

// A building whose Mission_120000 record covers itself and Section_1.
// `stick` maps relpath (from the building) to the bytes on the stick;
// `robot` maps the same keys to what the robot copied.
QString layOutRecordedBuilding(const QString& root,
                               const QHash<QString, QByteArray>& robot,
                               const QHash<QString, QByteArray>& stick) {
    const QString b = root + "/October_13_2026/Durafoam_ca";
    QJsonObject files;
    for (auto it = robot.cbegin(); it != robot.cend(); ++it) {
        QJsonObject rec;
        rec.insert("size", it.value().size());
        rec.insert("sha256", QString::fromLatin1(sha256Hex(it.value())));
        files.insert(it.key(), rec);
    }
    for (auto it = stick.cbegin(); it != stick.cend(); ++it) {
        writeFile(b + "/" + it.key(), it.value());
    }
    QJsonObject doc;
    doc.insert("schema", 2);
    doc.insert("sections", QJsonArray{QStringLiteral("Section_1")});
    doc.insert("files", files);
    writeFile(b + "/Mission_120000/thumbsync_manifest.json",
              QJsonDocument(doc).toJson());
    return b;
}

QStringList needsRecopyFiles(const QString& mission_dir) {
    QFile f(mission_dir + "/needs_recopy.json");
    if (!f.open(QIODevice::ReadOnly)) return {};
    QStringList out;
    for (const QJsonValue& v :
         QJsonDocument::fromJson(f.readAll()).object().value("files").toArray()) {
        out << v.toString();
    }
    return out;
}

}  // namespace

using f2c_cpp::StickCopyProblem;

TEST(StickCopyCheck, MatchingCopyHasNoProblem) {
    QTemporaryDir tmp;
    const QHash<QString, QByteArray> data{
        {"Mission_120000/mission_config.json", "{}"},
        {"Section_1/a.bin", "abc"},
        {"Section_1/Visual_data/f.jxl", QByteArray(500, 'f')},
    };
    const QString b = layOutRecordedBuilding(tmp.path(), data, data);
    const QList<UploadTarget> all = UploadStateProbe::scanLocalDataRoot(tmp.path());
    ASSERT_EQ(all.size(), 2);
    for (const UploadTarget& t : all) {
        EXPECT_EQ(t.copy_problem, StickCopyProblem::None) << t.section_name.toStdString();
    }
    EXPECT_FALSE(QFileInfo::exists(b + "/Mission_120000/needs_recopy.json"));
}

TEST(StickCopyCheck, ZeroByteFilesAreDamagedAndTheRobotIsAskedToRecopy) {
    QTemporaryDir tmp;
    const QHash<QString, QByteArray> robot{
        {"Mission_120000/mission_config.json", "{}"},
        {"Section_1/a.bin", "abc"},
        {"Section_1/Visual_data/f.jxl", QByteArray(500, 'f')},
        {"Section_1/rosbag/x.mcap.zstd", QByteArray(900, 'r')},
    };
    QHash<QString, QByteArray> stick = robot;
    // What the robot left when the stick was pulled before its cache
    // flushed: the entries exist, the data does not.
    stick["Section_1/Visual_data/f.jxl"] = QByteArray();
    stick["Section_1/rosbag/x.mcap.zstd"] = QByteArray();
    const QString b = layOutRecordedBuilding(tmp.path(), robot, stick);

    const QList<UploadTarget> all = UploadStateProbe::scanLocalDataRoot(tmp.path());
    const UploadTarget* section = find(all, "Section_1");
    ASSERT_NE(section, nullptr);
    EXPECT_EQ(section->copy_problem, StickCopyProblem::Damaged);
    EXPECT_EQ(section->copy_bad_files, 2);
    EXPECT_EQ(find(all, "Mission_120000")->copy_problem, StickCopyProblem::None);
    EXPECT_EQ(needsRecopyFiles(b + "/Mission_120000"),
              (QStringList{"Section_1/Visual_data/f.jxl",
                           "Section_1/rosbag/x.mcap.zstd"}));

    // The probe runs on every Refresh: an unchanged list is not rewritten.
    const QDateTime before =
        QFileInfo(b + "/Mission_120000/needs_recopy.json").lastModified();
    QThread::msleep(1100);
    UploadStateProbe::scanLocalDataRoot(tmp.path());
    EXPECT_EQ(QFileInfo(b + "/Mission_120000/needs_recopy.json").lastModified(), before);
}

TEST(StickCopyCheck, MissingFileIsDamaged) {
    QTemporaryDir tmp;
    const QHash<QString, QByteArray> robot{
        {"Section_1/a.bin", "abc"}, {"Section_1/b.bin", "de"}};
    QHash<QString, QByteArray> stick{{"Section_1/a.bin", "abc"}};
    layOutRecordedBuilding(tmp.path(), robot, stick);
    const QList<UploadTarget> all = UploadStateProbe::scanLocalDataRoot(tmp.path());
    EXPECT_EQ(find(all, "Section_1")->copy_problem, StickCopyProblem::Damaged);
    EXPECT_EQ(find(all, "Section_1")->copy_bad_files, 1);
}

TEST(StickCopyCheck, UnlistedFolderOnARecordedStickIsUnfinished) {
    QTemporaryDir tmp;
    const QHash<QString, QByteArray> data{{"Section_1/a.bin", "abc"}};
    const QString b = layOutRecordedBuilding(tmp.path(), data, data);
    // A second mission whose copy stopped before its record was written.
    writeFile(b + "/Mission_160000/mission_config.json", "{}");
    writeFile(b + "/Section_2_160000/a.bin", "zz");
    const QList<UploadTarget> all = UploadStateProbe::scanLocalDataRoot(tmp.path());
    EXPECT_EQ(find(all, "Mission_160000")->copy_problem, StickCopyProblem::Unfinished);
    EXPECT_EQ(find(all, "Section_2_160000")->copy_problem, StickCopyProblem::Unfinished);
    EXPECT_EQ(find(all, "Section_1")->copy_problem, StickCopyProblem::None);
}

TEST(StickCopyCheck, StickFromAnOlderRobotIsNotChecked) {
    QTemporaryDir tmp;
    layOutStick(tmp.path());  // record without a files map
    for (const UploadTarget& t : UploadStateProbe::scanLocalDataRoot(tmp.path())) {
        EXPECT_EQ(t.copy_problem, StickCopyProblem::None) << t.section_name.toStdString();
    }
}

TEST(StickCopyCheck, UnreadableRecordBlocksTheBuilding) {
    QTemporaryDir tmp;
    const QString b = tmp.path() + "/October_13_2026/Durafoam_ca";
    writeFile(b + "/Mission_120000/thumbsync_manifest.json", "");  // 0 bytes
    writeFile(b + "/Section_1/a.bin", "abc");
    const QList<UploadTarget> all = UploadStateProbe::scanLocalDataRoot(tmp.path());
    EXPECT_EQ(find(all, "Section_1")->copy_problem, StickCopyProblem::Damaged);
    EXPECT_EQ(find(all, "Mission_120000")->copy_problem, StickCopyProblem::Damaged);
}

TEST(StickCopyCheck, FinishedUploadIsNotChecked) {
    QTemporaryDir tmp;
    const QHash<QString, QByteArray> robot{{"Section_1/a.bin", "abc"}};
    const QHash<QString, QByteArray> stick{{"Section_1/a.bin", ""}};
    const QString b = layOutRecordedBuilding(tmp.path(), robot, stick);
    writeFile(b + "/Section_1/manifest.json", R"({"files":[]})");
    const QList<UploadTarget> all = UploadStateProbe::scanLocalDataRoot(tmp.path());
    EXPECT_EQ(find(all, "Section_1")->status, UploadStatus::Done);
    EXPECT_EQ(find(all, "Section_1")->copy_problem, StickCopyProblem::None);
}

TEST(UploadBuildingRows, DamagedFolderIsHeldBackAndTheRestStillUploads) {
    QHash<QString, UploadTarget> targets;
    auto add = [&](const QString& section, StickCopyProblem problem) {
        UploadTarget t;
        t.date_folder = "October_15_2026";
        t.building_slug = "1561_buckeye_dr";
        t.section_name = section;
        t.run_id = t.date_folder + "/" + t.building_slug + "/" + section;
        t.copy_problem = problem;
        targets.insert(t.run_id, t);
    };
    add("Mission_160735", StickCopyProblem::None);
    add("Section_7_160735_complete", StickCopyProblem::None);
    add("Mission_162137", StickCopyProblem::Damaged);
    add("Section_8_162137_partial", StickCopyProblem::Damaged);

    QList<UploadBuildingRow> rows = groupUploadTargets(targets);
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows.at(0).badge, UploadRowBadge::Pending);
    EXPECT_TRUE(rows.at(0).selectable);
    EXPECT_TRUE(rows.at(0).badge_text.contains("2 need ROOFUS"))
        << rows.at(0).badge_text.toStdString();
    const QList<UploadTarget> queued = uploadTargetsForRow(targets, rows.at(0));
    ASSERT_EQ(queued.size(), 2);
    for (const UploadTarget& t : queued) {
        EXPECT_EQ(t.copy_problem, StickCopyProblem::None);
    }

    // Once the good folders are uploaded only the damaged ones remain:
    // the row tells the operator what to do and cannot be ticked.
    targets[queued.at(0).run_id].status = UploadStatus::Done;
    targets[queued.at(1).run_id].status = UploadStatus::Done;
    rows = groupUploadTargets(targets);
    EXPECT_EQ(rows.at(0).badge, UploadRowBadge::NeedsRobot);
    EXPECT_FALSE(rows.at(0).selectable);
    EXPECT_TRUE(rows.at(0).badge_text.contains("plug drive into ROOFUS"));
    EXPECT_TRUE(uploadTargetsForRow(targets, rows.at(0)).isEmpty());
}

TEST(OffloadStatus, StickStillBeingWrittenIsIncomplete) {
    OffloadSnapshot s = parseOffloadStatusJson(
        R"({"state":"idle","queued":0,"jobs":{},"stick_safe_to_remove":false})");
    ASSERT_TRUE(s.parse_ok);
    EXPECT_TRUE(s.copyIncomplete());
    s = parseOffloadStatusJson(
        R"({"state":"idle","queued":0,"jobs":{},"stick_safe_to_remove":true})");
    EXPECT_FALSE(s.copyIncomplete());
    // Older robots do not send the field.
    s = parseOffloadStatusJson(R"({"state":"idle","queued":0,"jobs":{}})");
    EXPECT_FALSE(s.stick_safe_to_remove.has_value());
    EXPECT_FALSE(s.copyIncomplete());
}

TEST(UploadRunnerPolicy, OnlyQueueWideErrorsStopTheQueue) {
    EXPECT_TRUE(UploadRunner::isQueueStoppingError(
        "Unexpected error: Cloud refused /presign (HTTP 403): bad token"));
    EXPECT_TRUE(UploadRunner::isQueueStoppingError(
        "Unexpected error: Cloud refused /complete (HTTP 401)"));
    EXPECT_TRUE(UploadRunner::isQueueStoppingError(
        "Unexpected error: data_root does not exist: /media/x"));
    EXPECT_FALSE(UploadRunner::isQueueStoppingError(
        "Unexpected error: Cloud refused /presign (HTTP 400): bad relpath"));
    EXPECT_FALSE(UploadRunner::isQueueStoppingError(
        "Unexpected error: Stick copy of a.bin does not match the robot (1 file)."));
    EXPECT_FALSE(UploadRunner::isQueueStoppingError(
        "Unexpected error: File exceeds 5GB single-PUT limit: big.bin"));
}

namespace {

struct QueueOutcome {
    QStringList failures;
    QStringList started;
    int completed = 0;
    bool stopped_early = false;
};

QueueOutcome runQueueWithStub(const QString& stub_body) {
    QueueOutcome out;
    QTemporaryDir tmp;
    layOutStick(tmp.path());
    const QList<UploadTarget> all = UploadStateProbe::scanLocalDataRoot(tmp.path());
    const UploadTarget* s1 = find(all, "Section_1_101500");
    const UploadTarget* s2 = find(all, "Section_2_113000");
    const QString stub = tmp.path() + "/stub_uploader.py";
    writeFile(stub, stub_body.toUtf8());

    UploadRunner runner;
    runner.setSource(f2c_cpp::UploadSource::ThumbDrive);
    runner.setLocalScriptPath(stub);
    runner.setCloudAuth("https://api.example/", "client_x", "tok_y");
    runner.setRobotId("Roofus#0001");
    runner.setQueue({*s1, *s2});
    QObject::connect(&runner, &UploadRunner::targetStarted,
                     [&](int, int, const UploadTarget& t) { out.started << t.section_name; });
    QObject::connect(&runner, &UploadRunner::targetFailed,
                     [&](const UploadTarget& t, const QString&) { out.failures << t.section_name; });
    QObject::connect(&runner, &UploadRunner::targetCompleted,
                     [&](const UploadTarget&) { ++out.completed; });
    QEventLoop loop;
    QObject::connect(&runner, &UploadRunner::queueFinished, [&](bool) { loop.quit(); });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    runner.start();
    loop.exec();
    out.stopped_early = runner.stoppedEarly();
    return out;
}

}  // namespace

TEST(UploadThumbDrive, OneDamagedFolderDoesNotStopTheRestOfTheQueue) {
    int argc = 1;
    char arg0[] = "upload_thumb_drive_tests";
    char* argv[] = {arg0, nullptr};
    QCoreApplication app(argc, argv);
    const QueueOutcome out = runQueueWithStub(R"PY(
import sys
if sys.argv[3].endswith("Section_1_101500"):
    print("Unexpected error: Stick copy of a.bin does not match the robot (1 file).", flush=True)
    sys.exit(1)
print("Upload complete: {}", flush=True)
)PY");
    EXPECT_EQ(out.started, QStringList({"Section_1_101500", "Section_2_113000"}));
    EXPECT_EQ(out.failures, QStringList({"Section_1_101500"}));
    EXPECT_EQ(out.completed, 1);
    EXPECT_FALSE(out.stopped_early);
}

TEST(UploadThumbDrive, RefusedCredentialsStopTheQueue) {
    int argc = 1;
    char arg0[] = "upload_thumb_drive_tests";
    char* argv[] = {arg0, nullptr};
    QCoreApplication app(argc, argv);
    const QueueOutcome out = runQueueWithStub(R"PY(
import sys
print("Unexpected error: Cloud refused /presign (HTTP 403): bad token", flush=True)
sys.exit(1)
)PY");
    EXPECT_EQ(out.started, QStringList({"Section_1_101500"}));
    EXPECT_EQ(out.failures, QStringList({"Section_1_101500"}));
    EXPECT_EQ(out.completed, 0);
    EXPECT_TRUE(out.stopped_early);
}
