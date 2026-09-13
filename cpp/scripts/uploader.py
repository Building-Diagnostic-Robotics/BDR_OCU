#!/usr/bin/env python3
"""
uploader.py — presigned-URL uploader for the BDR Coverage Planner.

Canonical copy. Runs on the OCU laptop against the RDATA_EXT thumb drive
(`UploadSource::ThumbDrive`, the production path) and is installed by the
.deb at /usr/share/bdr-coverage-planner/uploader.py. The robot-resident copy
under pilot_control/scripts/ is the legacy SSH path and is kept in sync by
hand — the runtime contract below is shared by both.

Invoked by `cpp/src/upload_runner.cpp`:

  python3 -u uploader.py <data_root> <robot_id> <run_id>

`data_root` is the absolute path of a single section or mission folder, e.g.
`/media/<user>/RDATA_EXT/January_27_2026/Acme_HQ/Section_1_093045` (or the
same folder under the robot's `/R_DATA/` in the legacy path). `run_id` is the
slash-encoded path of that folder relative to the data root
(e.g. `January_27_2026/Acme_HQ/Section_1_093045`). The OCU passes both because
the script must not try to derive `run_id` from `data_root` (mount points and
robot install layouts differ).

Runtime contract (parsed by `UploadRunner`):
 - All informational lines start with a stable prefix ("✓ Uploaded:",
   "Skipping already uploaded:", "Connection error:", "Manual pause detected.",
   "Uploading", "Generating manifest", "Upload complete:", "State cleaned up.")
 - Stdout is line-buffered (`python3 -u` AND explicit `flush=True`).
 - Exit codes: 0 = success, 1 = unexpected error, 2 = bad CLI args.

Configuration is read from environment variables so per-robot values from the
OCU's `robots.json` flow in (QProcessEnvironment locally, env(1) over SSH)
without editing the script:

  BDR_CLOUD_API_BASE     — e.g. https://abc.execute-api.us-east-1.amazonaws.com
  BDR_CLOUD_CLIENT_ID    — e.g. sig_roofing_ID
  BDR_CLOUD_DEVICE_TOKEN — e.g. roofus#0001
  BDR_UPLOAD_WORKERS     — optional override, default 12

State files live inside `data_root` — on the thumb drive itself — so they
persist across OCU laptops and a laptop reimage loses nothing:
  upload_state.json — list of relpath strings already PUT to S3.
  pause.flag        — operator-driven graceful pause sentinel.
  manifest.json     — written last; presence == "fully uploaded".

Resume model: the script is idempotent. Re-invoking with the same
`<data_root> <robot_id> <run_id>` skips files in `upload_state.json` and
re-issues `/complete` if `manifest.json` has not yet been written.
"""

import hashlib
import json
import os
import sys
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Dict, List, Optional, Tuple

try:
    import requests
except ImportError:
    # "Unexpected error:" so the OCU treats this as a hard failure and
    # surfaces it at once, rather than as a transient connection error
    # to be retried three times.
    print("Unexpected error: python3-requests is not installed on this "
          "machine (apt install python3-requests).", flush=True)
    sys.exit(1)


# =========================
# Configuration (env-driven)
# =========================

API_BASE = os.environ.get(
    "BDR_CLOUD_API_BASE",
    "https://zx8j0tqep2.execute-api.us-east-1.amazonaws.com",
).rstrip("/")
CLIENT_ID = os.environ.get("BDR_CLOUD_CLIENT_ID", "")
DEVICE_TOKEN = os.environ.get("BDR_CLOUD_DEVICE_TOKEN", "")

# 5 GB single-PUT ceiling, matches backend `/presign` schema. Files larger than
# this fail loud — the backend does not currently mint multipart presigns.
MAX_FILE_BYTES = 5_000_000_000
CHUNK_BYTES = 8 * 1024 * 1024

try:
    UPLOAD_WORKERS = max(1, int(os.environ.get("BDR_UPLOAD_WORKERS", "12")))
except ValueError:
    UPLOAD_WORKERS = 12

STATE_FILENAME = "upload_state.json"
PAUSE_FILENAME = "pause.flag"
MANIFEST_FILENAME = "manifest.json"


# =========================
# Utility: SHA256
# =========================

def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(CHUNK_BYTES)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


# =========================
# State Management
# =========================

def state_path(data_root: str) -> str:
    return os.path.join(data_root, STATE_FILENAME)


