# Deployed source of the Lambda `roofus-presign-upload-service`
# (API Gateway zx8j0tqep2, us-east-1), as of 2026-10-10.
#
# This repo does not build or deploy it. Paste it into the Lambda
# console and click Deploy. Keep it in step with cpp/scripts/uploader.py:
# /presign signs Content-MD5 only when the body carries content_md5,
# and reports that with md5_signed. The uploader sends the header only
# when that flag is true.
#
# Environment: BUCKET, DEVICE_TOKENS_JSON ({client_id: device_token}).
# The role needs s3:PutObject on the bucket and s3:ListBucket for /verify.

import os
import json
import time
import re
import base64
import boto3
from botocore.client import Config

AWS_REGION = os.environ.get("AWS_REGION", "us-east-1")

s3 = boto3.client(
    "s3",
    region_name=AWS_REGION,
    config=Config(signature_version="s3v4"),
)

BUCKET = os.environ["BUCKET"]
DEVICE_TOKENS = json.loads(os.environ.get("DEVICE_TOKENS_JSON", "{}"))

SAFE_FILENAME = re.compile(r"^[a-zA-Z0-9._\-\/]+$")

MAX_VERIFY_RUNS = 20

# S3 single-PUT ceiling; matches the uploader's own limit.
MAX_PUT_BYTES = 5_000_000_000

def _resp(status, body):
    return {
        "statusCode": status,
        "headers": {"content-type": "application/json"},
        "body": json.dumps(body),
    }

def _get_path(event):
    # HTTP API uses requestContext.http.path
    return event.get("requestContext", {}).get("http", {}).get("path", "")

def _auth(event):
    headers = {k.lower(): v for k, v in (event.get("headers") or {}).items()}
    token = headers.get("x-device-token")
    client_id = headers.get("x-client-id")

    if not token or not client_id:
        return None, _resp(401, {"error": "Missing x-device-token or x-client-id"})

    expected = DEVICE_TOKENS.get(client_id)
    if not expected or token != expected:
        return None, _resp(403, {"error": "Invalid device token"})

    return client_id, None

def _make_key(client_id, robot_id, run_id, relpath):
    # Strongly control where uploads go
    # relpath can include subfolders like "lidar/scan01.bin"
    return f"{client_id}/{robot_id}/{run_id}/{relpath}"

def _valid_content_md5(value):
    # Content-MD5 is the base64 of the 16-byte MD5 digest.
    if not isinstance(value, str):
        return False
    try:
        return len(base64.b64decode(value, validate=True)) == 16
    except Exception:
        return False

def _verify(client_id, body):
    # Read-only: lists what S3 holds for each run so the OCU can
    # detect sections that are missing or incomplete in the cloud.
    robot_id = body.get("robot_id")
    run_ids = body.get("run_ids")

    if not isinstance(robot_id, str) or not robot_id or "/" in robot_id:
        return _resp(400, {"error": "Missing robot_id"})
    if (not isinstance(run_ids, list) or not run_ids
            or len(run_ids) > MAX_VERIFY_RUNS
            or not all(isinstance(r, str) and r and ".." not in r for r in run_ids)):
        return _resp(400, {"error": f"run_ids must be 1-{MAX_VERIFY_RUNS} paths"})

    runs = {}
    paginator = s3.get_paginator("list_objects_v2")
    for run_id in run_ids:
        prefix = _make_key(client_id, robot_id, run_id.strip("/"), "")
        files = []
        for page in paginator.paginate(Bucket=BUCKET, Prefix=prefix):
            for obj in page.get("Contents", []):
                files.append({
                    "relpath": obj["Key"][len(prefix):],
                    "size": obj["Size"],
                    "etag": obj["ETag"].strip('"').lower(),
                })
        runs[run_id] = {"files": files}

    return _resp(200, {"runs": runs})

def lambda_handler(event, context):
    path = _get_path(event)

    client_id, err = _auth(event)
    if err:
        return err

    body_raw = event.get("body") or "{}"
    try:
        body = json.loads(body_raw)
    except json.JSONDecodeError:
        return _resp(400, {"error": "Invalid JSON body"})

    # /verify takes a list of run_ids, so it is handled before the
    # single-run_id check below.
    if path.endswith("/verify"):
        return _verify(client_id, body)

    # Shared fields
    robot_id = body.get("robot_id")
    run_id = body.get("run_id")

    if not robot_id or not run_id:
        return _resp(400, {"error": "Missing robot_id or run_id"})

    if path.endswith("/presign"):
        relpath = body.get("relpath")  # relative path within run
        size_bytes = body.get("size_bytes")

        if not relpath or size_bytes is None:
            return _resp(400, {"error": "Missing relpath or size_bytes"})

        if int(size_bytes) > MAX_PUT_BYTES:
            return _resp(400, {"error": "File too large; max is 5GB"})

        # Basic filename safety
        if ".." in relpath or relpath.startswith("/") or not SAFE_FILENAME.match(relpath):
            return _resp(400, {"error": "Invalid relpath"})

        key = _make_key(client_id, robot_id, run_id, relpath)

        params = {
            "Bucket": BUCKET,
            "Key": key,
            "ServerSideEncryption": "AES256",
            # Optional: store metadata for debugging/search
            "Metadata": {
                "client_id": client_id,
                "robot_id": robot_id,
                "run_id": run_id
            }
        }

        # Optional: when the client sends the file's MD5, sign it into
        # the URL so S3 rejects any body that does not match. Clients
        # that do not send it get the same URL as before.
        content_md5 = body.get("content_md5")
        if content_md5 is not None:
            if not _valid_content_md5(content_md5):
                return _resp(400, {"error": "Invalid content_md5"})
            params["ContentMD5"] = content_md5

        # Presign PUT
        url = s3.generate_presigned_url(
            ClientMethod="put_object",
            Params=params,
            ExpiresIn=3600,  # 1 hour
        )

        return _resp(200, {
            "upload_url": url,
            "s3_key": key,
            "expires_in": 3600,
            "md5_signed": content_md5 is not None,
        })

    if path.endswith("/complete"):
        # Client posts manifest key or contents after uploading everything
        manifest_relpath = body.get("manifest_relpath", "manifest.json")
        key = _make_key(client_id, robot_id, run_id, manifest_relpath)

        # You can optionally write a small "completion marker" in S3 here
        # but that needs PutObject (we have it). Keep it simple:
        marker_key = _make_key(client_id, robot_id, run_id, "_UPLOAD_COMPLETE.json")

        completion_doc = {
            "client_id": client_id,
            "robot_id": robot_id,
            "run_id": run_id,
            "manifest_key": key,
            "completed_at_epoch": int(time.time())
        }

        s3.put_object(
            Bucket=BUCKET,
            Key=marker_key,
            Body=json.dumps(completion_doc).encode("utf-8"),
            ContentType="application/json",
            ServerSideEncryption="AES256",
        )

        return _resp(200, {"ok": True, "marker_key": marker_key})

    return _resp(404, {"error": "Unknown route"})
