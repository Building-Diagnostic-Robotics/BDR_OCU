#!/usr/bin/env python3
"""
uploader.py — presigned-URL uploader for the BDR Coverage Planner.

Canonical copy. Runs on the OCU laptop against the RDATA_EXT thumb drive
(`UploadSource::ThumbDrive`, the production path) and is installed by the
.deb at /usr/share/bdr-coverage-planner/uploader.py. The robot-resident copy
under pilot_control/scripts/ is the legacy SSH path and is kept in sync by
hand — the runtime contract below is shared by both. Keep the upload logic
in the two copies the same; only this header and the requests-import
message differ.

Invoked by `cpp/src/upload_runner.cpp`:

  python3 -u uploader.py <data_root> <robot_id> <run_id> [--force]

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
   "Uploading", "Generating manifest", "Upload complete:", "State cleaned up.",
   "Warning:", "Re-uploading:")
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
  upload_state.json — `completed` relpaths, plus `uploaded` mapping each
                      relpath to the size and md5 S3 accepted.
  pause.flag        — operator-driven graceful pause sentinel.
  manifest.json     — written last. It means fully uploaded only together
                      with the absence of upload_state.json, which is
                      removed after `/complete` succeeds. Each file entry
                      records size_bytes, sha256 and md5 (the S3 ETag).
  `--force` deletes only those bookkeeping files (and their `.tmp`
  siblings) and uploads the section again. It does not touch scan data
  or pause.flag.

Resume model: a file already in `uploaded` is skipped only when its local
size still matches. A size or content change is PUT again. `manifest.json`
with no `upload_state.json` is a no-op when every local file's size still
matches `size_bytes`; a mismatch deletes the manifest and uploads again.
A manifest left behind because `/complete` never finished (state file still
present) is not treated as done.
"""

import base64
import hashlib
import json
import os
import posixpath
import shlex
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Dict, List, NamedTuple, Optional, Tuple

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

# One warning per upload_run when /presign did not sign Content-MD5.
# Parallel workers share it.
_unsigned_md5_lock = threading.Lock()
_unsigned_md5_warned = False
# Set for the duration of one upload_run. None means this stick has no
# per-file record (copied before schema 2) and nothing is checked.
_copy_record = None
_stick_extra_warned = set()

# The robot this stick was copied from, for fixing a damaged stick file in
# place: `user@host`, set by the OCU when the robot is known. Empty means
# no fix; the operator is asked to plug the stick into the robot.
ROBOT_SSH = os.environ.get("BDR_ROBOT_SSH", "").strip()
ROBOT_DATA_ROOT = os.environ.get("BDR_ROBOT_DATA_ROOT", "").strip() or "/R_DATA"
# Over the radio a few MB/s is normal. Files above this wait for the robot.
ROBOT_FIX_MAX_BYTES = 1024 * 1024 * 1024
ROBOT_FIX_SUFFIX = ".bdr-robotfix"
_robot_fix_lock = threading.Lock()
# One failed connection per run is enough: no link, no point retrying per file.
_robot_unreachable = threading.Event()

try:
    UPLOAD_WORKERS = max(1, int(os.environ.get("BDR_UPLOAD_WORKERS", "12")))
except ValueError:
    UPLOAD_WORKERS = 12

STATE_FILENAME = "upload_state.json"
PAUSE_FILENAME = "pause.flag"
MANIFEST_FILENAME = "manifest.json"
# Written by the robot after it copies a mission to the stick. Schema 2
# carries a per-file sha256 taken from the robot's own disk.
THUMB_MANIFEST = "thumbsync_manifest.json"
# Written by the OCU after a cloud check. Bookkeeping, never scan data.
# A re-upload deletes it. A finished folder that no-ops keeps it: that
# file is the robot's proof the scan is already in the cloud.
CLOUD_VERIFIED_FILENAME = "cloud_verified.json"
# Written next to a mission's thumbsync_manifest.json by the laptop when
# the stick copy does not match the robot's record. The robot re-copies
# the listed files when the stick is plugged back in, then deletes it.
# Shared contract with pilot_control/scripts/rdata_thumbdrive_sync.py.
NEEDS_RECOPY_FILENAME = "needs_recopy.json"
_BOOKKEEPING_NAMES = frozenset((
    STATE_FILENAME,
    PAUSE_FILENAME,
    MANIFEST_FILENAME,
    CLOUD_VERIFIED_FILENAME,
    NEEDS_RECOPY_FILENAME,
    STATE_FILENAME + ".tmp",
    MANIFEST_FILENAME + ".tmp",
    CLOUD_VERIFIED_FILENAME + ".tmp",
    NEEDS_RECOPY_FILENAME + ".tmp",
))

# upload_state.json is saved after this many newly uploaded files, or
# this many seconds, whichever comes first, and always when a run stops.
# Rewriting it after every file costs O(files^2) bytes on the stick: a
# 45k-file section wrote hundreds of GB of state. A crash loses at most
# one batch of records, and those files are PUT again (same key, same
# bytes), which is harmless.
STATE_SAVE_EVERY_FILES = 200
STATE_SAVE_EVERY_S = 5.0
# Keys starting with "_" live only in memory and are never saved.
_PRIVATE_PREFIX = "_"


class CloudRejected(Exception):
    """The BDR API refused a request with a status a retry will not fix.

    401/403 from /presign or /complete mean the robot's credentials are
    wrong for every folder; the OCU stops the whole queue on them. Other
    4xx answers fail this folder only. 408, 429 and 5xx stay on the
    connection-error path and are retried.
    """

    def __init__(self, endpoint: str, status: int, detail: str = ""):
        self.endpoint = endpoint
        self.status = status
        text = f"Cloud refused {endpoint} (HTTP {status})"
        if detail:
            text += f": {detail}"
        super().__init__(text)


def _api_response(r, endpoint: str):
    """Raise CloudRejected for a non-retryable 4xx, else raise_for_status."""
    status = r.status_code
    if 400 <= status < 500 and status not in (408, 429):
        try:
            detail = (r.text or "").strip().replace("\n", " ")[:200]
        except Exception:
            detail = ""
        raise CloudRejected(endpoint, status, detail)
    r.raise_for_status()
    return r.json()


class UploadIntegrityError(Exception):
    """S3 did not accept the bytes we sent. Not a transient network error:
    retrying the same request would succeed, but the OCU must not mark the
    file uploaded, and it must not be auto-paused as a connection blip."""


# =========================
# Hashing
# =========================

def hash_file(path: str) -> Tuple[int, str, str]:
    """One read of `path` -> (byte count, md5 hex, sha256 hex).

    The byte count is what was read, not a separate stat, so the md5
    covers exactly the buffer we are about to send.
    """
    md5 = hashlib.md5()
    sha = hashlib.sha256()
    size = 0
    with open(path, "rb") as fh:
        while True:
            chunk = fh.read(CHUNK_BYTES)
            if not chunk:
                break
            size += len(chunk)
            md5.update(chunk)
            sha.update(chunk)
    return size, md5.hexdigest(), sha.hexdigest()


def _content_md5_b64(md5_hex: str) -> str:
    return base64.b64encode(bytes.fromhex(md5_hex)).decode("ascii")


