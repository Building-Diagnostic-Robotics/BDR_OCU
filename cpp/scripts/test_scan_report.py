#!/usr/bin/env python3
"""scan_report.py: collect at mission close, send when online.

The robot is a temp folder laid out like /R_DATA; its "ssh" runs the same
shell commands locally. S3 is the local PUT server from test_uploader.py,
behind a fake /presign that builds the key exactly like the Lambda.
"""

import json
import os
import subprocess
import sys
import tarfile
import tempfile
import threading
import unittest
import unittest.mock
from datetime import datetime, timedelta, timezone
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import scan_report  # noqa: E402
import test_uploader  # noqa: E402
import uploader  # noqa: E402

CLIENT = "Easton_roofing_KC"
ROBOT = "Roofus#0002"
LOG_ID = "mission_20261017_131000"
DAY, BUILDING, MISSION = "October_17_2026", "1185_Obrien_Dr_3", "Mission_131105"


class LocalRobot(scan_report.Robot):
    """Runs the remote command locally; `down` makes every call fail like a dead link."""

    down = False

    def run(self, remote, timeout=30):
        if self.down:
            raise scan_report.RobotUnreachable("ssh: connect to host: No route to host")
        done = subprocess.run(["bash", "-c", remote], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
        return done.returncode, done.stdout


def make_robot_mission(root: Path, log_id=LOG_ID, finalized=True, big_logs=False) -> Path:
    mission = root / DAY / BUILDING / MISSION
    section = root / DAY / BUILDING / "Section_1_131104_complete_gnss_invalid"
    (mission / "logs").mkdir(parents=True)
    (section / "logs").mkdir(parents=True)
    (mission / "logs" / "robot_ros_logs.tar.gz").write_bytes(b"M" * (25 * 1024 * 1024 if big_logs else 2048))
    (section / "logs" / "robot_ros_logs.tar.gz").write_bytes(b"S" * 4096)
    config = {
        "mission_folder": str(mission), "mission_started_at": "2026-10-17T13:11:05",
        "mission_finalized_at": "2026-10-17T15:40:00" if finalized else None,
        "finalized_via": "end_and_save", "operator_name": "Joe", "building_name": "1185 Obrien Dr 3",
        "ocu_logs": [log_id], "gnss_enabled": True, "gnss_invalid": True,
        "gnss_invalid_reasons": ["RAWX coverage ratio 0.612 below 0.800"],
        "mission_gnss_bytes_written": 123456,
        "robot_ros_logs": {"version": 2, "status": "ok"},
        "sections": [{"section_name": "Section_1_131104", "section_folder": str(section),
                      "completion_tag": "complete_gnss_invalid", "deleted": False,
                      "start_time": "2026-10-17T13:11:04", "end_time": "2026-10-17T15:39:00"}],
    }
    (mission / "mission_config.json").write_text(json.dumps(config))
    # Another mission that must not match.
    other = root / DAY / BUILDING / "Mission_090000"
    other.mkdir(parents=True)
    (other / "mission_config.json").write_text(json.dumps({"ocu_logs": ["mission_20261017_085900"]}))
    return mission


class Case(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name)
        self.robot_root = base / "R_DATA"
        self.robot_root.mkdir()
        self.logs = base / "mission_logs"
        self.logs.mkdir()
        (self.logs / f"{LOG_ID}.log").write_text("2026-10-17T13:10:00Z [send] launch\n")
        (self.logs / f"{LOG_ID}.laptop_ros_logs.tar.gz").write_bytes(b"L" * 1000)
        self.outbox = base / "scan_outbox"
        LocalRobot.down = False
        patcher = unittest.mock.patch.object(scan_report, "Robot", LocalRobot)
        patcher.start()
        self.addCleanup(patcher.stop)

    def tearDown(self):
        self.tmp.cleanup()

    def collect(self, wait=0.0, log_id=LOG_ID):
        return scan_report.main([
            "collect", "--outbox", str(self.outbox), "--logs-dir", str(self.logs), "--log-id", log_id,
            "--client-id", CLIENT, "--robot-id", ROBOT, "--host", "robot", "--data-root", str(self.robot_root),
            "--ocu-version", "2.3.0", "--wait", str(wait),
        ])

    def entry(self, log_id=LOG_ID) -> dict:
        return json.loads((self.outbox / log_id / scan_report.ENTRY).read_text())


class CollectTests(Case):
    def test_collect_finds_the_mission_by_log_id_and_bundles_it(self):
        make_robot_mission(self.robot_root)
        self.assertEqual(self.collect(), 0)
        entry = self.entry()
        self.assertEqual(entry["state"], scan_report.READY)
        self.assertEqual(entry["run_id"], f"{DAY}/{BUILDING}/{MISSION}")
        self.assertEqual(entry["stem"], f"{DAY}__{BUILDING}__{MISSION}")
        record = json.loads((self.outbox / LOG_ID / scan_report.RECORD).read_text())
        self.assertEqual(record["run_id"], f"{DAY}/{BUILDING}/{MISSION}")
        self.assertEqual(record["completed_at"], "2026-10-17T15:40:00")
        self.assertEqual(record["sections"][0]["name"], "Section_1_131104_complete_gnss_invalid")
        self.assertEqual(record["gnss"]["invalid_reasons"], ["RAWX coverage ratio 0.612 below 0.800"])
        self.assertEqual(record["robot_logs_in_bundle"], "included")
        with tarfile.open(self.outbox / LOG_ID / scan_report.BUNDLE) as tar:
            names = sorted(tar.getnames())
        self.assertEqual(names, sorted([
            "mission_config.json",
            f"robot/{MISSION}/logs/robot_ros_logs.tar.gz",
            "robot/Section_1_131104_complete_gnss_invalid/logs/robot_ros_logs.tar.gz",
            f"ocu/{LOG_ID}.log",
            f"ocu/{LOG_ID}.laptop_ros_logs.tar.gz",
        ]))

    def test_big_robot_logs_stay_for_the_stick(self):
        make_robot_mission(self.robot_root, big_logs=True)
        self.collect()
        record = json.loads((self.outbox / LOG_ID / scan_report.RECORD).read_text())
        self.assertTrue(record["robot_logs_in_bundle"].startswith("skipped"))
        self.assertFalse(any(name.startswith("robot/") for name in record["bundle"]))

    def test_unreachable_robot_leaves_a_pending_entry(self):
        make_robot_mission(self.robot_root)
        LocalRobot.down = True
        self.assertEqual(self.collect(), 0)
        entry = self.entry()
        self.assertEqual(entry["state"], scan_report.PENDING)
        self.assertIn("not reachable", entry["last_error"])
        self.assertFalse((self.outbox / LOG_ID / scan_report.RECORD).exists())

    def test_no_mission_on_the_robot_is_not_a_scan(self):
        self.collect()
        entry = self.entry()
        self.assertEqual(entry["state"], scan_report.SKIPPED)
        self.assertIn("no Mission folder", entry["last_error"])

    def test_bad_log_id_is_refused(self):
        self.assertEqual(self.collect(log_id="../etc"), 2)

    def test_stem_cleans_names_the_lambda_would_refuse(self):
        self.assertEqual(scan_report.record_stem("Oct_1_2026/Acme HQ #2/Mission_101500"),
                         "Oct_1_2026__Acme-HQ-2__Mission_101500")


class SendCase(Case):
    def setUp(self):
        super().setUp()
        self.server = test_uploader.PutServer()
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.keys = []
        uploader.CLIENT_ID, uploader.DEVICE_TOKEN, uploader.API_BASE = CLIENT, "tok", "http://api.invalid"

        def presign(robot_id, run_id, relpath, size, content_md5):
            key = f"{CLIENT}/{robot_id}/{run_id}/{relpath}"  # the Lambda's _make_key
            self.keys.append(key)
            return {"upload_url": f"http://127.0.0.1:{self.port}/put?rel={relpath}", "md5_signed": True}

        patcher = unittest.mock.patch.object(uploader, "presign", side_effect=presign)
        patcher.start()
        self.addCleanup(patcher.stop)
        env = unittest.mock.patch.dict(os.environ, {"BDR_CLOUD_CLIENT_ID": CLIENT, "BDR_CLOUD_DEVICE_TOKEN": "tok"})
        env.start()
        self.addCleanup(env.stop)

    def send(self, *extra):
        return scan_report.main(["send", "--outbox", str(self.outbox), "--robot-id", ROBOT, "--ocu-version", "2.3.0", *extra])


class SendTests(SendCase):
    def test_send_uploads_bundle_then_record_flat_and_pings(self):
        make_robot_mission(self.robot_root)
        self.collect()
        self.assertEqual(self.send(), 0)
        stem = f"{DAY}__{BUILDING}__{MISSION}"
        self.assertEqual(self.keys, [
            f"{CLIENT}/{ROBOT}/_scan_records/{stem}.tar.gz",
            f"{CLIENT}/{ROBOT}/_scan_records/{stem}.json",
            f"{CLIENT}/{ROBOT}/_ocu_status/last_seen.json",
        ])
        # Flat: nothing under _scan_records is a folder.
        for key in self.keys:
            rest = key.split("/_scan_records/", 1)
            if len(rest) == 2:
                self.assertNotIn("/", rest[1])
        record = json.loads(next(p["body"] for p in self.server.puts if p["rel"].endswith(".json") and "last_seen" not in p["rel"]))
        self.assertEqual(record["run_id"], f"{DAY}/{BUILDING}/{MISSION}")
        seen = json.loads(next(p["body"] for p in self.server.puts if p["rel"] == "last_seen.json"))
        self.assertEqual(seen["outbox_pending"], 0)
        self.assertEqual(self.entry()["state"], scan_report.SENT)
        # A second run sends nothing new and does not ping again so soon.
        self.keys.clear()
        self.assertEqual(self.send(), 0)
        self.assertEqual(self.keys, [])

    def test_pending_entry_is_fetched_once_the_robot_is_back(self):
        make_robot_mission(self.robot_root)
        LocalRobot.down = True
        self.collect()
        self.assertEqual(self.send(), 0)
        self.assertEqual(self.entry()["state"], scan_report.PENDING)
        seen = json.loads(self.server.puts[-1]["body"])
        self.assertEqual(seen["outbox_pending"], 1)
        LocalRobot.down = False
        status = self.outbox / "status.json"
        status.unlink()  # allow the next ping
        self.assertEqual(self.send(), 0)
        self.assertEqual(self.entry()["state"], scan_report.SENT)

    def test_no_fetch_during_a_mission(self):
        make_robot_mission(self.robot_root)
        LocalRobot.down = True
        self.collect()
        LocalRobot.down = False
        self.send("--no-fetch")
        self.assertEqual(self.entry()["state"], scan_report.PENDING)

    def test_offline_keeps_everything(self):
        make_robot_mission(self.robot_root)
        self.collect()
        import requests

        with unittest.mock.patch.object(uploader, "presign", side_effect=requests.ConnectionError("no route")):
            self.assertEqual(self.send(), 3)
        self.assertEqual(self.entry()["state"], scan_report.READY)

    def test_refused_credentials_stop_the_run(self):
        make_robot_mission(self.robot_root)
        self.collect()
        with unittest.mock.patch.object(uploader, "presign", side_effect=uploader.CloudRejected("/presign", 403)):
            self.assertEqual(self.send(), 4)
        self.assertEqual(self.entry()["state"], scan_report.READY)

    def test_no_credentials_keeps_reports(self):
        make_robot_mission(self.robot_root)
        self.collect()
        with unittest.mock.patch.dict(os.environ, {"BDR_CLOUD_DEVICE_TOKEN": ""}):
            self.assertEqual(self.send(), 0)
        self.assertEqual(self.keys, [])
        self.assertEqual(self.entry()["state"], scan_report.READY)

    def test_old_pending_expires_and_old_sent_is_pruned(self):
        make_robot_mission(self.robot_root)
        LocalRobot.down = True
        self.collect()
        entry = self.entry()
        entry["created_at"] = (datetime.now(timezone.utc) - timedelta(days=15)).isoformat()
        (self.outbox / LOG_ID / scan_report.ENTRY).write_text(json.dumps(entry))
        self.send()
        self.assertEqual(self.entry()["state"], scan_report.EXPIRED)
        entry = self.entry()
        entry["created_at"] = (datetime.now(timezone.utc) - timedelta(days=31)).isoformat()
        (self.outbox / LOG_ID / scan_report.ENTRY).write_text(json.dumps(entry))
        self.send()
        self.assertFalse((self.outbox / LOG_ID).exists())

    def test_one_run_at_a_time(self):
        self.outbox.mkdir(parents=True, exist_ok=True)
        lock = scan_report._lock(scan_report.Outbox(self.outbox))
        try:
            self.assertEqual(self.send(), 5)
        finally:
            lock.close()


if __name__ == "__main__":
    unittest.main()
