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
_BOOKKEEPING_NAMES = frozenset((
    STATE_FILENAME,
    PAUSE_FILENAME,
    MANIFEST_FILENAME,
    STATE_FILENAME + ".tmp",
    MANIFEST_FILENAME + ".tmp",
))


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
    `upload_state.json` and `manifest.json`. Any other `*.tmp` name is
    data — rosbag and capture files must not be dropped by a broad
    suffix rule.
    """
    return _basename(rel_norm) in _BOOKKEEPING_NAMES


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


def remove_stale_temps(data_root: str) -> None:
    """Drop atomic-write leftovers from a killed run. A live save uses
    the same names, so this only runs when this process is the uploader."""
    for name in (STATE_FILENAME + ".tmp", MANIFEST_FILENAME + ".tmp"):
        path = os.path.join(data_root, name)
        try:
            os.remove(path)
        except FileNotFoundError:
            pass
        except OSError as exc:
            print(f"Warning: could not remove {name}: {exc}", flush=True)


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
    """Remove the uploader's own state, manifest and `.tmp` siblings.

    Used by `--force` before a fresh upload. Scan data and `pause.flag`
    stay. Returns an error string when a file is still present.
    """
    errors = []
    for name in (MANIFEST_FILENAME, STATE_FILENAME,
                 MANIFEST_FILENAME + ".tmp", STATE_FILENAME + ".tmp"):
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
        return
    if rel_norm not in completed:
        completed.append(rel_norm)


def forget_upload(state: dict, rel_norm: str) -> None:
    """Drop a record so the next run PUTs the file again."""
    uploaded = state.get("uploaded")
    if isinstance(uploaded, dict):
        uploaded.pop(rel_norm, None)
    completed = state.get("completed")
    if isinstance(completed, list):
        state["completed"] = [rel for rel in completed if rel != rel_norm]


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
               md5_hex: str, size: int) -> None:
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
    #
    # Content-MD5 is deliberately NOT a signed header. The Lambda never
    # sees the file bytes, so it cannot have signed one, and S3 checks
    # the header against the stored object on its own. A body that does
    # not match — including a truncated or empty body — is rejected with
    # 400 BadDigest instead of being stored.
    if size < 0:
        raise RuntimeError(f"negative upload size for {file_path}")
    headers = {
        "x-amz-server-side-encryption": "AES256",
        "x-amz-meta-client_id": CLIENT_ID,
        "x-amz-meta-robot_id": robot_id,
        "x-amz-meta-run_id": run_id,
        "Content-Length": str(size),
        "Content-MD5": _content_md5_b64(md5_hex),
    }
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
        size, md5_hex, _sha = hash_file(full)
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
    presign_resp = presign(robot_id, run_id, rel_norm, size)
    upload_put(_upload_url(presign_resp, rel_norm), full,
               robot_id, run_id, md5_hex, size)
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


def data_files(data_root: str) -> List[Tuple[str, str]]:
    found: List[Tuple[str, str]] = []
    for full, rel in iter_files(data_root):
        rel_norm = rel.replace("\\", "/")
        if is_sentinel(rel_norm):
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
            save_state(data_root, state)
    except UploadIntegrityError as exc:
        stop_event.set()
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
        presign_resp = presign(robot_id, run_id, MANIFEST_FILENAME, m_size)
        upload_put(_upload_url(presign_resp, MANIFEST_FILENAME), mpath,
                   robot_id, run_id, m_md5, m_size)
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
    """Upload `pending`. Returns an exit code to stop with, or None."""
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


def upload_run(data_root: str, robot_id: str, run_id: str, *,
               force: bool = False) -> int:
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

    remove_stale_temps(data_root)

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
    except UploadIntegrityError as exc:
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