def _response_md5(response) -> Optional[str]:
    """MD5 hex from an S3 ETag, or None when the ETag is not an MD5.

    SSE-S3 single-PUT ETags are the object MD5. Multipart ETags
    (`<hex>-<n>`), weak ETags and directory-bucket ETags are not, and
    must not be compared — a mismatch there would fail every upload.
    """
    raw = response.headers.get("ETag", "")
    if not isinstance(raw, str):
        return None
    raw = raw.strip()
    if len(raw) >= 2 and raw[0] in "Ww" and raw[1] == "/":
        return None
    if len(raw) >= 2 and raw[0] == '"' and raw[-1] == '"':
        raw = raw[1:-1]
    raw = raw.lower()
    if len(raw) == 32 and all(c in "0123456789abcdef" for c in raw):
        return raw
    return None


def _s3_error_code(response) -> str:
    try:
        text = response.text or ""
    except Exception:
        return ""
    start = text.find("<Code>")
    end = text.find("</Code>")
    if start < 0 or end <= start:
        return ""
    return text[start + len("<Code>"):end].strip()


# =========================
# State Management
# =========================

def _basename(rel_norm: str) -> str:
    slash = rel_norm.rfind("/")
    if slash < 0:
        return rel_norm
    return rel_norm[slash + 1:]


def is_sentinel(rel_norm: str) -> bool:
    """Bookkeeping files the uploader owns. Never uploaded as data.

    The `.tmp` siblings are the atomic-write leftovers of
    `upload_state.json`, `manifest.json` and `cloud_verified.json`.
    Any other `*.tmp` name is data — rosbag and capture files must
    not be dropped by a broad suffix rule.
    """
    name = _basename(rel_norm)
    return name in _BOOKKEEPING_NAMES or name.endswith(ROBOT_FIX_SUFFIX)


def state_path(data_root: str) -> str:
    return os.path.join(data_root, STATE_FILENAME)


def pause_path(data_root: str) -> str:
    return os.path.join(data_root, PAUSE_FILENAME)


def manifest_path(data_root: str) -> str:
    return os.path.join(data_root, MANIFEST_FILENAME)


def load_state(data_root: str, robot_id: str, run_id: str) -> dict:
    path = state_path(data_root)
    fresh = {
        "client_id": CLIENT_ID,
        "robot_id": robot_id,
        "run_id": run_id,
        "completed": [],
        "uploaded": {},
    }
    if not os.path.exists(path):
        return fresh
    try:
        with open(path, "r") as f:
            loaded = json.load(f)
    except (OSError, json.JSONDecodeError):
        # Corrupt state file — start fresh; partial uploads will re-PUT to
        # the same S3 key (idempotent overwrite) so we don't lose data.
        return fresh
    if not isinstance(loaded, dict):
        return fresh
    return loaded


def save_state(data_root: str, state: dict) -> None:
    """Atomic write: tmp + os.replace() so a crash mid-write never corrupts
    the on-disk state.  Same pattern used by data_collection_coordinator and
    finalize_mission_local."""
    target = state_path(data_root)
    tmp = target + ".tmp"
    public = {k: v for k, v in state.items()
              if not (isinstance(k, str) and k.startswith(_PRIVATE_PREFIX))}
    with open(tmp, "w") as f:
        json.dump(public, f, indent=2)
        f.flush()
        try:
            os.fsync(f.fileno())
        except OSError:
            pass
    os.replace(tmp, target)
    state["_unsaved"] = 0
    state["_saved_at"] = time.monotonic()


def note_progress(data_root: str, state: dict) -> None:
    """Count one new record and save when a batch is due. Hold the
    state lock when calling from a worker."""
    state["_unsaved"] = int(state.get("_unsaved") or 0) + 1
    last = state.get("_saved_at")
    if last is None:
        state["_saved_at"] = last = time.monotonic()
    if (state["_unsaved"] >= STATE_SAVE_EVERY_FILES
            or time.monotonic() - last >= STATE_SAVE_EVERY_S):
        save_state(data_root, state)


def flush_state(data_root: str, state: dict) -> None:
    """Save when anything is unsaved. Every exit path of a run calls this."""
    if int(state.get("_unsaved") or 0) > 0:
        save_state(data_root, state)


def should_pause(data_root: str) -> bool:
    return os.path.exists(pause_path(data_root))


def remove_named(data_root: str, names) -> Optional[str]:
    """Remove each name. Missing files are fine. Returns an error string
    when a file is still there afterwards."""
    errors = []
    for name in names:
        path = os.path.join(data_root, name)
        try:
            os.remove(path)
        except FileNotFoundError:
            continue
        except OSError as exc:
            errors.append(f"could not remove {name}: {exc}")
    if errors:
        return "; ".join(errors)
    return None


def remove_stale_temps(data_root: str) -> None:
    """Drop atomic-write leftovers from a killed run. A live save uses
    the same names, so this only runs when this process is the uploader.

    `cloud_verified.json` itself is not a leftover: a no-op re-run of a
    finished folder must keep the robot's proof."""
    err = remove_named(data_root, (
        STATE_FILENAME + ".tmp",
        MANIFEST_FILENAME + ".tmp",
        CLOUD_VERIFIED_FILENAME + ".tmp",
    ))
    if err:
        print(f"Warning: {err}", flush=True)


def remove_cloud_verified(data_root: str) -> None:
    """Drop the cloud-check proof and a torn temp file.

    Called only when this run will rewrite the upload. A hash mismatch
    would already make the robot ignore the file; deleting it means a
    crash mid-upload cannot leave a stale proof behind."""
    err = remove_named(data_root, (
        CLOUD_VERIFIED_FILENAME,
        CLOUD_VERIFIED_FILENAME + ".tmp",
    ))
    if err:
        print(f"Warning: {err}", flush=True)


def _discard_manifest(data_root: str) -> Optional[str]:
    """Remove a manifest that must not be treated as 'uploaded'.
    Returns an error string when the file is still there afterwards."""
    path = manifest_path(data_root)
    try:
        os.remove(path)
    except FileNotFoundError:
        return None
    except OSError as exc:
        return f"could not remove {MANIFEST_FILENAME}: {exc}"
    return None


def discard_bookkeeping(data_root: str) -> Optional[str]:
    """Remove the uploader's own state, manifest, cloud proof and `.tmp`
    siblings.

    Used by `--force` before a fresh upload. Scan data and `pause.flag`
    stay. Returns an error string when a file is still present.
    """
    return remove_named(data_root, (
        MANIFEST_FILENAME, STATE_FILENAME, CLOUD_VERIFIED_FILENAME,
        MANIFEST_FILENAME + ".tmp", STATE_FILENAME + ".tmp",
        CLOUD_VERIFIED_FILENAME + ".tmp",
    ))


def parse_cli(argv: List[str]) -> Optional[Tuple[str, str, str, bool]]:
    """(data_root, robot_id, run_id, force) or None when argv is wrong.

    `--force` is only accepted as the final argument so a path cannot
    be mistaken for the flag.
    """
    args = list(argv)
    force = False
    if args and args[-1] == "--force":
        force = True
        args = args[:-1]
    if len(args) != 3:
        return None
    return args[0], args[1], args[2], force


