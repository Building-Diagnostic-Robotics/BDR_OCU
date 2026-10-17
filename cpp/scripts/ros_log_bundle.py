#!/usr/bin/env python3
"""
ros_log_bundle.py — cut the ROS logs of a mission out of the log directory
and pack them as .tar.gz, so they travel with the mission's data.

Shared, byte-for-byte, by the robot (pilot_control/scripts, used by
rdata_thumbdrive_sync.py the moment a mission completes) and the OCU laptop
(BDR_OCU cpp/scripts, run when the OCU closes a mission log). Keep the two
copies identical. Stdlib only: no ROS, no rclpy.

Line-exact, not file-exact. A ROS log file lives as long as its process,
which is usually longer than one section or one mission, so files are cut
by the timestamp on each line:
  launch.log, <proc>-N-stdout.log   "1697545432.1234567 ..."
  node logs (<name>_<pid>_<ms>.log) "[INFO] [1697545432.123456789] [...]"
A line without a timestamp (traceback, multi-line message) goes with the
line before it. A file with no recognisable timestamps at all (custom
RCUTILS_CONSOLE_OUTPUT_FORMAT) cannot be cut and goes whole to the target
marked `catch_all`, if it was written to inside that target's time range.

Targets: each is an archive plus time intervals; a line goes to the first
target whose interval holds its time (so sections listed before the
mission take their lines, and the mission archive gets the rest). Each
archive keeps the source layout under "ros_log/". One machine, one clock:
the intervals and the log times are both this machine's, so no
laptop/robot clock matching is involved.

Size: each archive is capped (default 100 MB compressed). Files go in
newest first; a slice whose estimated compressed size would not fit is
skipped (named in the summary). A log still being written is read only up
to the size it had when packing started, so archives are always valid.

Never raises for missing logs: each summary says what happened ("ok",
"empty", "no_log_dir", "error"). Packing never fails a mission copy.

CLI (the OCU runs this):
  ros_log_bundle.py --since EPOCH [--until EPOCH] --out PATH
                    [--log-dir DIR] [--cap-bytes N] [--wait-launch-secs S]
prints one JSON summary line; exit 0 (2 on bad arguments).
"""
from __future__ import annotations

import argparse
import json
import math
import os
import re
import sys
import tarfile
import tempfile
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Callable, Dict, Iterable, List, Optional, Tuple

DEFAULT_CAP_BYTES = 100 * 1024 * 1024
#: Seconds before the mission start that still count (the launch that
#: started the mission writes its first lines a little before).
DEFAULT_LEAD_SECS = 120.0
#: Text logs compress about 5-10x; assume 8x to decide whether a file fits.
_EST_RATIO = 8
ARCHIVE_ROOT = "ros_log"

_LOG_ID_OK = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")


# ---------------------------------------------------------------- log ids ---

def sanitize_log_id(value) -> str:
    """A safe OCU log id ('mission_20261017_101500'), or '' if not one.

    The id becomes a file name on the robot side of nothing and on the
    laptop side of a lookup, so only plain names are accepted: letters,
    digits, '.', '_', '-', no leading dot, no path separators, <= 128.
    """
    if not isinstance(value, str):
        return ""
    value = value.strip()
    if not _LOG_ID_OK.match(value) or ".." in value:
        return ""
    return value


def merge_log_ids(existing, new_id) -> List[str]:
    """`existing` (list or junk) plus `new_id` if valid and not already in."""
    out: List[str] = []
    if isinstance(existing, list):
        for item in existing:
            clean = sanitize_log_id(item)
            if clean and clean not in out:
                out.append(clean)
    clean = sanitize_log_id(new_id)
    if clean and clean not in out:
        out.append(clean)
    return out


# ------------------------------------------------------------- log dir ------

def resolve_log_dir(env: Optional[Dict[str, str]] = None) -> Path:
    """Where ROS writes logs: RDATA_ROS_LOG_DIR (override for the offload
    service), ROS_LOG_DIR, ROS_HOME/log, else ~/.ros/log."""
    env = os.environ if env is None else env
    for key in ("RDATA_ROS_LOG_DIR", "ROS_LOG_DIR"):
        if env.get(key):
            return Path(env[key]).expanduser()
    if env.get("ROS_HOME"):
        return Path(env["ROS_HOME"]).expanduser() / "log"
    home = env.get("HOME") or os.path.expanduser("~")
    return Path(home) / ".ros" / "log"


