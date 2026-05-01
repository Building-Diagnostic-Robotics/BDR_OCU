import os
import json
import hashlib
import requests
from tqdm import tqdm

# =========================
# CONFIG (set these)
# =========================

API_BASE = "https://zx8j0tqep2.execute-api.us-east-1.amazonaws.com"
CLIENT_ID = "clientA"
DEVICE_TOKEN = "TOKEN123"

MAX_FILE_BYTES = 3_000_000_000  # 1GB per your requirement

# Chunk size used for hashing + upload streaming
CHUNK_BYTES = 8 * 1024 * 1024  # 8 MB


# =========================
# Utility: SHA256 for integrity
# =========================

def sha256_file(path: str, chunk_size: int = CHUNK_BYTES) -> str:
    """
    Computes SHA256 hash of a file so you can verify integrity later.
    Useful for debugging and ensuring the cloud copy matches local.
    """
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(chunk_size)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


# =========================
# API call: request presigned URL for 1 file
# =========================

def presign(robot_id: str, run_id: str, relpath: str, size_bytes: int) -> dict:
    """
    Calls your broker service to get a presigned upload URL for this exact file.
    Returns: { upload_url, s3_key, expires_in }
    """
    url = f"{API_BASE}/presign"
    headers = {
        "x-client-id": CLIENT_ID,
        "x-device-token": DEVICE_TOKEN,
        "content-type": "application/json",
    }
    payload = {
        "robot_id": robot_id,
        "run_id": run_id,
        "relpath": relpath.replace("\\", "/"),  # normalize windows paths
        "size_bytes": int(size_bytes),
    }

    r = requests.post(url, headers=headers, json=payload, timeout=30)
    r.raise_for_status()
    return r.json()


# =========================
# Upload: PUT file bytes to S3 using presigned URL
# =========================

def upload_put(upload_url: str, file_path: str):
    file_size = os.path.getsize(file_path)
    headers = {
        "x-amz-server-side-encryption": "AES256",
        "Content-Length": str(file_size),
    }

    with open(file_path, "rb") as f:
        r = requests.put(upload_url, data=f, headers=headers, timeout=3600)
        r.raise_for_status()


# =========================
# Optional: Tell backend "run is complete"
# =========================

def complete(robot_id: str, run_id: str, manifest_relpath: str = "manifest.json") -> dict:
    """
    Calls your broker to mark the run complete.
    Your broker can write _UPLOAD_COMPLETE.json or trigger processing.
    """
    url = f"{API_BASE}/complete"
    headers = {
        "x-client-id": CLIENT_ID,
        "x-device-token": DEVICE_TOKEN,
        "content-type": "application/json",
    }
    payload = {
        "robot_id": robot_id,
        "run_id": run_id,
        "manifest_relpath": manifest_relpath
    }

    r = requests.post(url, headers=headers, json=payload, timeout=30)
    r.raise_for_status()
    return r.json()


# =========================
# File discovery: walk a folder
# =========================

def iter_files(root_dir: str):
    """
    Yields (full_path, relative_path) for every file under root_dir.
    """
    for base, _, files in os.walk(root_dir):
        for name in files:
            full = os.path.join(base, name)
            rel = os.path.relpath(full, root_dir)
            yield full, rel


# =========================
# Main upload flow
# =========================

def upload_run(data_root: str, robot_id: str, run_id: str):
    """
    Upload everything in data_root as one run.

    - Upload all files except manifest.json
    - Create manifest.json (with file list + sha256)
    - Upload manifest.json last
    - Call /complete
    """
    manifest = {
        "client_id": CLIENT_ID,
        "robot_id": robot_id,
        "run_id": run_id,
        "files": []
    }

    # 1) Upload all files (except manifest.json if it already exists)
    for full, rel in iter_files(data_root):
        rel_norm = rel.replace("\\", "/")

        if rel_norm == "manifest.json":
            continue

        size = os.path.getsize(full)
        if size > MAX_FILE_BYTES:
            raise RuntimeError(f"File too large (>1GB): {rel_norm} size={size}")

        # Hash for integrity + debugging
        file_hash = sha256_file(full)

        # Get presigned URL
        presign_resp = presign(robot_id, run_id, rel_norm, size)
        upload_url = presign_resp["upload_url"]
        s3_key = presign_resp["s3_key"]

        # Upload with retries
        for attempt in range(1, 4):
            try:
                upload_put(upload_url, full)
                break
            except Exception as e:
                if attempt == 3:
                    raise
                print(f"Retry {attempt}/3 failed for {rel_norm}: {e}")

        manifest["files"].append({
            "relpath": rel_norm,
            "s3_key": s3_key,
            "size_bytes": size,
            "sha256": file_hash
        })

    # 2) Write manifest.json locally
    manifest_path = os.path.join(data_root, "manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)

    # 3) Upload manifest.json LAST (acts as “run ready” index)
    m_size = os.path.getsize(manifest_path)
    presign_resp = presign(robot_id, run_id, "manifest.json", m_size)
    upload_put(presign_resp["upload_url"], manifest_path)

    # 4) Mark complete (optional but recommended)
    done = complete(robot_id, run_id, "manifest.json")
    print("Upload complete:", done)


# =========================
# CLI entry point
# =========================

if __name__ == "__main__":
    import sys
    if len(sys.argv) != 4:
        print("Usage: python upload_run.py <data_root> <robot_id> <run_id>")
        raise SystemExit(2)

    data_root = sys.argv[1]
    robot_id = sys.argv[2]
    run_id = sys.argv[3]

    upload_run(data_root, robot_id, run_id)