def stored_record(state: dict, rel_norm: str) -> Optional[Tuple[int, str]]:
    """(size, md5) S3 accepted for `rel_norm`, or None if unverified.

    Records written by older builds have a `completed` list and no
    `uploaded` map. Those return None so the file is PUT again once
    under the size check.
    """
    uploaded = state.get("uploaded")
    if not isinstance(uploaded, dict):
        return None
    rec = uploaded.get(rel_norm)
    if not isinstance(rec, dict):
        return None
    size = rec.get("size")
    md5_hex = rec.get("md5")
    # `type is int` rejects bool, which is a subclass of int.
    if type(size) is not int or size < 0:
        return None
    if not isinstance(md5_hex, str) or len(md5_hex) != 32:
        return None
    if any(c not in "0123456789abcdef" for c in md5_hex):
        return None
    return size, md5_hex


def already_uploaded(state: dict, full: str, rel_norm: str) -> bool:
    rec = stored_record(state, rel_norm)
    if rec is None:
        return False
    try:
        return os.path.getsize(full) == rec[0]
    except OSError:
        return False


def record_uploaded(state: dict, rel_norm: str, size: int, md5_hex: str) -> None:
    uploaded = state.get("uploaded")
    if not isinstance(uploaded, dict):
        uploaded = {}
        state["uploaded"] = uploaded
    uploaded[rel_norm] = {"size": size, "md5": md5_hex}
    completed = state.get("completed")
    if not isinstance(completed, list):
        state["completed"] = [rel_norm]
        state["_completed_set"] = {rel_norm}
        return
    seen = _completed_set(state)
    if rel_norm not in seen:
        completed.append(rel_norm)
        seen.add(rel_norm)


def _completed_set(state: dict) -> set:
    """Membership index for `completed`, built once per run. A list scan
    per file was O(files^2) on large sections."""
    seen = state.get("_completed_set")
    if not isinstance(seen, set):
        completed = state.get("completed")
        seen = set(c for c in completed if isinstance(c, str)) \
            if isinstance(completed, list) else set()
        state["_completed_set"] = seen
    return seen


def forget_upload(state: dict, rel_norm: str) -> None:
    """Drop a record so the next run PUTs the file again."""
    uploaded = state.get("uploaded")
    if isinstance(uploaded, dict):
        uploaded.pop(rel_norm, None)
    completed = state.get("completed")
    if isinstance(completed, list):
        state["completed"] = [rel for rel in completed if rel != rel_norm]
    seen = state.get("_completed_set")
    if isinstance(seen, set):
        seen.discard(rel_norm)


def _sample(names: List[str]) -> str:
    shown = ", ".join(names[:5])
    extra = len(names) - 5
    if extra > 0:
        return f"{shown} (+{extra} more)"
    return shown


# =========================
# API Calls
# =========================

def _auth_headers() -> Dict[str, str]:
    return {
        "x-client-id": CLIENT_ID,
        "x-device-token": DEVICE_TOKEN,
        "content-type": "application/json",
    }


def presign(robot_id: str, run_id: str, relpath: str, size_bytes: int,
            content_md5: str) -> Dict:
    # content_md5 is the base64 MD5 of the bytes about to be sent. The
    # Lambda signs it into the URL when it understands the field, and
    # says so with md5_signed. Older Lambdas ignore the field.
    url = f"{API_BASE}/presign"
    payload = {
        "robot_id": robot_id,
        "run_id": run_id,
        "relpath": relpath.replace("\\", "/"),
        "size_bytes": int(size_bytes),
        "content_md5": content_md5,
    }
    r = requests.post(url, headers=_auth_headers(), json=payload, timeout=30)
    return _api_response(r, "/presign")


def complete(robot_id: str, run_id: str,
             manifest_relpath: str = MANIFEST_FILENAME) -> Dict:
    url = f"{API_BASE}/complete"
    payload = {
        "robot_id": robot_id,
        "run_id": run_id,
        "manifest_relpath": manifest_relpath,
    }
    r = requests.post(url, headers=_auth_headers(), json=payload, timeout=30)
    return _api_response(r, "/complete")


def _upload_url(presign_resp, rel_norm: str) -> str:
    if not isinstance(presign_resp, dict):
        raise RuntimeError(f"presign response for {rel_norm} was not a JSON object")
    upload_url = presign_resp.get("upload_url")
    if not isinstance(upload_url, str) or not upload_url:
        raise RuntimeError(f"presign response missing upload_url for {rel_norm}")
    return upload_url


# =========================
# Upload
# =========================

class _ExactReader:
    """Non-seekable reader of exactly `size` bytes.

    requests 2.x classifies a body with `__iter__` as a stream and then
    overwrites Content-Length with `super_len()`. `super_len` prefers
    `__len__` over a stat of the live file, so the length we hashed is
    the length that is sent. The object is not seekable, which stops
    requests from statting the file again and picking up a growth that
    the MD5 does not cover. `__iter__` is only there for that
    classification — the socket path calls `read()`.

    `read()` stops after `size` bytes, so a file that grows mid-PUT
    cannot append bytes past the Content-MD5. A short read raises
    instead of letting http.client send a short body under a longer
    Content-Length.
    """

    def __init__(self, fh, size: int):
        self._fh = fh
        self._size = size
        self._left = size

    def __len__(self) -> int:
        return self._size

    def __iter__(self):
        raise TypeError("upload body is consumed via read(), not iteration")

    def read(self, n: int = -1) -> bytes:
        if self._left <= 0:
            return b""
        if n is None or n < 0 or n > self._left:
            n = self._left
        chunk = self._fh.read(n)
        if not chunk:
            raise UploadIntegrityError(
                f"file ended early ({self._left} bytes still unread)")
        if len(chunk) > n:
            chunk = chunk[:n]
        self._left -= len(chunk)
        return chunk