# ------------------------------------------------------- launch running -----

def launch_running(proc_root: str = "/proc", own_pid: Optional[int] = None) -> bool:
    """True while any `ros2 launch pilot_control ...launch.py` is alive."""
    own_pid = os.getpid() if own_pid is None else own_pid
    try:
        entries = os.listdir(proc_root)
    except OSError:
        return False
    for name in entries:
        if not name.isdigit() or int(name) == own_pid:
            continue
        try:
            with open(os.path.join(proc_root, name, "cmdline"), "rb") as fh:
                raw = fh.read()
        except OSError:
            continue  # exited meanwhile, or not ours to read
        args = [a.decode("utf-8", "replace") for a in raw.split(b"\0") if a]
        if not args:
            continue
        joined = " ".join(args)
        if ("ros2" in joined and "launch" in args and "pilot_control" in args
                and any(a.endswith(".launch.py") for a in args)):
            return True
    return False


def wait_for_launch_exit(timeout_s: float,
                         is_running: Callable[[], bool] = launch_running,
                         sleep: Callable[[float], None] = time.sleep,
                         clock: Callable[[], float] = time.monotonic,
                         poll_s: float = 1.0) -> bool:
    """Wait up to `timeout_s` for the robot stack to finish shutting down,
    so its last log lines are on disk. True if it exited (or was not
    running), False on timeout. Never longer than timeout_s (+ one poll)."""
    if timeout_s <= 0:
        return not is_running()
    end = clock() + timeout_s
    while is_running():
        remaining = end - clock()
        if remaining <= 0:
            return False
        sleep(min(poll_s, remaining))
    return True


# ----------------------------------------------------------- timestamps ---

#: launch.log and per-process output files: "<epoch> ..." at line start.
_TS_LAUNCH = re.compile(rb"^(1\d{9}\.\d{1,9})(?=[\s\]])")
#: rcl node log files (default format): "[LEVEL] [<epoch>] ...".
_TS_NODE = re.compile(rb"^\[[A-Z]+\]\s+\[(1\d{9}\.\d{1,9})\]")
#: A file whose first lines carry no timestamp is not cut (catch_all).
_PROBE_LINES = 200
#: Node logs are named <name>_<pid>_<creation ms>.log.
_NODE_LOG_CREATED = re.compile(r"_(\d{13})\.log$")
#: ros2 launch run folders: YYYY-MM-DD-HH-MM-SS-ffffff-<host>-<pid>, local time.
_LAUNCH_DIR_CREATED = re.compile(
    r"^(\d{4})-(\d{2})-(\d{2})-(\d{2})-(\d{2})-(\d{2})-(\d{6})-")


def line_time(line: bytes) -> Optional[float]:
    """Epoch seconds a ROS log line was written at, or None."""
    m = _TS_LAUNCH.match(line) or _TS_NODE.match(line)
    return float(m.group(1)) if m else None


def created_at(rel: str) -> Optional[float]:
    """When a log file was started, from its name (None if not encoded)."""
    m = _NODE_LOG_CREATED.search(os.path.basename(rel))
    if m:
        return int(m.group(1)) / 1000.0
    m = _LAUNCH_DIR_CREATED.match(rel.split(os.sep, 1)[0])
    if m:
        y, mo, d, h, mi, sec, us = (int(g) for g in m.groups())
        try:
            return datetime(y, mo, d, h, mi, sec, us).timestamp()
        except ValueError:
            return None
    return None


# --------------------------------------------------------------- select -----

def select_files(log_dir: Path, since: float,
                 until: Optional[float] = None) -> List[Dict]:
    """Regular files (no symlinks) under log_dir that may hold lines from
    [since, until]: last written at or after `since`, and not started
    after `until` (when the name says when). Newest first."""
    out: List[Dict] = []
    for base, dirs, files in os.walk(log_dir, followlinks=False):
        dirs[:] = [d for d in dirs if not os.path.islink(os.path.join(base, d))]
        for name in files:
            path = os.path.join(base, name)
            try:
                st = os.lstat(path)
            except OSError:
                continue
            if not os.path.isfile(path) or os.path.islink(path):
                continue
            if st.st_mtime < since:
                continue
            rel = os.path.relpath(path, log_dir)
            started = created_at(rel)
            if until is not None and started is not None and started > until:
                continue
            out.append({"path": path, "size": st.st_size, "mtime": st.st_mtime,
                        "rel": rel})
    out.sort(key=lambda f: (-f["mtime"], f["rel"]))
    return out


