#!/usr/bin/env python3
"""
scan_report.py — tell BDR about each scan the next time the laptop is online.

The robot is offline and its data reaches S3 only when the operator uploads
the stick, often weeks later. This script makes a small report of every scan
right after the mission ends and sends it whenever the laptop has internet,
so BDR knows a scan happened and which data to expect.

Two commands, both started by the OCU (detached, like ros_log_bundle.py):

  collect   at mission close. Finds the robot's Mission folder by the OCU
            log id (mission_config.json "ocu_logs"), copies its
            mission_config.json and, when small, the robot ROS log archives
            over the radio, adds the OCU log, and puts one report in the
            outbox. If the robot cannot be reached, the report waits as
            pending and `send` fetches it on a later run.

  send      every few minutes while the OCU runs. Fetches pending reports,
            uploads ready ones through the same /presign API as uploader.py,
            and writes the laptop's last-online note.

S3 layout (flat files: ReportGen and the client portal list folders under
<client>/<robot>/ as dates, so `_scan_records` must hold no sub-folders):

  <client>/<robot>/_scan_records/<day>__<building>__<Mission_x>.tar.gz
  <client>/<robot>/_scan_records/<day>__<building>__<Mission_x>.json   (last)
  <client>/<robot>/_ocu_status/last_seen.json

record.json carries the exact run_id, so a building name that had to be
cleaned for the file name still matches its upload folder.

Credentials come from the environment, as for uploader.py:
BDR_CLOUD_API_BASE, BDR_CLOUD_CLIENT_ID, BDR_CLOUD_DEVICE_TOKEN.

Exit codes: 0 ok, 2 bad arguments, 3 offline (nothing lost), 4 cloud
refused the credentials, 5 another run holds the lock.
"""

from __future__ import annotations

import argparse
import fcntl
import io
import json
import os
import re
import shlex
import shutil
import socket
import subprocess
import sys
import tarfile
import tempfile
import time
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Callable, Dict, List, Optional, Tuple

SCHEMA = 1
RECORDS = "_scan_records"
STATUS = "_ocu_status"
ENTRY = "entry.json"
RECORD = "record.json"
BUNDLE = "bundle.tar.gz"
# Robot log archives ride along only when this small: the radio does ~4 Mb/s.
ROBOT_LOG_CAP_BYTES = 20 * 1024 * 1024
LAPTOP_LOG_CAP_BYTES = 20 * 1024 * 1024
PENDING_GIVE_UP = timedelta(days=14)
KEEP_SENT = timedelta(days=30)
PING_EVERY = timedelta(minutes=15)

PENDING, READY, SENT, SKIPPED, EXPIRED = "pending_fetch", "ready", "sent", "skipped", "expired"

_SAFE = re.compile(r"[^A-Za-z0-9._-]+")
_LOG_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
_MISSION_PATH = re.compile(r"^(?P<root>/.+)/(?P<day>[^/]+)/(?P<building>[^/]+)/(?P<mission>Mission_[^/]+)/mission_config\.json$")


def now_utc() -> datetime:
    return datetime.now(timezone.utc)


def iso(value: datetime) -> str:
    return value.astimezone(timezone.utc).replace(microsecond=0).isoformat()


def parse_iso(value) -> Optional[datetime]:
    try:
        parsed = datetime.fromisoformat(str(value))
    except (TypeError, ValueError):
        return None
    return parsed if parsed.tzinfo else parsed.replace(tzinfo=timezone.utc)


def say(text: str) -> None:
    print(f"[scan_report] {text}", flush=True)


def record_stem(run_id: str) -> str:
    """`Oct_1_2026/Acme HQ/Mission_101500` -> `Oct_1_2026__Acme-HQ__Mission_101500`.

    The presign Lambda only accepts [A-Za-z0-9._-/] in a file name, so
    anything else becomes '-'. The exact run_id stays inside record.json.
    """
    parts = [part for part in run_id.strip("/").split("/") if part]
    return "__".join(_SAFE.sub("-", part).strip("-") or "x" for part in parts)


def write_json(path: Path, doc: dict) -> None:
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(doc, indent=2, default=str) + "\n", encoding="utf-8")
    os.replace(tmp, path)


def read_json(path: Path) -> Optional[dict]:
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return doc if isinstance(doc, dict) else None


# ------------------------------------------------------------------ robot ---

class RobotUnreachable(Exception):
    pass