def upload_put(upload_url: str, file_path: str,
               robot_id: str, run_id: str,
               md5_hex: str, size: int,
               content_md5: Optional[str]) -> None:
    # The /presign Lambda signs these headers into the URL:
    #   host, x-amz-server-side-encryption,
    #   x-amz-meta-client_id, x-amz-meta-robot_id, x-amz-meta-run_id,
    #   and Content-MD5 when the request carried content_md5 and the
    #   response says md5_signed.
    # Every signed header MUST be echoed back on the PUT with the exact
    # value the Lambda used or S3 returns 403 SignatureDoesNotMatch.
    # `host` is set automatically by urllib3 from the URL. Keep the
    # values in lock-step with the Lambda.
    #
    # Content-MD5 is sent only when `content_md5` is set, which is only
    # when the URL signed it. S3 rejects an unsigned Content-MD5 with
    # 403 HeadersNotSigned, before it looks at the body. When it is
    # signed, a body that does not match is rejected with 400 BadDigest
    # and nothing is stored. The ETag check below still runs either way,
    # and it is the whole check when the Lambda did not sign.
    if size < 0:
        raise RuntimeError(f"negative upload size for {file_path}")
    headers = {
        "x-amz-server-side-encryption": "AES256",
        "x-amz-meta-client_id": CLIENT_ID,
        "x-amz-meta-robot_id": robot_id,
        "x-amz-meta-run_id": run_id,
        "Content-Length": str(size),
    }
    if content_md5 is not None:
        headers["Content-MD5"] = content_md5
    if size == 0:
        # urllib3's body-framing logic falls back to
        # `Transfer-Encoding: chunked` when handed an empty file stream,
        # and S3 rejects chunked PUTs with 501 NotImplemented. Pass the
        # body as an explicit empty bytes object so urllib3 takes the
        # known-length code path and emits Content-Length: 0 instead.
        # 0-byte uploads are almost always salvage cases (recorder
        # crashed before the first chunk flushed) but the script must
        # tolerate them so a single dud file doesn't halt the whole
        # section's upload. Content-MD5 of the empty body still has to
        # match, so an empty PUT cannot stand in for a non-empty file.
        r = requests.put(upload_url, data=b"", headers=headers, timeout=3600)
    else:
        with open(file_path, "rb") as fh:
            reader = _ExactReader(fh, size)
            r = requests.put(upload_url, data=reader, headers=headers, timeout=3600)
    code = _s3_error_code(r)
    if r.status_code == 400 and code in ("BadDigest", "InvalidDigest"):
        raise UploadIntegrityError(
            f"S3 rejected {file_path}: stored bytes did not match the upload "
            f"({code}, {size} bytes, md5 {md5_hex})")
    # A signature mismatch will not start working on retry. Expired-URL
    # 403s use a different code and stay on the connection-error path.
    if r.status_code == 403 and code == "SignatureDoesNotMatch":
        raise UploadIntegrityError(
            f"S3 rejected the upload signature for {file_path} "
            f"({size} bytes). A signed header does not match the presign.")
    r.raise_for_status()
    # SSE-S3 single-PUT ETags are the MD5 of the stored object. A 200
    # with no MD5, or with the MD5 of a different body (an empty object
    # while the file is not empty), must not be recorded as uploaded.
    etag = _response_md5(r)
    if etag is None:
        raise UploadIntegrityError(
            f"S3 response for {file_path} did not include an MD5 ETag "
            f"({size} bytes, local md5 {md5_hex})")
    if etag != md5_hex:
        raise UploadIntegrityError(
            f"S3 stored different bytes than were sent for {file_path} "
            f"(etag {etag}, local md5 {md5_hex}, {size} bytes)")


def _reset_unsigned_md5_warning() -> None:
    global _unsigned_md5_warned
    with _unsigned_md5_lock:
        _unsigned_md5_warned = False


def _warn_unsigned_md5() -> None:
    global _unsigned_md5_warned
    with _unsigned_md5_lock:
        if _unsigned_md5_warned:
            return
        _unsigned_md5_warned = True
    print("Warning: /presign did not sign Content-MD5; "
          "each file is still checked against the S3 ETag.", flush=True)


def _presign_and_put(robot_id: str, run_id: str, rel_norm: str,
                     file_path: str, md5_hex: str, size: int) -> None:
    """Mint a URL for these exact bytes and PUT them.

    The base64 MD5 is computed once and sent to /presign. It is echoed
    as Content-MD5 only when the Lambda reports md5_signed, because S3
    rejects the header on a URL that did not sign it.
    """
    content_md5 = _content_md5_b64(md5_hex)
    presign_resp = presign(robot_id, run_id, rel_norm, size, content_md5)
    upload_url = _upload_url(presign_resp, rel_norm)
    if presign_resp.get("md5_signed") is True:
        upload_put(upload_url, file_path, robot_id, run_id,
                   md5_hex, size, content_md5)
        return
    _warn_unsigned_md5()
    upload_put(upload_url, file_path, robot_id, run_id,
               md5_hex, size, None)


def put_verified_file(robot_id: str, run_id: str,
                      full: str, rel_norm: str) -> Tuple[int, str]:
    """Hash, PUT, and confirm S3 stored those bytes.

    Returns (size, md5). Raises UploadIntegrityError when the file
    changes around the PUT or S3's response does not match, so the
    caller does not record the file.
    """
    try:
        stamped = os.path.getsize(full)
    except OSError as exc:
        raise UploadIntegrityError(f"cannot stat {rel_norm}: {exc}") from exc
    if stamped > MAX_FILE_BYTES:
        raise RuntimeError(
            f"File exceeds 5GB single-PUT limit: {rel_norm} ({stamped} bytes)")
    try:
        size, md5_hex, sha = hash_file(full)
    except OSError as exc:
        raise UploadIntegrityError(f"cannot read {rel_norm}: {exc}") from exc
    if size > MAX_FILE_BYTES:
        raise RuntimeError(
            f"File exceeds 5GB single-PUT limit: {rel_norm} ({size} bytes)")
    try:
        current = os.path.getsize(full)
    except OSError as exc:
        raise UploadIntegrityError(f"cannot stat {rel_norm}: {exc}") from exc
    if current != size:
        raise UploadIntegrityError(
            f"{rel_norm} changed before upload "
            f"(read {size} bytes, file is now {current} bytes)")
    if size == 0:
        print(f"Warning: {rel_norm} is 0 bytes; uploading it empty.", flush=True)
    try:
        _check_stick_copy(rel_norm, size, sha)
    except StickCopyMismatch:
        # Fix it from the robot when the link is up, then read it again.
        if not repair_from_robot(full, rel_norm, _copy_record):
            raise
        try:
            size, md5_hex, sha = hash_file(full)
        except OSError as exc:
            raise UploadIntegrityError(f"cannot read {rel_norm}: {exc}") from exc
        _check_stick_copy(rel_norm, size, sha)
    _presign_and_put(robot_id, run_id, rel_norm, full, md5_hex, size)
    try:
        after = os.path.getsize(full)
    except OSError as exc:
        raise UploadIntegrityError(f"cannot stat {rel_norm}: {exc}") from exc
    if after != size:
        raise UploadIntegrityError(
            f"{rel_norm} changed during upload "
            f"(sent {size} bytes, file is now {after} bytes)")
    return size, md5_hex


# =========================
# File Discovery
# =========================

def iter_files(root_dir: str):
    for base, _, files in os.walk(root_dir):
        for name in files:
            full = os.path.join(base, name)
            rel = os.path.relpath(full, root_dir)
            yield full, rel


# Added to a mission folder on the stick by the OCU laptop after the robot's
# copy: the OCU mission log and the laptop's ROS logs for that mission. Not
# in the robot's copy record by design, still uploaded and listed in the
# cloud manifest. Plain files directly under this folder only; ".part" is an
# interrupted laptop copy and is never uploaded.
OCU_LOG_PREFIX = "logs/ocu/"


def is_ocu_log(rel_norm: str) -> bool:
    if not rel_norm.startswith(OCU_LOG_PREFIX):
        return False
    name = rel_norm[len(OCU_LOG_PREFIX):]
    return bool(name) and "/" not in name and not name.startswith(".") \
        and not name.endswith(".part")


def _stick_extra(rel_norm: str) -> bool:
    """True when the robot's record does not include this file.

    thumbsync_manifest.json is the robot's own provenance and is still
    uploaded, and so are the OCU's logs (is_ocu_log). Each other extra file
    is named once per run.
    """
    if _copy_record is None or rel_norm in _copy_record.by_rel:
        return False
    if rel_norm == THUMB_MANIFEST or is_ocu_log(rel_norm):
        return False
    if rel_norm not in _stick_extra_warned:
        _stick_extra_warned.add(rel_norm)
        print(f"Warning: {rel_norm} is not in the robot's copy record; "
              f"not uploading it.", flush=True)
    return True