def pause_path(data_root: str) -> str:
    return os.path.join(data_root, PAUSE_FILENAME)


def manifest_path(data_root: str) -> str:
    return os.path.join(data_root, MANIFEST_FILENAME)


def load_state(data_root: str, robot_id: str, run_id: str) -> dict:
    path = state_path(data_root)
    if not os.path.exists(path):
        return {
            "client_id": CLIENT_ID,
            "robot_id": robot_id,
            "run_id": run_id,
            "completed": [],
        }
    try:
        with open(path, "r") as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        # Corrupt state file — start fresh; partial uploads will re-PUT to
        # the same S3 key (idempotent overwrite) so we don't lose data.
        return {
            "client_id": CLIENT_ID,
            "robot_id": robot_id,
            "run_id": run_id,
            "completed": [],
        }


def save_state(data_root: str, state: dict) -> None:
    """Atomic write: tmp + os.replace() so a crash mid-write never corrupts
    the on-disk state.  Same pattern used by data_collection_coordinator and
    finalize_mission_local."""
    target = state_path(data_root)
    tmp = target + ".tmp"
    with open(tmp, "w") as f:
        json.dump(state, f, indent=2)
        f.flush()
        try:
            os.fsync(f.fileno())
        except OSError:
            pass
    os.replace(tmp, target)


def should_pause(data_root: str) -> bool:
    return os.path.exists(pause_path(data_root))


# =========================
# API Calls
# =========================

def _auth_headers() -> Dict[str, str]:
    return {
        "x-client-id": CLIENT_ID,
        "x-device-token": DEVICE_TOKEN,
        "content-type": "application/json",
    }


def presign(robot_id: str, run_id: str, relpath: str, size_bytes: int) -> Dict:
    url = f"{API_BASE}/presign"
    payload = {
        "robot_id": robot_id,
        "run_id": run_id,
        "relpath": relpath.replace("\\", "/"),
        "size_bytes": int(size_bytes),
    }
    r = requests.post(url, headers=_auth_headers(), json=payload, timeout=30)
    r.raise_for_status()
    return r.json()


def complete(robot_id: str, run_id: str,
             manifest_relpath: str = MANIFEST_FILENAME) -> Dict:
    url = f"{API_BASE}/complete"
    payload = {
        "robot_id": robot_id,
        "run_id": run_id,
        "manifest_relpath": manifest_relpath,
    }
    r = requests.post(url, headers=_auth_headers(), json=payload, timeout=30)
    r.raise_for_status()
    return r.json()


# =========================
# Upload
# =========================

def upload_put(upload_url: str, file_path: str,
               robot_id: str, run_id: str) -> None:
    # The /presign Lambda mints SigV4 URLs that fold five headers into the
    # canonical request:
    #   host, x-amz-server-side-encryption,
    #   x-amz-meta-client_id, x-amz-meta-robot_id, x-amz-meta-run_id.
    # Every signed header MUST be echoed back on the PUT with the exact
    # value the Lambda used or S3 returns 403 SignatureDoesNotMatch.
    # `host` is set automatically by urllib3 from the URL; the other four
    # are set explicitly here. Keep the values in lock-step with the
    # Lambda — if the server-side signing list changes, this dict has to
    # change too.
    file_size = os.path.getsize(file_path)
    headers = {
        "x-amz-server-side-encryption": "AES256",
        "x-amz-meta-client_id": CLIENT_ID,
        "x-amz-meta-robot_id": robot_id,
        "x-amz-meta-run_id": run_id,
        "Content-Length": str(file_size),
    }
    if file_size == 0:
        # urllib3's body-framing logic falls back to
        # `Transfer-Encoding: chunked` when handed an empty file stream,
        # and S3 rejects chunked PUTs with 501 NotImplemented. Pass the
        # body as an explicit empty bytes object so urllib3 takes the
        # known-length code path and emits Content-Length: 0 instead.
        # 0-byte uploads are almost always salvage cases (recorder
        # crashed before the first chunk flushed) but the script must
        # tolerate them so a single dud file doesn't halt the whole
        # section's upload.
        r = requests.put(upload_url, data=b"", headers=headers, timeout=3600)
    else:
        with open(file_path, "rb") as f:
            r = requests.put(upload_url, data=f, headers=headers, timeout=3600)
    r.raise_for_status()


# =========================
# File Discovery
# =========================