class Robot:
    """SSH to the robot. Every call fails fast; a dead link is normal here."""

    def __init__(self, host: str, user: str, connect_timeout: int = 6):
        self.host, self.user, self.connect_timeout = host, user, connect_timeout

    def _argv(self, remote: str) -> List[str]:
        return [
            "ssh", "-o", f"ConnectTimeout={self.connect_timeout}", "-o", "BatchMode=yes",
            "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
            "-o", "LogLevel=ERROR", "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=6",
            f"{self.user}@{self.host}", remote,
        ]

    def run(self, remote: str, timeout: float = 30) -> Tuple[int, bytes]:
        try:
            done = subprocess.run(self._argv(remote), stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, timeout=timeout)
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise RobotUnreachable(str(exc)) from exc
        if done.returncode == 255:  # ssh itself failed: no link, auth, host down
            raise RobotUnreachable(done.stderr.decode("utf-8", "replace").strip()[:200])
        return done.returncode, done.stdout

    def find_mission(self, data_root: str, log_id: str) -> Optional[str]:
        """Path of the mission_config.json that lists this OCU log id."""
        pattern = shlex.quote(f'"{log_id}"')
        glob = f"{shlex.quote(data_root.rstrip('/'))}/*/*/Mission_*/mission_config.json"
        _, out = self.run(f"grep -lF -- {pattern} {glob} 2>/dev/null || true")
        paths = [line for line in out.decode("utf-8", "replace").splitlines() if line.strip()]
        return sorted(paths)[-1] if paths else None

    def read_text(self, path: str, limit: int = 4 * 1024 * 1024) -> Optional[str]:
        code, out = self.run(f"head -c {limit} {shlex.quote(path)}")
        return out.decode("utf-8", "replace") if code == 0 else None

    def sizes(self, paths: List[str]) -> Dict[str, int]:
        if not paths:
            return {}
        quoted = " ".join(shlex.quote(p) for p in paths)
        _, out = self.run(f"stat -c '%s %n' {quoted} 2>/dev/null || true")
        found = {}
        for line in out.decode("utf-8", "replace").splitlines():
            size, _, name = line.partition(" ")
            if size.isdigit():
                found[name] = int(size)
        return found

    def fetch(self, remote: str, local: Path, timeout: float = 120) -> bool:
        local.parent.mkdir(parents=True, exist_ok=True)
        code, out = self.run(f"cat {shlex.quote(remote)}", timeout=timeout)
        if code != 0:
            return False
        local.write_bytes(out)
        return True

    def list_files(self, folder: str) -> List[str]:
        _, out = self.run(f"find {shlex.quote(folder)} -maxdepth 2 -type f 2>/dev/null || true")
        return [line for line in out.decode("utf-8", "replace").splitlines() if line.strip()]


# ------------------------------------------------------------------ outbox ---

class Outbox:
    def __init__(self, root: Path):
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)

    def entry_dir(self, log_id: str) -> Path:
        return self.root / log_id

    def entries(self) -> List[Tuple[Path, dict]]:
        found = []
        for child in sorted(p for p in self.root.iterdir() if p.is_dir()):
            doc = read_json(child / ENTRY)
            if doc is not None:
                found.append((child, doc))
        return found

    def save(self, folder: Path, entry: dict) -> None:
        folder.mkdir(parents=True, exist_ok=True)
        write_json(folder / ENTRY, entry)

    def status(self) -> dict:
        return read_json(self.root / "status.json") or {}

    def save_status(self, doc: dict) -> None:
        write_json(self.root / "status.json", doc)


def new_entry(args) -> dict:
    return {
        "schema": SCHEMA,
        "log_id": args.log_id,
        "state": PENDING,
        "created_at": iso(now_utc()),
        "client_id": args.client_id,
        "robot_id": args.robot_id,
        "host": args.host,
        "user": args.user,
        "data_root": args.data_root,
        "logs_dir": args.logs_dir,
        "ocu": {
            "version": args.ocu_version or "",
            "laptop": socket.gethostname(),
            "building_entered": args.building or "",
            "operator_entered": args.operator or "",
        },
        "attempts": 0,
        "last_error": "",
    }


# ------------------------------------------------------------- build report ---