def data_files(data_root: str) -> List[Tuple[str, str]]:
    found: List[Tuple[str, str]] = []
    for full, rel in iter_files(data_root):
        rel_norm = rel.replace("\\", "/")
        if is_sentinel(rel_norm) or _stick_extra(rel_norm):
            continue
        found.append((full, rel_norm))
    return found


def manifest_size_drift(data_root: str) -> Optional[str]:
    """None when `manifest.json` still describes the local files.

    Manifests that record no `size_bytes` (older script) are trusted:
    there is nothing to compare. A present size that disagrees, a local
    file the manifest does not list, or a listed file that is gone is a
    reason to delete the manifest and upload again.
    """
    path = manifest_path(data_root)
    try:
        with open(path, "r") as fh:
            manifest = json.load(fh)
    except (OSError, json.JSONDecodeError) as exc:
        return f"{MANIFEST_FILENAME} is unreadable ({exc})"
    if not isinstance(manifest, dict):
        return f"{MANIFEST_FILENAME} is not a JSON object"
    files = manifest.get("files")
    if not isinstance(files, list):
        return f"{MANIFEST_FILENAME} has no file list"

    entries = []
    for entry in files:
        if not isinstance(entry, dict):
            continue
        rel = entry.get("relpath")
        if not isinstance(rel, str) or is_sentinel(rel):
            continue
        entries.append(entry)
    sized = []
    for entry in entries:
        size = entry.get("size_bytes")
        if type(size) is int and size >= 0:
            sized.append(entry)
    if entries and not sized:
        return None
    if len(sized) != len(entries):
        return f"{MANIFEST_FILENAME} is missing size_bytes for some files"
    recorded = {entry["relpath"]: entry["size_bytes"] for entry in sized}

    seen = set()
    for full, rel_norm in data_files(data_root):
        seen.add(rel_norm)
        if rel_norm not in recorded:
            return f"{rel_norm} is not in the uploaded manifest"
        try:
            size = os.path.getsize(full)
        except OSError as exc:
            return f"cannot stat {rel_norm}: {exc}"
        if size != recorded[rel_norm]:
            return (f"{rel_norm} is {size} bytes on the drive, "
                    f"manifest recorded {recorded[rel_norm]}")
    missing = sorted(rel for rel in recorded if rel not in seen)
    if missing:
        verb = "is" if len(missing) == 1 else "are"
        return (f"{_sample(missing)} {verb} in the manifest but no longer "
                "on the drive")
    return None


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
) -> Tuple[str, Optional[BaseException]]:
    """Returns (status, error). status: ok | pause | conn_err | error | skip"""
    if stop_event.is_set():
        return ("skip", None)

    if should_pause(data_root):
        stop_event.set()
        return ("pause", None)

    try:
        size, md5_hex = put_verified_file(robot_id, run_id, full, rel_norm)
        with state_lock:
            record_uploaded(state, rel_norm, size, md5_hex)
            note_progress(data_root, state)
    except StickCopyMismatch as exc:
        stop_event.set()
        with state_lock:
            save_state(data_root, state)
        return ("stick", exc)
    except UploadIntegrityError as exc:
        stop_event.set()
        with state_lock:
            save_state(data_root, state)
        return ("error", exc)
    except requests.exceptions.RequestException as exc:
        stop_event.set()
        with state_lock:
            save_state(data_root, state)
        return ("conn_err", exc)
    except Exception as exc:
        stop_event.set()
        with state_lock:
            save_state(data_root, state)
        return ("error", exc)

    print(f"\u2713 Uploaded: {rel_norm}", flush=True)
    return ("ok", None)


def _finish_one(status: str, err: Optional[BaseException]) -> Optional[int]:
    """Map one file's status to an upload_run exit code, or None to go on."""
    if status in ("ok", "skip"):
        return None
    if status == "pause":
        print("Manual pause detected. Stopping safely.", flush=True)
        return 0
    if status == "conn_err":
        print(f"Connection error: {err}", flush=True)
        print("Auto-pausing. You can resume later.", flush=True)
        return 0
    if status == "stick" and isinstance(err, StickCopyMismatch):
        _report_bad_stick_copy([err.rel_norm], err.record)
        return 1
    print(f"Unexpected error: {err}", flush=True)
    return 1


def _confirmed_entries(data_root: str, state: dict) -> Optional[List[dict]]:
    """Manifest file entries for data that still matches the upload record.

    Returns None after reporting an error (and forgetting any record
    that no longer matches, so the next run PUTs those files again).
    """
    on_disk = data_files(data_root)
    disk = {rel for _full, rel in on_disk}
    completed = state.get("completed")
    if isinstance(completed, list):
        gone = [
            rel for rel in completed
            if isinstance(rel, str) and not is_sentinel(rel) and rel not in disk
        ]
        if gone:
            for rel in gone:
                forget_upload(state, rel)
            save_state(data_root, state)
            print(
                "Warning: uploaded earlier but no longer on the drive: "
                f"{_sample(gone)}",
                flush=True)

    mismatched: List[str] = []
    unrecorded: List[str] = []
    entries: List[dict] = []
    for full, rel_norm in on_disk:
        rec = stored_record(state, rel_norm)
        if rec is None:
            unrecorded.append(rel_norm)
            continue
        rec_size, rec_md5 = rec
        try:
            size, md5_hex, sha = hash_file(full)
        except OSError as exc:
            print(f"Unexpected error: cannot read {rel_norm}: {exc}", flush=True)
            return None
        if size != rec_size or md5_hex != rec_md5:
            mismatched.append(rel_norm)
            forget_upload(state, rel_norm)
            continue
        entries.append({
            "relpath": rel_norm,
            "s3_key": f"{CLIENT_ID}/{state.get('robot_id', '')}/{state.get('run_id', '')}/{rel_norm}",
            "size_bytes": size,
            "sha256": sha,
            "md5": md5_hex,
        })

    if mismatched or unrecorded:
        if mismatched:
            save_state(data_root, state)
        parts = []
        if mismatched:
            parts.append(
                f"changed after upload: {_sample(mismatched)}")
        if unrecorded:
            parts.append(
                f"not uploaded yet: {_sample(unrecorded)}")
        print(f"Unexpected error: {'; '.join(parts)}", flush=True)
        return None
    return entries


def _write_manifest(data_root: str, manifest: dict) -> str:
    mpath = manifest_path(data_root)
    tmp = mpath + ".tmp"
    with open(tmp, "w") as fh:
        json.dump(manifest, fh, indent=2)
        fh.flush()
        try:
            os.fsync(fh.fileno())
        except OSError:
            pass
    os.replace(tmp, mpath)
    return mpath