# --------------------------------------------------------------- bundle -----

@dataclass
class Target:
    """One output archive and the time intervals whose lines it takes."""
    name: str
    out_path: Path
    intervals: List[Tuple[float, float]]
    #: Receives whole files that have no line timestamps.
    catch_all: bool = False

    def holds(self, t: float) -> bool:
        return any(lo <= t <= hi for lo, hi in self.intervals)


def _new_summary(target: Target, log_dir: Path, cap_bytes: int) -> Dict:
    lo = min((a for a, _ in target.intervals), default=None)
    hi = max((b for _, b in target.intervals), default=None)
    return {
        "status": "ok", "log_dir": str(log_dir), "since": lo,
        "until": None if hi == math.inf else hi,
        "files": 0, "bytes_in": 0, "bytes_out": 0, "cap_bytes": cap_bytes,
        "skipped": [], "unsliced": [], "truncated": False,
        "archive": target.out_path.name, "error": None,
    }


def _iter_lines(path: str, limit: int):
    """Lines of the file's first `limit` bytes (its size when packing
    started): a log still being written is never read past that."""
    with open(path, "rb") as fh:
        remaining = limit
        while remaining > 0:
            line = fh.readline(min(remaining, 1 << 20))
            if not line:
                return
            remaining -= len(line)
            yield line


def _cut_file(f: Dict, targets: List[Target]) -> Tuple[Dict[str, object], bool]:
    """Split one file by line time. Returns ({target name: temp file with
    its slice, positioned at 0}, unsliced?). Raises OSError."""
    outs: Dict[str, object] = {}

    def sink(name):
        if name not in outs:
            outs[name] = tempfile.TemporaryFile()
        return outs[name]

    try:
        pending: List[bytes] = []
        current: Optional[str] = None
        seen_time = False
        for line in _iter_lines(f["path"], f["size"]):
            t = line_time(line)
            if t is not None:
                seen_time = True
                current = next((x.name for x in targets if x.holds(t)), None)
                if pending:
                    if current is not None:
                        sink(current).write(b"".join(pending))
                    pending = []
            elif not seen_time:
                pending.append(line)
                if len(pending) >= _PROBE_LINES:
                    break  # no timestamps up front: not a format we can cut
                continue
            if current is not None:
                sink(current).write(line)
        if not seen_time:
            for tmp in outs.values():
                tmp.close()
            outs = {}
            whole = next((x for x in targets if x.catch_all), None)
            if whole is None:
                return {}, True
            # Whole file, if it was written to inside the catch-all range.
            started = created_at(f["rel"])
            if not any(f["mtime"] >= lo and (started is None or started <= hi)
                       for lo, hi in whole.intervals):
                return {}, True
            tmp = sink(whole.name)
            with open(f["path"], "rb") as src:
                remaining = f["size"]
                while remaining > 0:
                    chunk = src.read(min(remaining, 1 << 20))
                    if not chunk:
                        break
                    tmp.write(chunk)
                    remaining -= len(chunk)
            tmp.seek(0)
            return outs, True
    except OSError:
        for tmp in outs.values():
            tmp.close()
        raise
    for tmp in outs.values():
        tmp.seek(0)
    return outs, False


class _Writer:
    """One .tar.gz being written to <out>.part, replaced into place on
    close. Opened only when the target receives its first slice."""

    def __init__(self, out_path: Path):
        self.out_path = out_path
        self.part = out_path.with_name(out_path.name + ".part")
        out_path.parent.mkdir(parents=True, exist_ok=True)
        self.raw = open(self.part, "wb")
        self.tar = tarfile.open(fileobj=self.raw, mode="w:gz", compresslevel=6)

    def tell(self) -> int:
        return self.raw.tell()

    def add(self, rel: str, mtime: float, data) -> int:
        size = data.seek(0, os.SEEK_END)
        data.seek(0)
        info = tarfile.TarInfo(name=f"{ARCHIVE_ROOT}/{rel}")
        info.size = size
        info.mtime = int(mtime)
        info.mode = 0o644
        self.tar.addfile(info, data)
        return size

    def finish(self) -> int:
        self.tar.close()
        self.raw.flush()
        os.fsync(self.raw.fileno())
        self.raw.close()
        os.replace(self.part, self.out_path)
        return self.out_path.stat().st_size

    def abandon(self) -> None:
        for closer in (self.tar.close, self.raw.close):
            try:
                closer()
            except (OSError, ValueError, tarfile.TarError):
                pass
        try:
            os.unlink(self.part)
        except OSError:
            pass