def iter_files(root_dir: str):
    for base, _, files in os.walk(root_dir):
        for name in files:
            full = os.path.join(base, name)
            rel = os.path.relpath(full, root_dir)
            yield full, rel


# =========================
# Main Upload Flow
# =========================

def _upload_one_file(
    data_root: str,
    robot_id: str,
    run_id: str,
    state: dict,
    state_lock: threading.Lock,
    stop_event: threading.Event,
    full: str,
    rel_norm: str,
) -> Tuple[str, Optional[Exception]]:
    """Returns (status, error). status: ok | pause | conn_err | error | skip"""
    if stop_event.is_set():
        return ("skip", None)

    if should_pause(data_root):
        stop_event.set()
        return ("pause", None)

    size = os.path.getsize(full)
    if size > MAX_FILE_BYTES:
        err = RuntimeError(
            f"File exceeds 5GB single-PUT limit: {rel_norm} ({size} bytes)")
        stop_event.set()
        with state_lock:
            save_state(data_root, state)
        return ("error", err)

    try:
        # SHA256 is computed for the manifest at the end; compute up-front so
        # a corrupt-during-read disk error fails this file (not the manifest).
        sha256_file(full)

        presign_resp = presign(robot_id, run_id, rel_norm, size)
        upload_put(presign_resp["upload_url"], full, robot_id, run_id)

        with state_lock:
            state["completed"].append(rel_norm)
            save_state(data_root, state)

        print(f"\u2713 Uploaded: {rel_norm}", flush=True)
        return ("ok", None)

    except requests.exceptions.RequestException as e:
        stop_event.set()
        with state_lock:
            save_state(data_root, state)
        return ("conn_err", e)

    except Exception as e:
        stop_event.set()
        with state_lock:
            save_state(data_root, state)
        return ("error", e)