def _upload_manifest_and_complete(data_root: str, robot_id: str, run_id: str):
    """PUT manifest.json and call /complete.

    On any failure the local manifest is removed. Leaving it in place
    made the next run take the 'already uploaded' path, so a dropped
    manifest PUT or a failed /complete was reported as success.
    """
    mpath = manifest_path(data_root)
    try:
        m_size, m_md5, _sha = hash_file(mpath)
        try:
            current = os.path.getsize(mpath)
        except OSError as exc:
            raise UploadIntegrityError(
                f"cannot stat {MANIFEST_FILENAME}: {exc}") from exc
        if current != m_size:
            raise UploadIntegrityError(
                f"{MANIFEST_FILENAME} changed while it was being read")
        _presign_and_put(robot_id, run_id, MANIFEST_FILENAME, mpath,
                         m_md5, m_size)
        return complete(robot_id, run_id, MANIFEST_FILENAME)
    except Exception as exc:
        disc = _discard_manifest(data_root)
        if disc:
            raise UploadIntegrityError(
                f"{disc} after a failed manifest upload ({exc}). "
                f"Delete {MANIFEST_FILENAME} before resuming, or this "
                "section will be treated as uploaded."
            ) from exc
        raise


def _run_pending(data_root: str, robot_id: str, run_id: str,
                 state: dict, pending: List[Tuple[str, str]]) -> Optional[int]:
    """Upload `pending`. Returns an exit code to stop with, or None.

    upload_state.json is always saved before this returns, whatever the
    outcome, so a pause or a stop never loses the last batch.
    """
    try:
        return _run_pending_inner(data_root, robot_id, run_id, state, pending)
    finally:
        try:
            flush_state(data_root, state)
        except OSError as exc:
            print(f"Warning: could not save {STATE_FILENAME}: {exc}", flush=True)


def _run_pending_inner(data_root: str, robot_id: str, run_id: str,
                       state: dict, pending: List[Tuple[str, str]]) -> Optional[int]:
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
                data_root, robot_id, run_id, state,
                state_lock, stop_event, full, rel_norm,
            ): rel_norm
            for full, rel_norm in pending
        }
        exit_code: Optional[int] = None
        try:
            for fut in as_completed(futures):
                try:
                    status, err = fut.result()
                except Exception as exc:
                    # A worker that escapes its own handler (a failure
                    # inside the result hand-off) is still a hard stop.
                    status, err = ("error", exc)
                exit_code = _finish_one(status, err)
                if exit_code is not None:
                    break
        finally:
            # cancel_futures drops work that has not started. In-flight
            # PUTs finish; stop_event keeps them from starting the next
            # one. Python < 3.9 has no cancel_futures.
            try:
                executor.shutdown(wait=True, cancel_futures=exit_code is not None)
            except TypeError:
                executor.shutdown(wait=True)
        return exit_code

    for full, rel_norm in pending:
        print(f"Uploading: {rel_norm}", flush=True)
        status, err = _upload_one_file(
            data_root, robot_id, run_id, state,
            threading.Lock(), threading.Event(), full, rel_norm)
        exit_code = _finish_one(status, err)
        if exit_code is not None:
            return exit_code
    return None


class CopyRecord(NamedTuple):
    """Per-file sha256 the robot recorded for this one folder."""

    by_rel: dict
    mission_rel: str
    # Absolute Mission_* folder holding the record; needs_recopy.json
    # goes there.
    mission_dir: str = ""
    # This folder's name, the prefix of its keys in the record.
    folder: str = ""
    # The robot's building folder (`/R_DATA/<date>/<building>`), from the
    # record's `mission_folder`. Empty on records that do not carry it.
    robot_building: str = ""


class StickCopyMismatch(UploadIntegrityError):
    """A stick file's bytes differ from the robot's record."""

    def __init__(self, rel_norm: str, record: "CopyRecord"):
        self.rel_norm = rel_norm
        self.record = record
        super().__init__(
            f"Stick copy of {rel_norm} does not match the robot. "
            + _stick_recovery(record))


class CopyUnfinished(UploadIntegrityError):
    """The robot's copy of this folder never wrote its record."""


def _read_json_object(path: str):
    try:
        with open(path, "r") as fh:
            doc = json.load(fh)
    except (OSError, json.JSONDecodeError):
        return None
    if not isinstance(doc, dict):
        return None
    return doc


def _mission_relpath(manifest_path: str) -> str:
    """`<date>/<building>/Mission_*` for the operator's recovery step."""
    mission = os.path.dirname(os.path.abspath(manifest_path))
    building = os.path.dirname(mission)
    day = os.path.dirname(building)
    parts = (os.path.basename(day), os.path.basename(building),
             os.path.basename(mission))
    if all(parts):
        return "/".join(parts)
    return mission


def find_thumb_manifest(data_root: str) -> Optional[str]:
    """This folder's thumbsync manifest, or None when the stick has none.

    A mission folder carries its own. A section folder uses the sibling
    Mission_* manifest that lists it. Two missions listing the same
    section is an error: there is no safe record to trust. A sibling
    manifest that cannot be read is also an error: it cannot say which
    sections it covers, so every section in the building folder stops.
    """
    root = os.path.abspath(data_root)
    own = os.path.join(root, THUMB_MANIFEST)
    if os.path.isfile(own):
        return own
    folder = os.path.basename(root)
    try:
        names = os.listdir(os.path.dirname(root))
    except OSError:
        return None
    matches = []
    unreadable = []
    for name in names:
        if not name.startswith("Mission_"):
            continue
        path = os.path.join(os.path.dirname(root), name, THUMB_MANIFEST)
        if not os.path.isfile(path):
            continue
        doc = _read_json_object(path)
        if doc is None:
            unreadable.append(path)
            continue
        sections = doc.get("sections")
        if isinstance(sections, list) and folder in sections:
            matches.append(path)
    if unreadable:
        rels = ", ".join(_mission_relpath(path) for path in unreadable)
        raise UploadIntegrityError(
            f"Copy record unreadable for {rels}, so it cannot say which "
            f"sections it covers. Plug the stick into the robot to "
            f"re-copy {rels}.")
    if len(matches) > 1:
        raise UploadIntegrityError(
            f"More than one mission on the stick lists {folder}; "
            "cannot tell which copy record to trust.")
    if matches:
        return matches[0]
    return None


def building_has_copy_records(data_root: str) -> bool:
    """True when a sibling Mission_* holds a per-file (schema 2) record.

    On such a stick every copied folder is listed by a record, so one
    that is not was never finished: the robot writes the record last.
    A stick with no per-file records (copied by an older robot) keeps
    the old rule and uploads as before.
    """
    parent = os.path.dirname(os.path.abspath(data_root))
    try:
        names = os.listdir(parent)
    except OSError:
        return False
    for name in names:
        if not name.startswith("Mission_"):
            continue
        doc = _read_json_object(os.path.join(parent, name, THUMB_MANIFEST))
        if doc is not None and isinstance(doc.get("files"), dict):
            return True
    return False


def _parse_copy_files(files: dict, folder: str) -> dict:
    prefix = folder + "/"
    by_rel = {}
    for key, rec in files.items():
        if not isinstance(key, str) or not key.startswith(prefix):
            continue
        rel = key[len(prefix):]
        if not rel or rel.startswith("/") or ".." in rel.split("/"):
            raise UploadIntegrityError(
                f"{THUMB_MANIFEST} has an invalid path {key!r}.")
        if not isinstance(rec, dict):
            raise UploadIntegrityError(
                f"{THUMB_MANIFEST} entry for {key} is not an object.")
        size = rec.get("size")
        sha = rec.get("sha256")
        sha_ok = (isinstance(sha, str) and len(sha) == 64
                  and all(c in "0123456789abcdef" for c in sha.lower()))
        if type(size) is not int or size < 0 or not sha_ok:
            raise UploadIntegrityError(
                f"{THUMB_MANIFEST} entry for {key} has no size and sha256.")
        by_rel[rel] = (size, sha.lower())
    return by_rel