def split_bundle(log_dir: Path, targets: List[Target], *,
                 cap_bytes: int = DEFAULT_CAP_BYTES) -> Dict[str, Dict]:
    """Write each target's archive (atomically; none if it got no lines).
    Returns {target name: JSON-able summary}. Never raises OSError."""
    log_dir = Path(log_dir)
    summaries = {t.name: _new_summary(t, log_dir, cap_bytes) for t in targets}
    if not log_dir.is_dir():
        for s in summaries.values():
            s["status"] = "no_log_dir"
        return summaries
    spans = [iv for t in targets for iv in t.intervals]
    if not spans:
        for s in summaries.values():
            s["status"] = "empty"
        return summaries
    lo = min(a for a, _ in spans)
    hi = max(b for _, b in spans)
    files = select_files(log_dir, lo, None if hi == math.inf else hi)

    writers: Dict[str, _Writer] = {}
    failed: Dict[str, str] = {}
    by_name = {t.name: t for t in targets}
    for f in files:
        try:
            slices, unsliced = _cut_file(f, targets)
        except OSError:
            for s in summaries.values():
                s["skipped"].append(f["rel"])
            continue
        for name, data in slices.items():
            s = summaries[name]
            try:
                if name in failed:
                    continue
                if unsliced:
                    s["unsliced"].append(f["rel"])
                size = data.seek(0, os.SEEK_END)
                data.seek(0)
                writer = writers.get(name)
                written = writer.tell() if writer else 0
                if written + size // _EST_RATIO + 4096 > cap_bytes:
                    s["skipped"].append(f["rel"])
                    s["truncated"] = True
                    continue  # a smaller, older slice may still fit
                if writer is None:
                    writer = writers[name] = _Writer(by_name[name].out_path)
                s["bytes_in"] += writer.add(f["rel"], f["mtime"], data)
                s["files"] += 1
            except OSError as exc:
                failed[name] = str(exc)
                if name in writers:
                    writers.pop(name).abandon()
            finally:
                data.close()

    for t in targets:
        s = summaries[t.name]
        if t.name in failed:
            s.update(status="error", error=failed[t.name], files=0, bytes_in=0)
            continue
        writer = writers.get(t.name)
        if writer is None:
            s["status"] = "empty"
            continue
        try:
            s["bytes_out"] = writer.finish()
        except OSError as exc:
            writer.abandon()
            s.update(status="error", error=str(exc), files=0, bytes_in=0)
    return summaries


def bundle(log_dir: Path, since: float, out_path: Path, *,
           until: Optional[float] = None,
           cap_bytes: int = DEFAULT_CAP_BYTES) -> Dict:
    """One archive of the lines written in [since, until] (until None =
    up to now). Returns its summary."""
    target = Target("all", Path(out_path),
                    [(since, math.inf if until is None else until)],
                    catch_all=True)
    return split_bundle(log_dir, [target], cap_bytes=cap_bytes)["all"]


# ------------------------------------------------------------------ CLI -----

def main(argv: Optional[Iterable[str]] = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    p.add_argument("--since", type=float, required=True,
                   help="epoch seconds; lines written before are left out")
    p.add_argument("--until", type=float, default=None)
    p.add_argument("--out", required=True)
    p.add_argument("--log-dir", default=None)
    p.add_argument("--cap-bytes", type=int, default=DEFAULT_CAP_BYTES)
    p.add_argument("--wait-launch-secs", type=float, default=0.0)
    try:
        args = p.parse_args(None if argv is None else list(argv))
    except SystemExit:
        return 2
    if args.wait_launch_secs > 0:
        wait_for_launch_exit(args.wait_launch_secs)
    log_dir = Path(args.log_dir) if args.log_dir else resolve_log_dir()
    summary = bundle(log_dir, args.since, Path(args.out), until=args.until,
                     cap_bytes=max(1, args.cap_bytes))
    print(json.dumps(summary), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