def upload_run(data_root: str, robot_id: str, run_id: str) -> int:
    if not CLIENT_ID:
        print("Connection error: BDR_CLOUD_CLIENT_ID env var unset.", flush=True)
        return 1
    if not DEVICE_TOKEN:
        print("Connection error: BDR_CLOUD_DEVICE_TOKEN env var unset.", flush=True)
        return 1
    if not API_BASE:
        print("Connection error: BDR_CLOUD_API_BASE env var unset.", flush=True)
        return 1
    if not os.path.isdir(data_root):
        print(f"Connection error: data_root does not exist: {data_root}", flush=True)
        return 1

    # Manifest existence == "previously fully uploaded". Re-running on a
    # done section is a no-op so the dialog can re-issue blindly without
    # blowing through bandwidth.
    if os.path.exists(manifest_path(data_root)):
        print(f"Already uploaded: {run_id}", flush=True)
        print("Upload complete: noop", flush=True)
        return 0

    state = load_state(data_root, robot_id, run_id)
    state["client_id"] = CLIENT_ID
    state["robot_id"] = robot_id
    state["run_id"] = run_id
    completed = set(state.get("completed", []))

    print(f"Resuming upload. {len(completed)} files already completed.", flush=True)

    pending: List[Tuple[str, str]] = []
    for full, rel in iter_files(data_root):
        rel_norm = rel.replace("\\", "/")
        if rel_norm in (STATE_FILENAME, PAUSE_FILENAME, MANIFEST_FILENAME):
            continue
        if rel_norm in completed:
            print(f"Skipping already uploaded: {rel_norm}", flush=True)
            continue
        pending.append((full, rel_norm))

    if should_pause(data_root):
        print("Manual pause detected. Stopping safely.", flush=True)
        return 0

    total_pending = len(pending)
    print(f"Uploading {total_pending} files with up to {UPLOAD_WORKERS} parallel workers.",
          flush=True)

    if total_pending > 1 and UPLOAD_WORKERS > 1:
        stop_event = threading.Event()
        state_lock = threading.Lock()
        executor = ThreadPoolExecutor(max_workers=UPLOAD_WORKERS)
        futures = {
            executor.submit(
                _upload_one_file,
                data_root,
                robot_id,
                run_id,
                state,
                state_lock,
                stop_event,
                full,
                rel_norm,
            ): rel_norm
            for full, rel_norm in pending
        }
        stop_early = False
        try:
            for fut in as_completed(futures):
                status, err = fut.result()
                if status in ("ok", "skip"):
                    continue
                if status == "pause":
                    print("Manual pause detected. Stopping safely.", flush=True)
                    stop_early = True
                    break
                if status == "conn_err":
                    print(f"Connection error: {err}", flush=True)
                    print("Auto-pausing. You can resume later.", flush=True)
                    stop_early = True
                    break
                if status == "error" and err is not None:
                    print(f"Unexpected error: {err}", flush=True)
                    return 1
        finally:
            try:
                executor.shutdown(wait=True, cancel_futures=stop_early)
            except TypeError:
                # Python <3.9 fallback (shouldn't hit on Ubuntu 22.04 but
                # keep behaviour identical to legacy script).
                executor.shutdown(wait=True)
        if stop_early:
            return 0
    else:
        for full, rel_norm in pending:
            if should_pause(data_root):
                print("Manual pause detected. Stopping safely.", flush=True)
                return 0
            size = os.path.getsize(full)
            if size > MAX_FILE_BYTES:
                print(
                    f"Unexpected error: File exceeds 5GB single-PUT limit: {rel_norm}",
                    flush=True)
                return 1
            print(f"Uploading: {rel_norm}", flush=True)
            try:
                sha256_file(full)
                presign_resp = presign(robot_id, run_id, rel_norm, size)
                upload_put(presign_resp["upload_url"], full, robot_id, run_id)
                state["completed"].append(rel_norm)
                save_state(data_root, state)
                print(f"\u2713 Uploaded: {rel_norm}", flush=True)
            except requests.exceptions.RequestException as e:
                print(f"Connection error: {e}", flush=True)
                print("Auto-pausing. You can resume later.", flush=True)
                save_state(data_root, state)
                return 0
            except Exception as e:
                print(f"Unexpected error: {e}", flush=True)
                save_state(data_root, state)
                return 1

    # Generate manifest.
    print("Generating manifest...", flush=True)
    manifest = {
        "client_id": CLIENT_ID,
        "robot_id": robot_id,
        "run_id": run_id,
        "files": [],
    }
    for full, rel in iter_files(data_root):
        rel_norm = rel.replace("\\", "/")
        if rel_norm in (STATE_FILENAME, PAUSE_FILENAME, MANIFEST_FILENAME):
            continue
        if rel_norm in state["completed"]:
            try:
                size = os.path.getsize(full)
                file_hash = sha256_file(full)
            except OSError as e:
                print(f"Unexpected error: cannot stat/hash {rel_norm}: {e}",
                      flush=True)
                return 1
            manifest["files"].append({
                "relpath": rel_norm,
                "s3_key": f"{CLIENT_ID}/{robot_id}/{run_id}/{rel_norm}",
                "size_bytes": size,
                "sha256": file_hash,
            })

    mpath = manifest_path(data_root)
    tmp = mpath + ".tmp"
    with open(tmp, "w") as f:
        json.dump(manifest, f, indent=2)
        f.flush()
        try:
            os.fsync(f.fileno())
        except OSError:
            pass
    os.replace(tmp, mpath)

    try:
        m_size = os.path.getsize(mpath)
        presign_resp = presign(robot_id, run_id, MANIFEST_FILENAME, m_size)
        upload_put(presign_resp["upload_url"], mpath, robot_id, run_id)
    except requests.exceptions.RequestException as e:
        print(f"Connection error: {e}", flush=True)
        print("Auto-pausing. You can resume later.", flush=True)
        # Manifest stays on disk; resume re-uploads it without re-PUTting
        # the data files (they're in state["completed"]).
        return 0

    try:
        done = complete(robot_id, run_id, MANIFEST_FILENAME)
        print(f"Upload complete: {json.dumps(done, separators=(',', ':'))}",
              flush=True)
    except requests.exceptions.RequestException as e:
        print(f"Connection error: {e}", flush=True)
        print("Auto-pausing. You can resume later.", flush=True)
        return 0

    # Local state cleanup. Manifest stays on disk so subsequent runs see
    # this section as fully uploaded.
    try:
        os.remove(state_path(data_root))
        print("State cleaned up.", flush=True)
    except OSError:
        pass

    return 0


def main() -> int:
    if len(sys.argv) != 4:
        print("Usage: uploader.py <data_root> <robot_id> <run_id>", flush=True)
        return 2

    data_root = sys.argv[1]
    robot_id = sys.argv[2]
    run_id = sys.argv[3]

    return upload_run(data_root, robot_id, run_id)


if __name__ == "__main__":
    sys.exit(main())