def load_copy_record(data_root: str) -> Optional[CopyRecord]:
    """The robot's record for this folder, or None on a pre-schema-2 stick.

    None means upload as before. A record is enforced: a bad manifest
    raises instead of being treated as "no record".
    """
    path = find_thumb_manifest(data_root)
    folder = os.path.basename(os.path.abspath(data_root))
    if path is None:
        if building_has_copy_records(data_root):
            raise CopyUnfinished(
                f"The robot's copy of {folder} did not finish (no copy "
                "record on the stick lists it). Plug the stick into the "
                "robot to finish the copy.")
        return None
    doc = _read_json_object(path)
    if doc is None:
        raise UploadIntegrityError(
            f"{THUMB_MANIFEST} is unreadable; not uploading.")
    files = doc.get("files")
    if not isinstance(files, dict):
        return None
    mission_rel = _mission_relpath(path)
    robot_mission = doc.get("mission_folder")
    if isinstance(robot_mission, str) and robot_mission.startswith("/"):
        robot_building = posixpath.dirname(robot_mission.rstrip("/"))
    else:
        robot_building = posixpath.join(ROBOT_DATA_ROOT,
                                        posixpath.dirname(mission_rel))
    return CopyRecord(_parse_copy_files(files, folder), mission_rel,
                      os.path.dirname(os.path.abspath(path)), folder,
                      robot_building)


def _stick_recovery(record: CopyRecord) -> str:
    return (f"Plug the stick into the robot to repair the copy of "
            f"{record.mission_rel}.")


def write_needs_recopy(mission_dir: str, keys: List[str]) -> None:
    """Add `keys` (copy-record keys, `<folder>/<relpath>`) to the mission's
    needs_recopy.json and flush it to the stick.

    The robot re-copies those files the next time the stick is plugged in
    and deletes the file. Existing entries are kept, so several sections
    of one mission can report into the same file.
    """
    path = os.path.join(mission_dir, NEEDS_RECOPY_FILENAME)
    files = set()
    old = _read_json_object(path)
    if old is not None and isinstance(old.get("files"), list):
        files.update(k for k in old["files"] if isinstance(k, str))
    files.update(keys)
    doc = {
        "schema": 1,
        "written_by": "ocu-uploader",
        "updated_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "files": sorted(files),
    }
    tmp = path + ".tmp"
    with open(tmp, "w") as fh:
        json.dump(doc, fh, indent=2)
        fh.flush()
        try:
            os.fsync(fh.fileno())
        except OSError:
            pass  # exFAT can refuse; the rename still lands the whole file.
    os.replace(tmp, path)
    try:
        dfd = os.open(mission_dir, os.O_RDONLY)
        try:
            os.fsync(dfd)
        finally:
            os.close(dfd)
    except OSError:
        pass


def _report_bad_stick_copy(rels: List[str], record: CopyRecord,
                           missing: bool = False) -> None:
    """Print the operator error for damaged stick files and leave the
    robot a list of them to re-copy."""
    keys = [f"{record.folder}/{rel}" for rel in rels]
    if record.mission_dir:
        try:
            write_needs_recopy(record.mission_dir, keys)
        except OSError as exc:
            print(f"Warning: could not write {NEEDS_RECOPY_FILENAME}: {exc}",
                  flush=True)
    shown = ", ".join(rels[:5])
    extra = f" and {len(rels) - 5} more" if len(rels) > 5 else ""
    if missing:
        what = f"Stick is missing {shown}{extra} that the robot copied."
    else:
        what = (f"Stick copy of {shown}{extra} does not match the robot "
                f"({len(rels)} file{'s' if len(rels) != 1 else ''}).")
    print(f"Unexpected error: {what} {_stick_recovery(record)}", flush=True)


def _robot_fetch_argv(remote_path: str) -> List[str]:
    """ssh command that writes one robot file to stdout (tests replace it)."""
    return [
        "ssh", "-o", "ConnectTimeout=6", "-o", "BatchMode=yes",
        "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
        "-o", "LogLevel=ERROR", "-o", "ServerAliveInterval=5",
        "-o", "ServerAliveCountMax=6",
        ROBOT_SSH, "cat -- " + shlex.quote(remote_path),
    ]


def _fsync_path(path: str) -> None:
    try:
        fd = os.open(path, os.O_RDONLY)
    except OSError:
        return
    try:
        os.fsync(fd)
    except OSError:
        pass  # exFAT can refuse on a directory
    finally:
        os.close(fd)


def repair_from_robot(full: str, rel_norm: str, record: "CopyRecord") -> bool:
    """Replace a damaged stick file with the robot's copy, over the radio.

    The file is fetched next to the bad one, checked against the robot's
    recorded size and sha256, flushed, and only then renamed over it. True
    when the stick now holds the robot's bytes. False (and the stick left
    as it was) when there is no robot link, the robot no longer has the
    file, the file is too big for the radio, or what arrived is not right.
    """
    expected = record.by_rel.get(rel_norm)
    if (not ROBOT_SSH or expected is None or not record.robot_building
            or expected[0] > ROBOT_FIX_MAX_BYTES or _robot_unreachable.is_set()):
        return False
    size, sha = expected
    remote = posixpath.join(record.robot_building, record.folder, rel_norm)
    tmp = os.path.join(os.path.dirname(full),
                       "." + os.path.basename(full) + ROBOT_FIX_SUFFIX)
    # Serial: one radio link, and the stick writes one file at a time anyway.
    with _robot_fix_lock:
        if _robot_unreachable.is_set():
            return False
        timeout = 60 + size / 250_000  # ~250 kB/s worst case over the radio
        try:
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(tmp, "wb") as out:
                done = subprocess.run(_robot_fetch_argv(remote), stdout=out,
                                      stderr=subprocess.PIPE, timeout=timeout)
                out.flush()
                os.fsync(out.fileno())
        except subprocess.TimeoutExpired:
            _robot_unreachable.set()
            _remove_quietly(tmp)
            return False
        except OSError:
            _remove_quietly(tmp)
            return False
        if done.returncode != 0:
            if done.returncode == 255:  # ssh itself: no link, auth, host down
                _robot_unreachable.set()
            _remove_quietly(tmp)
            return False
        try:
            got_size, _md5, got_sha = hash_file(tmp)
        except OSError:
            _remove_quietly(tmp)
            return False
        if got_size != size or got_sha != sha:
            _remove_quietly(tmp)
            return False
        try:
            os.replace(tmp, full)
        except OSError:
            _remove_quietly(tmp)
            return False
        _fsync_path(os.path.dirname(full))
    print(f"Fixed {rel_norm} on the stick from the robot.", flush=True)
    return True


def _remove_quietly(path: str) -> None:
    try:
        os.remove(path)
    except OSError:
        pass


def _check_stick_copy(rel_norm: str, size: int, sha: str) -> None:
    if _copy_record is None:
        return
    expected = _copy_record.by_rel.get(rel_norm)
    if expected is None:
        return
    if size == expected[0] and sha == expected[1]:
        return
    raise StickCopyMismatch(rel_norm, _copy_record)