def build_record(entry: dict, config: dict, run_id: str, bundled: List[str], robot_logs_note: str) -> dict:
    sections = []
    for item in config.get("sections") or []:
        if not isinstance(item, dict):
            continue
        folder = str(item.get("section_folder") or item.get("section_name") or "")
        sections.append({
            "name": Path(folder).name,
            "completion_tag": item.get("completion_tag"),
            "deleted": bool(item.get("deleted")),
            "start": item.get("start_time"),
            "end": item.get("end_time"),
        })
    return {
        "schema": SCHEMA,
        "run_id": run_id,
        "client_id": entry["client_id"],
        "robot_id": entry["robot_id"],
        "ocu_log_id": entry["log_id"],
        "building": config.get("building_name") or entry["ocu"].get("building_entered") or "",
        "operator": config.get("operator_name") or entry["ocu"].get("operator_entered") or "",
        "started_at": config.get("mission_started_at"),
        "completed_at": config.get("mission_finalized_at"),
        "finalized_via": config.get("finalized_via") or "",
        "scan_mode": config.get("scan_mode"),
        "sections": sections,
        "gnss": {
            "enabled": config.get("gnss_enabled"),
            "invalid": config.get("gnss_invalid"),
            "invalid_reasons": config.get("gnss_invalid_reasons") or [],
            "warning_reasons": config.get("gnss_warning_reasons") or [],
            "ubx_bytes": config.get("mission_gnss_bytes_written"),
            "driver_restarts": config.get("driver_restarts"),
        },
        "robot_ros_logs": config.get("robot_ros_logs"),
        "robot_logs_in_bundle": robot_logs_note,
        "ocu": dict(entry["ocu"], collected_at=iso(now_utc())),
        "bundle": bundled,
        "robot_report": "mission_config",
    }


def _wait_for_file(path: Path, deadline: float, settle_s: float = 3.0) -> bool:
    """True once the file exists and its size has held still for settle_s."""
    last, stable_since = -1, None
    while time.monotonic() < deadline:
        try:
            size = path.stat().st_size
        except OSError:
            size = -1
        if size >= 0 and size == last:
            if stable_since is None:
                stable_since = time.monotonic()
            if time.monotonic() - stable_since >= settle_s:
                return True
        else:
            stable_since = None
        last = size
        time.sleep(1.0)
    return path.is_file()


def fetch_entry(folder: Path, entry: dict, robot: Robot, wait_s: float = 0.0,
                sleep: Callable[[float], None] = time.sleep) -> dict:
    """Try to turn a pending entry into a ready one. Returns the updated entry.

    Raises RobotUnreachable when the robot cannot be reached; the caller
    keeps the entry pending.
    """
    deadline = time.monotonic() + wait_s
    # mission_config.json exists from scan start, so a short wait is enough;
    # no match then means the scan was deleted or never started.
    find_deadline = time.monotonic() + min(wait_s, 15.0)
    config_path = robot.find_mission(entry["data_root"], entry["log_id"])
    while config_path is None and time.monotonic() < find_deadline:
        sleep(3.0)
        config_path = robot.find_mission(entry["data_root"], entry["log_id"])
    if config_path is None:
        entry.update(state=SKIPPED, last_error="no Mission folder on the robot names this OCU log "
                     "(the scan was deleted, or data collection never started)")
        return entry
    match = _MISSION_PATH.match(config_path)
    if not match:
        entry.update(state=SKIPPED, last_error=f"unexpected mission path {config_path}")
        return entry
    run_id = f"{match['day']}/{match['building']}/{match['mission']}"
    mission_dir = config_path.rsplit("/", 1)[0]

    # Wait for the finalize and the robot's log packing (about 6 s after it).
    config = {}
    while True:
        text = robot.read_text(config_path)
        try:
            config = json.loads(text or "{}")
        except ValueError:
            config = {}
        packed = (config.get("robot_ros_logs") or {}).get("status") not in (None, "", "pending")
        if (config.get("mission_finalized_at") and packed) or time.monotonic() >= deadline:
            break
        sleep(3.0)

    stage = Path(tempfile.mkdtemp(prefix="bundle-", dir=folder))
    try:
        (stage / "mission_config.json").write_text(json.dumps(config, indent=2), encoding="utf-8")
        bundled = ["mission_config.json"]

        # Robot ROS logs: mission + per-section archives, all or none, under the cap.
        archives = [f"{mission_dir}/logs/robot_ros_logs.tar.gz"]
        for item in config.get("sections") or []:
            name = Path(str((item or {}).get("section_folder") or "")).name
            if name:
                archives.append(f"{mission_dir.rsplit('/', 1)[0]}/{name}/logs/robot_ros_logs.tar.gz")
        sizes = robot.sizes(archives)
        total = sum(sizes.values())
        if not sizes:
            note = "none on the robot"
        elif total > ROBOT_LOG_CAP_BYTES:
            note = f"skipped: {total} bytes is over the radio cap; they arrive with the stick upload"
        else:
            note = "included"
            for remote in sizes:
                rel = "robot/" + remote[len(mission_dir.rsplit('/', 1)[0]) + 1:]
                if robot.fetch(remote, stage / rel):
                    bundled.append(rel)
                else:
                    note = "partly included"
        # A robot that writes scan_report/ (planned) gets it all, small by design.
        for remote in robot.list_files(f"{mission_dir}/scan_report"):
            rel = "robot/" + remote[len(mission_dir.rsplit('/', 1)[0]) + 1:]
            if robot.fetch(remote, stage / rel):
                bundled.append(rel)

        # OCU's own logs for this mission. The laptop ROS bundle is packed
        # detached ~35-65 s after the log closes; wait for it inside wait_s.
        logs_dir = Path(entry.get("logs_dir") or "")
        (stage / "ocu").mkdir(exist_ok=True)
        log_file = logs_dir / f"{entry['log_id']}.log"
        if log_file.is_file():
            shutil.copy2(log_file, stage / "ocu" / log_file.name)
            bundled.append(f"ocu/{log_file.name}")
        laptop = logs_dir / f"{entry['log_id']}.laptop_ros_logs.tar.gz"
        if wait_s > 0:
            _wait_for_file(laptop, deadline)
        if laptop.is_file() and laptop.stat().st_size <= LAPTOP_LOG_CAP_BYTES:
            shutil.copy2(laptop, stage / "ocu" / laptop.name)
            bundled.append(f"ocu/{laptop.name}")

        with tarfile.open(folder / BUNDLE, "w:gz") as tar:
            for rel in bundled:
                tar.add(stage / rel, arcname=rel)
        record = build_record(entry, config, run_id, bundled, note)
        write_json(folder / RECORD, record)
    finally:
        shutil.rmtree(stage, ignore_errors=True)
    entry.update(state=READY, run_id=run_id, stem=record_stem(run_id), fetched_at=iso(now_utc()), last_error="")
    return entry