def bad_stick_files(data_root: str,
                    record: CopyRecord) -> Tuple[List[str], List[str]]:
    """(missing, wrong_size) relpaths against the robot's record.

    A stat per file, no reads: cheap enough to run before every upload,
    and it catches a copy whose data never reached the stick (files
    present at 0 bytes) before anything is sent.
    """
    missing = []
    wrong = []
    for rel, (size, _sha) in record.by_rel.items():
        path = os.path.join(data_root, *rel.split("/"))
        if os.path.islink(path) or not os.path.isfile(path):
            missing.append(rel)
            continue
        try:
            if os.path.getsize(path) != size:
                wrong.append(rel)
        except OSError:
            missing.append(rel)
    return sorted(missing), sorted(wrong)


def missing_stick_files(data_root: str, record: CopyRecord) -> List[str]:
    return bad_stick_files(data_root, record)[0]


def upload_run(data_root: str, robot_id: str, run_id: str, *,
               force: bool = False) -> int:
    """Upload one section or mission folder. Returns the exit code."""
    global _copy_record
    try:
        return _upload_run(data_root, robot_id, run_id, force=force)
    finally:
        # The record belongs to this run only; a later caller in the
        # same process (tests, a future batch mode) must not inherit it.
        _copy_record = None


def _upload_run(data_root: str, robot_id: str, run_id: str, *,
                force: bool = False) -> int:
    # Not "Connection error:" — the OCU retries those, and a retry cannot
    # fix a missing setting or a folder that is not there.
    if not CLIENT_ID:
        print("Unexpected error: BDR_CLOUD_CLIENT_ID env var unset.", flush=True)
        return 1
    if not DEVICE_TOKEN:
        print("Unexpected error: BDR_CLOUD_DEVICE_TOKEN env var unset.", flush=True)
        return 1
    if not API_BASE:
        print("Unexpected error: BDR_CLOUD_API_BASE env var unset.", flush=True)
        return 1
    if not os.path.isdir(data_root):
        print(f"Unexpected error: data_root does not exist: {data_root}", flush=True)
        return 1

    global _copy_record
    _copy_record = None
    _stick_extra_warned.clear()
    _robot_unreachable.clear()
    _reset_unsigned_md5_warning()
    remove_stale_temps(data_root)

    try:
        _copy_record = load_copy_record(data_root)
    except UploadIntegrityError as exc:
        print(f"Unexpected error: {exc}", flush=True)
        return 1
    if _copy_record is not None:
        missing, wrong = bad_stick_files(data_root, _copy_record)
        if missing or wrong:
            # Fix what the robot can send over the radio; report the rest.
            fixed = {rel for rel in missing + wrong
                     if repair_from_robot(os.path.join(data_root, *rel.split("/")),
                                          rel, _copy_record)}
            missing = [rel for rel in missing if rel not in fixed]
            wrong = [rel for rel in wrong if rel not in fixed]
        if missing or wrong:
            if wrong:
                _report_bad_stick_copy(missing + wrong, _copy_record)
            else:
                _report_bad_stick_copy(missing, _copy_record, missing=True)
            return 1

    if force:
        # Drop the finished-upload markers first, then take the normal
        # path. A retry must not pass force again: the state file this
        # run writes is the resume record.
        print("Re-uploading: operator requested a fresh upload.", flush=True)
        disc = discard_bookkeeping(data_root)
        if disc:
            print(f"Unexpected error: {disc}", flush=True)
            return 1

    has_manifest = os.path.exists(manifest_path(data_root))
    has_state = os.path.exists(state_path(data_root))
    if has_manifest and not has_state:
        # State is removed only after /complete succeeds, so a manifest
        # on its own is a finished upload — unless the files have since
        # changed size, in which case the stored objects are stale.
        drift = manifest_size_drift(data_root)
        if drift is None:
            print(f"Already uploaded: {run_id}", flush=True)
            print("Upload complete: noop", flush=True)
            return 0
        print(f"Re-uploading: {drift}.", flush=True)
        disc = _discard_manifest(data_root)
        if disc:
            print(f"Unexpected error: {disc}", flush=True)
            return 1
    elif has_manifest and has_state:
        # Manifest is written before /complete. Both files present means
        # that call never finished. Drop the manifest so a crash in this
        # run cannot take the no-op path next time.
        print("Re-uploading: manifest was written but the upload was not confirmed.",
              flush=True)
        disc = _discard_manifest(data_root)
        if disc:
            print(f"Unexpected error: {disc}", flush=True)
            return 1

    # Past the "already uploaded" return, so this run will rewrite the
    # manifest. The old proof must not outlive that.
    remove_cloud_verified(data_root)

    state = load_state(data_root, robot_id, run_id)
    state["client_id"] = CLIENT_ID
    state["robot_id"] = robot_id
    state["run_id"] = run_id

    pending: List[Tuple[str, str]] = []
    verified = 0
    for full, rel_norm in data_files(data_root):
        if already_uploaded(state, full, rel_norm):
            verified += 1
            print(f"Skipping already uploaded: {rel_norm}", flush=True)
            continue
        pending.append((full, rel_norm))

    print(f"Resuming upload. {verified} files already completed.", flush=True)

    if should_pause(data_root):
        print("Manual pause detected. Stopping safely.", flush=True)
        return 0

    exit_code = _run_pending(data_root, robot_id, run_id, state, pending)
    if exit_code is not None:
        return exit_code

    print("Generating manifest...", flush=True)
    entries = _confirmed_entries(data_root, state)
    if entries is None:
        return 1
    manifest = {
        "client_id": CLIENT_ID,
        "robot_id": robot_id,
        "run_id": run_id,
        "files": entries,
    }
    # s3_key must use the arguments, not whatever an older state file stored.
    for entry in manifest["files"]:
        rel_norm = entry["relpath"]
        entry["s3_key"] = f"{CLIENT_ID}/{robot_id}/{run_id}/{rel_norm}"
    _write_manifest(data_root, manifest)

    try:
        done = _upload_manifest_and_complete(data_root, robot_id, run_id)
    except (UploadIntegrityError, CloudRejected) as exc:
        print(f"Unexpected error: {exc}", flush=True)
        return 1
    except requests.exceptions.RequestException as exc:
        print(f"Connection error: {exc}", flush=True)
        print("Auto-pausing. You can resume later.", flush=True)
        return 0

    print(f"Upload complete: {json.dumps(done, separators=(',', ':'))}",
          flush=True)

    # Local state cleanup. Manifest stays on disk so subsequent runs see
    # this section as fully uploaded. Removing it only after /complete
    # returns is what makes "manifest present, state absent" mean done.
    try:
        os.remove(state_path(data_root))
        print("State cleaned up.", flush=True)
    except OSError as exc:
        print(f"Warning: could not remove {STATE_FILENAME}: {exc}", flush=True)

    return 0


def main() -> int:
    parsed = parse_cli(sys.argv[1:])
    if parsed is None:
        print("Usage: uploader.py <data_root> <robot_id> <run_id> [--force]",
              flush=True)
        return 2

    data_root, robot_id, run_id, force = parsed
    return upload_run(data_root, robot_id, run_id, force=force)


if __name__ == "__main__":
    sys.exit(main())