# ------------------------------------------------------------------- cloud ---

class Offline(Exception):
    pass


class Refused(Exception):
    pass


def _uploader():
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import uploader  # noqa: E402  (reads BDR_CLOUD_* at import)
    return uploader


def put_file(uploader, robot_id: str, run_id: str, local: Path, relpath: str) -> None:
    try:
        uploader.put_verified_file(robot_id, run_id, str(local), relpath)
    except uploader.CloudRejected as exc:
        if exc.status in (401, 403):
            raise Refused(str(exc)) from exc
        raise
    except Exception as exc:  # requests' connection errors and timeouts
        name = type(exc).__name__
        if name in ("ConnectionError", "Timeout", "ConnectTimeout", "ReadTimeout", "SSLError", "ProxyError"):
            raise Offline(str(exc)) from exc
        raise


# ------------------------------------------------------------------ commands ---

def cmd_collect(args) -> int:
    if not _LOG_ID.match(args.log_id or ""):
        say(f"bad log id {args.log_id!r}")
        return 2
    outbox = Outbox(Path(args.outbox))
    folder = outbox.entry_dir(args.log_id)
    entry = read_json(folder / ENTRY) or new_entry(args)
    if entry.get("state") not in (PENDING,):
        say(f"{args.log_id}: already {entry.get('state')}")
        return 0
    outbox.save(folder, entry)
    robot = Robot(args.host, args.user)
    entry["attempts"] = int(entry.get("attempts") or 0) + 1
    try:
        entry = fetch_entry(folder, entry, robot, wait_s=args.wait)
    except RobotUnreachable as exc:
        entry["last_error"] = f"robot not reachable: {exc}"
        say(f"{args.log_id}: {entry['last_error']}; will retry from send")
    except Exception as exc:  # noqa: BLE001 — never lose the entry
        entry["last_error"] = f"{type(exc).__name__}: {exc}"
        say(f"{args.log_id}: fetch failed: {entry['last_error']}")
    outbox.save(folder, entry)
    say(f"{args.log_id}: {entry['state']}" + (f" ({entry.get('run_id')})" if entry.get("run_id") else ""))
    return 0


def _lock(outbox: Outbox):
    handle = open(outbox.root / ".lock", "w")
    try:
        fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        handle.close()
        return None
    return handle


def cmd_send(args) -> int:
    outbox = Outbox(Path(args.outbox))
    lock = _lock(outbox)
    if lock is None:
        say("another run is active")
        return 5
    try:
        return _send(outbox, args)
    finally:
        lock.close()


def _send(outbox: Outbox, args) -> int:
    now = now_utc()
    entries = outbox.entries()

    # 1. Pending fetches, only for this laptop's robot and only when allowed.
    for folder, entry in entries:
        if entry.get("state") != PENDING:
            continue
        created = parse_iso(entry.get("created_at")) or now
        if now - created > PENDING_GIVE_UP:
            entry.update(state=EXPIRED, last_error="robot never reachable within 14 days; "
                         "the OCU log still goes up with the stick upload")
            outbox.save(folder, entry)
            continue
        if args.no_fetch or entry.get("robot_id") != args.robot_id:
            continue
        entry["attempts"] = int(entry.get("attempts") or 0) + 1
        try:
            entry = fetch_entry(folder, entry, Robot(entry["host"], entry["user"], connect_timeout=4))
        except RobotUnreachable as exc:
            entry["last_error"] = f"robot not reachable: {exc}"
            outbox.save(folder, entry)
            break  # same robot for the rest; try again next run
        except Exception as exc:  # noqa: BLE001
            entry["last_error"] = f"{type(exc).__name__}: {exc}"
        outbox.save(folder, entry)

    if not (os.environ.get("BDR_CLOUD_CLIENT_ID") and os.environ.get("BDR_CLOUD_DEVICE_TOKEN")):
        say("no cloud credentials for this robot; reports stay in the outbox")
        return 0
    uploader = _uploader()

    # 2. Ready reports: bundle first, record last (Datum treats the record as complete).
    sent = 0
    try:
        for folder, entry in outbox.entries():
            if entry.get("state") != READY:
                continue
            if entry.get("client_id") != os.environ.get("BDR_CLOUD_CLIENT_ID"):
                continue  # another robot's report on this laptop; its own credentials send it
            stem = entry["stem"]
            if (folder / BUNDLE).is_file():
                put_file(uploader, entry["robot_id"], RECORDS, folder / BUNDLE, f"{stem}.tar.gz")
            put_file(uploader, entry["robot_id"], RECORDS, folder / RECORD, f"{stem}.json")
            entry.update(state=SENT, sent_at=iso(now_utc()), last_error="")
            outbox.save(folder, entry)
            sent += 1
            say(f"sent {entry['robot_id']} {entry['run_id']}")

        # 3. Last-online note.
        status = outbox.status()
        last_ping = parse_iso(status.get("last_ping"))
        if last_ping is None or now - last_ping >= PING_EVERY or sent:
            remaining = [e for _, e in outbox.entries() if e.get("state") in (PENDING, READY)]
            oldest = min((parse_iso(e.get("created_at")) or now for e in remaining), default=None)
            note = {
                "schema": SCHEMA,
                "time": iso(now_utc()),
                "robot_id": args.robot_id,
                "laptop": socket.gethostname(),
                "ocu_version": args.ocu_version or "",
                "outbox_pending": len(remaining),
                "oldest_pending_at": iso(oldest) if oldest else None,
                "free_bytes": shutil.disk_usage(outbox.root).free,
            }
            tmp = outbox.root / "last_seen.json"
            write_json(tmp, note)
            put_file(uploader, args.robot_id, STATUS, tmp, "last_seen.json")
            status["last_ping"] = note["time"]
            outbox.save_status(status)
    except Offline as exc:
        say(f"offline: {exc}")
        return 3
    except Refused as exc:
        say(f"cloud refused this robot's credentials: {exc}")
        return 4

    # 4. Prune sent, skipped and expired entries after 30 days.
    for folder, entry in outbox.entries():
        stamp = parse_iso(entry.get("sent_at") or entry.get("created_at")) or now
        if entry.get("state") in (SENT, SKIPPED, EXPIRED) and now - stamp > KEEP_SENT:
            shutil.rmtree(folder, ignore_errors=True)
    return 0


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(prog="scan_report.py")
    sub = parser.add_subparsers(dest="command", required=True)
    collect = sub.add_parser("collect")
    collect.add_argument("--outbox", required=True)
    collect.add_argument("--logs-dir", required=True)
    collect.add_argument("--log-id", required=True)
    collect.add_argument("--client-id", required=True)
    collect.add_argument("--robot-id", required=True)
    collect.add_argument("--host", required=True)
    collect.add_argument("--user", default="roofus")
    collect.add_argument("--data-root", default="/R_DATA")
    collect.add_argument("--building", default="")
    collect.add_argument("--operator", default="")
    collect.add_argument("--ocu-version", default="")
    collect.add_argument("--wait", type=float, default=90.0)
    send = sub.add_parser("send")
    send.add_argument("--outbox", required=True)
    send.add_argument("--robot-id", required=True)
    send.add_argument("--ocu-version", default="")
    send.add_argument("--no-fetch", action="store_true")
    try:
        args = parser.parse_args(argv)
    except SystemExit as exc:
        return 2 if exc.code else 0
    return cmd_collect(args) if args.command == "collect" else cmd_send(args)


if __name__ == "__main__":
    sys.exit(main())
