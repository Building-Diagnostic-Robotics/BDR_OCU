#!/usr/bin/env python3
"""Integrity checks for uploader.py.

A local PUT server stands in for S3: it records the bytes and headers
the script actually sends. The suite fails if a non-empty file is
accepted without a matching Content-MD5 and ETag.
"""

import base64
import contextlib
import hashlib
import io
import json
import os
import sys
import tempfile
import threading
import unittest
import unittest.mock
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uploader  # noqa: E402


ROBOT = "Roofus#0001"
RUN = "September_13_2026/Acme_HQ/Section_1_101500"
CLIENT = "sig_roofing_ID"


def md5_hex(data: bytes) -> str:
    return hashlib.md5(data).hexdigest()


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class _Response:
    def __init__(self, etag):
        self.headers = {"ETag": etag}


class PutHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_PUT(self):
        length = self.headers.get("Content-Length")
        body = b"" if length is None else self.rfile.read(int(length))
        rel = urllib.parse.parse_qs(
            urllib.parse.urlparse(self.path).query).get("rel", [""])[0]
        rec = {
            "rel": rel,
            "body": body,
            "content_length": length,
            "transfer_encoding": self.headers.get("Transfer-Encoding"),
            "content_md5": self.headers.get("Content-MD5"),
            "sse": self.headers.get("x-amz-server-side-encryption"),
            "meta_client": self.headers.get("x-amz-meta-client_id"),
            "meta_robot": self.headers.get("x-amz-meta-robot_id"),
            "meta_run": self.headers.get("x-amz-meta-run_id"),
        }
        with self.server.lock:
            self.server.puts.append(rec)
            hook = self.server.on_put
            mode = self.server.mode
        if hook is not None:
            hook(rel, body)

        if mode == "bad_digest":
            payload = (b'<?xml version="1.0"?><Error><Code>BadDigest</Code>'
                       b"<Message>no</Message></Error>")
            self._reply(400, payload, None)
            return
        if mode == "sig_mismatch":
            payload = (b'<?xml version="1.0"?><Error>'
                       b"<Code>SignatureDoesNotMatch</Code></Error>")
            self._reply(403, payload, None)
            return
        if mode == "manifest_fail" and rel == uploader.MANIFEST_FILENAME:
            self._reply(500, b"nope", None)
            return
        if mode == "no_etag":
            self._reply(200, b"", None)
            return
        if mode == "lie_empty":
            etag = md5_hex(b"")
        else:
            etag = md5_hex(body)
        self._reply(200, b"", etag)

    def _reply(self, status, payload, etag):
        self.send_response(status)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Connection", "close")
        if etag is not None:
            self.send_header("ETag", '"%s"' % etag)
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, fmt, *args):
        return


class PutServer(ThreadingHTTPServer):
    def __init__(self):
        super().__init__(("127.0.0.1", 0), PutHandler)
        self.puts = []
        self.lock = threading.Lock()
        self.mode = "ok"
        self.on_put = None


SERVER = None
PORT = None


def setUpModule():
    global SERVER, PORT
    SERVER = PutServer()
    PORT = SERVER.server_address[1]
    thread = threading.Thread(target=SERVER.serve_forever, daemon=True)
    thread.start()


def tearDownModule():
    SERVER.shutdown()
    SERVER.server_close()


def write(path, data: bytes):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(data)


class UploadCase(unittest.TestCase):
    def setUp(self):
        uploader.CLIENT_ID = CLIENT
        uploader.DEVICE_TOKEN = "roofus#0001"
        uploader.API_BASE = "http://api.invalid"
        self._workers = uploader.UPLOAD_WORKERS
        uploader.UPLOAD_WORKERS = 4
        SERVER.mode = "ok"
        SERVER.on_put = None
        with SERVER.lock:
            SERVER.puts.clear()
        self.complete_calls = []
        self.presign = unittest.mock.patch.object(
            uploader, "presign", side_effect=self._presign)
        self.complete = unittest.mock.patch.object(
            uploader, "complete", side_effect=self._complete)
        self.presign.start()
        self.complete.start()
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name

    def tearDown(self):
        self.presign.stop()
        self.complete.stop()
        uploader.UPLOAD_WORKERS = self._workers
        self.tmp.cleanup()

    def _presign(self, robot_id, run_id, relpath, size_bytes):
        query = urllib.parse.urlencode(
            {"rel": relpath, "size": str(size_bytes)})
        return {"upload_url": "http://127.0.0.1:%d/put?%s" % (PORT, query)}

    def _complete(self, robot_id, run_id, manifest_relpath=uploader.MANIFEST_FILENAME):
        self.complete_calls.append((robot_id, run_id, manifest_relpath))
        return {"ok": True}

    def run_upload(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = uploader.upload_run(self.root, ROBOT, RUN)
        return rc, buf.getvalue()

    def puts(self):
        with SERVER.lock:
            return list(SERVER.puts)

    def assert_put_matches_body(self, rec):
        self.assertIsNotNone(rec["content_length"], rec["rel"])
        self.assertEqual(int(rec["content_length"]), len(rec["body"]), rec["rel"])
        self.assertIsNone(rec["transfer_encoding"], rec["rel"])
        self.assertEqual(
            base64.b64decode(rec["content_md5"]),
            hashlib.md5(rec["body"]).digest(),
            rec["rel"])
        self.assertEqual(rec["sse"], "AES256")
        self.assertEqual(rec["meta_client"], CLIENT)
        self.assertEqual(rec["meta_robot"], ROBOT)
        self.assertEqual(rec["meta_run"], RUN)


class RecordTests(unittest.TestCase):
    def test_hash_and_content_md5_of_known_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "a.bin")
            write(path, b"abc")
            size, md5, sha = uploader.hash_file(path)
        self.assertEqual(size, 3)
        self.assertEqual(md5, "900150983cd24fb0d6963f7d28e17f72")
        self.assertEqual(
            sha,
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
        self.assertEqual(
            uploader._content_md5_b64(md5),
            base64.b64encode(bytes.fromhex(md5)).decode("ascii"))

    def test_empty_file_hashes(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "empty")
            write(path, b"")
            size, md5, sha = uploader.hash_file(path)
        self.assertEqual((size, md5), (0, md5_hex(b"")))
        self.assertEqual(sha, sha256_hex(b""))

    def test_etag_parsing(self):
        empty = md5_hex(b"")
        self.assertEqual(
            uploader._response_md5(_Response('"%s"' % empty.upper())), empty)
        self.assertIsNone(uploader._response_md5(_Response("W/\"%s\"" % empty)))
        self.assertIsNone(uploader._response_md5(_Response('"abcd-2"')))
        self.assertIsNone(uploader._response_md5(_Response("")))

    def test_sentinel_names_are_exact(self):
        self.assertTrue(uploader.is_sentinel("upload_state.json"))
        self.assertTrue(uploader.is_sentinel("nested/manifest.json.tmp"))
        self.assertTrue(uploader.is_sentinel("pause.flag"))
        self.assertFalse(uploader.is_sentinel("notes.tmp"))
        self.assertFalse(uploader.is_sentinel("thumbsync_manifest.json"))
        self.assertFalse(uploader.is_sentinel("Visual_data/frame.tmp.jpg"))

    def test_bool_size_is_not_a_verified_record(self):
        state = {"uploaded": {"a.bin": {"size": True, "md5": "a" * 32}}}
        self.assertIsNone(uploader.stored_record(state, "a.bin"))

    def test_already_uploaded_follows_current_size(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "a.bin")
            write(path, b"abcd")
            state = {"uploaded": {"a.bin": {"size": 4, "md5": "a" * 32}}}
            self.assertTrue(uploader.already_uploaded(state, path, "a.bin"))
            write(path, b"abcde")
            self.assertFalse(uploader.already_uploaded(state, path, "a.bin"))
            self.assertFalse(uploader.already_uploaded({}, path, "a.bin"))

    def test_manifest_drift(self):
        with tempfile.TemporaryDirectory() as tmp:
            write(os.path.join(tmp, "a.bin"), b"abcd")
            write(os.path.join(tmp, "notes.tmp"), b"zzz")
            manifest = {
                "files": [
                    {"relpath": "a.bin", "size_bytes": 4},
                    {"relpath": "notes.tmp", "size_bytes": 3},
                ]
            }
            write(os.path.join(tmp, "manifest.json"),
                  json.dumps(manifest).encode())
            self.assertIsNone(uploader.manifest_size_drift(tmp))

            write(os.path.join(tmp, "a.bin"), b"abcde")
            self.assertIn("a.bin", uploader.manifest_size_drift(tmp))

            write(os.path.join(tmp, "a.bin"), b"abcd")
            write(os.path.join(tmp, "extra.bin"), b"q")
            self.assertIn("extra.bin", uploader.manifest_size_drift(tmp))

            os.remove(os.path.join(tmp, "extra.bin"))
            os.remove(os.path.join(tmp, "notes.tmp"))
            self.assertIn("notes.tmp", uploader.manifest_size_drift(tmp))

            legacy = {"files": [{"relpath": "a.bin"}]}
            write(os.path.join(tmp, "manifest.json"),
                  json.dumps(legacy).encode())
            self.assertIsNone(uploader.manifest_size_drift(tmp))

            write(os.path.join(tmp, "manifest.json"), b"not-json")
            self.assertIn("unreadable", uploader.manifest_size_drift(tmp))

    def test_parse_cli_accepts_force_only_as_the_last_arg(self):
        self.assertEqual(
            uploader.parse_cli(["/data", "robot", "run/id"]),
            ("/data", "robot", "run/id", False))
        self.assertEqual(
            uploader.parse_cli(["/data", "robot", "run/id", "--force"]),
            ("/data", "robot", "run/id", True))
        self.assertIsNone(uploader.parse_cli(["/data", "robot"]))
        self.assertIsNone(uploader.parse_cli(["--force", "/data", "robot", "run"]))
        self.assertIsNone(uploader.parse_cli([]))


class PutTests(UploadCase):
    def test_parallel_upload_sends_every_byte_and_second_run_is_a_noop(self):
        files = {
            "a.bin": b"hello",
            "sub/b.bin": b"world!!",
            "notes.tmp": b"tmp-data",
            "empty.dat": b"",
            "thumbsync_manifest.json": b"{}",
        }
        for rel, data in files.items():
            write(os.path.join(self.root, rel), data)
        write(os.path.join(self.root, "upload_state.json.tmp"), b"junk")
        write(os.path.join(self.root, "manifest.json.tmp"), b"junk")

        rc, out = self.run_upload()
        self.assertEqual(rc, 0, out)
        self.assertIn("Warning: empty.dat is 0 bytes", out)
        self.assertIn("State cleaned up.", out)
        self.assertEqual(len(self.complete_calls), 1)

        sent = {rec["rel"]: rec for rec in self.puts()}
        self.assertEqual(set(sent), set(files) | {uploader.MANIFEST_FILENAME})
        for rel, data in files.items():
            self.assertEqual(sent[rel]["body"], data, rel)
            self.assert_put_matches_body(sent[rel])
        self.assert_put_matches_body(sent[uploader.MANIFEST_FILENAME])
        self.assertFalse(os.path.exists(
            os.path.join(self.root, "upload_state.json")))
        self.assertFalse(os.path.exists(
            os.path.join(self.root, "upload_state.json.tmp")))

        with open(os.path.join(self.root, "manifest.json")) as fh:
            manifest = json.load(fh)
        by_rel = {entry["relpath"]: entry for entry in manifest["files"]}
        self.assertEqual(set(by_rel), set(files))
        for rel, data in files.items():
            self.assertEqual(by_rel[rel]["size_bytes"], len(data))
            self.assertEqual(by_rel[rel]["sha256"], sha256_hex(data))
            self.assertEqual(by_rel[rel]["md5"], md5_hex(data))
            self.assertEqual(
                by_rel[rel]["s3_key"],
                "%s/%s/%s/%s" % (CLIENT, ROBOT, RUN, rel))

        with SERVER.lock:
            SERVER.puts.clear()
        rc, out = self.run_upload()
        self.assertEqual(rc, 0, out)
        self.assertIn("Already uploaded:", out)
        self.assertEqual(self.puts(), [])
        self.assertEqual(len(self.complete_calls), 1)

    def test_sequential_path_uploads_the_file_body(self):
        uploader.UPLOAD_WORKERS = 1
        write(os.path.join(self.root, "a.bin"), b"seq-body")
        write(os.path.join(self.root, "b.bin"), b"second")
        rc, out = self.run_upload()
        self.assertEqual(rc, 0, out)
        self.assertIn("Uploading: a.bin", out)
        bodies = {rec["rel"]: rec["body"] for rec in self.puts()}
        self.assertEqual(bodies["a.bin"], b"seq-body")
        self.assertEqual(bodies["b.bin"], b"second")
        for rec in self.puts():
            self.assert_put_matches_body(rec)

    def test_size_change_after_success_reuploads(self):
        write(os.path.join(self.root, "a.bin"), b"abcd")
        rc, out = self.run_upload()
        self.assertEqual(rc, 0, out)
        write(os.path.join(self.root, "a.bin"), b"abcde")
        with SERVER.lock:
            SERVER.puts.clear()
        rc, out = self.run_upload()
        self.assertEqual(rc, 0, out)
        self.assertIn("Re-uploading:", out)
        data_put = next(rec for rec in self.puts() if rec["rel"] == "a.bin")
        self.assertEqual(data_put["body"], b"abcde")
        self.assert_put_matches_body(data_put)
        with open(os.path.join(self.root, "manifest.json")) as fh:
            manifest = json.load(fh)
        self.assertEqual(manifest["files"][0]["size_bytes"], 5)

    def test_legacy_completed_list_is_uploaded_again(self):
        payload = b"legacy"
        write(os.path.join(self.root, "a.bin"), payload)
        write(os.path.join(self.root, "upload_state.json"),
              json.dumps({"completed": ["a.bin"]}).encode())
        rc, out = self.run_upload()
        self.assertEqual(rc, 0, out)
        self.assertIn("\u2713 Uploaded: a.bin", out)
        bodies = [rec for rec in self.puts() if rec["rel"] == "a.bin"]
        self.assertEqual(len(bodies), 1)
        self.assertEqual(bodies[0]["body"], payload)
        self.assert_put_matches_body(bodies[0])

    def test_legacy_manifest_without_sizes_is_a_noop(self):
        write(os.path.join(self.root, "a.bin"), b"abcd")
        write(os.path.join(self.root, "manifest.json"),
              json.dumps({"files": [{"relpath": "a.bin"}]}).encode())
        rc, out = self.run_upload()
        self.assertEqual(rc, 0, out)
        self.assertIn("Already uploaded:", out)
        self.assertEqual(self.puts(), [])

    def test_unconfirmed_manifest_resumes_without_reputting_data(self):
        payload = b"kept"
        write(os.path.join(self.root, "a.bin"), payload)
        digest = md5_hex(payload)
        write(os.path.join(self.root, "upload_state.json"), json.dumps({
            "completed": ["a.bin"],
            "uploaded": {"a.bin": {"size": len(payload), "md5": digest}},
        }).encode())
        write(os.path.join(self.root, "manifest.json"),
              json.dumps({"files": [{"relpath": "a.bin", "size_bytes": 4}]}).encode())
        rc, out = self.run_upload()
        self.assertEqual(rc, 0, out)
        self.assertIn("not confirmed", out)
        rels = [rec["rel"] for rec in self.puts()]
        self.assertNotIn("a.bin", rels)
        self.assertIn(uploader.MANIFEST_FILENAME, rels)
        self.assertEqual(len(self.complete_calls), 1)
        self.assertFalse(os.path.exists(
            os.path.join(self.root, "upload_state.json")))

    def test_lying_empty_etag_is_not_recorded(self):
        SERVER.mode = "lie_empty"
        write(os.path.join(self.root, "a.bin"), b"not-empty")
        rc, out = self.run_upload()
        self.assertEqual(rc, 1, out)
        self.assertIn("Unexpected error:", out)
        self.assertNotIn("Connection error:", out)
        self.assertIn("different bytes", out)
        rec = self.puts()[0]
        self.assertEqual(rec["body"], b"not-empty")
        self.assert_put_matches_body(rec)
        state_path = os.path.join(self.root, "upload_state.json")
        if os.path.exists(state_path):
            with open(state_path) as fh:
                state = json.load(fh)
            self.assertNotIn("a.bin", state.get("completed", []))
        self.assertFalse(os.path.exists(
            os.path.join(self.root, "manifest.json")))
        self.assertEqual(self.complete_calls, [])

    def test_missing_etag_is_rejected(self):
        SERVER.mode = "no_etag"
        write(os.path.join(self.root, "a.bin"), b"abc")
        rc, out = self.run_upload()
        self.assertEqual(rc, 1, out)
        self.assertIn("did not include an MD5 ETag", out)
        self.assertNotIn("Connection error:", out)
        self.assertEqual(self.puts()[0]["body"], b"abc")

    def test_bad_digest_is_a_hard_failure(self):
        SERVER.mode = "bad_digest"
        write(os.path.join(self.root, "a.bin"), b"abc")
        rc, out = self.run_upload()
        self.assertEqual(rc, 1, out)
        self.assertIn("BadDigest", out)
        self.assertIn("Unexpected error:", out)
        self.assertNotIn("Auto-pausing", out)
        self.assertFalse(os.path.exists(
            os.path.join(self.root, "manifest.json")))

    def test_signature_mismatch_is_not_retried_as_a_connection_error(self):
        SERVER.mode = "sig_mismatch"
        write(os.path.join(self.root, "a.bin"), b"abc")
        rc, out = self.run_upload()
        self.assertEqual(rc, 1, out)
        self.assertIn("signature", out)
        self.assertNotIn("Auto-pausing", out)

    def test_failed_manifest_put_is_resumable(self):
        SERVER.mode = "manifest_fail"
        payload = b"data-bytes"
        write(os.path.join(self.root, "a.bin"), payload)
        rc, out = self.run_upload()
        self.assertEqual(rc, 0, out)
        self.assertIn("Connection error:", out)
        self.assertFalse(os.path.exists(
            os.path.join(self.root, "manifest.json")))
        self.assertEqual(self.complete_calls, [])
        with open(os.path.join(self.root, "upload_state.json")) as fh:
            state = json.load(fh)
        self.assertEqual(state["uploaded"]["a.bin"]["size"], len(payload))
        self.assertEqual(state["uploaded"]["a.bin"]["md5"], md5_hex(payload))

        SERVER.mode = "ok"
        with SERVER.lock:
            SERVER.puts.clear()
        rc, out = self.run_upload()
        self.assertEqual(rc, 0, out)
        rels = [rec["rel"] for rec in self.puts()]
        self.assertNotIn("a.bin", rels)
        self.assertIn(uploader.MANIFEST_FILENAME, rels)
        self.assertEqual(len(self.complete_calls), 1)

    def test_growth_during_put_is_not_recorded(self):
        path = os.path.join(self.root, "grow.bin")
        write(path, b"hello")

        def grow(rel, body):
            if rel == "grow.bin":
                with open(path, "ab") as fh:
                    fh.write(b"!")

        SERVER.on_put = grow
        rc, out = self.run_upload()
        self.assertEqual(rc, 1, out)
        self.assertIn("changed during upload", out)
        self.assertEqual(self.puts()[0]["body"], b"hello")
        state_path = os.path.join(self.root, "upload_state.json")
        if os.path.exists(state_path):
            with open(state_path) as fh:
                state = json.load(fh)
            self.assertNotIn("grow.bin", state.get("uploaded", {}))
        self.assertFalse(os.path.exists(
            os.path.join(self.root, "manifest.json")))

    def test_same_size_rewrite_is_caught_before_the_manifest(self):
        path = os.path.join(self.root, "a.bin")
        write(path, b"XXXX")

        def rewrite(rel, body):
            if rel == "a.bin":
                with open(path, "wb") as fh:
                    fh.write(b"YYYY")

        SERVER.on_put = rewrite
        rc, out = self.run_upload()
        self.assertEqual(rc, 1, out)
        self.assertIn("changed after upload", out)
        self.assertFalse(os.path.exists(
            os.path.join(self.root, "manifest.json")))
        with open(os.path.join(self.root, "upload_state.json")) as fh:
            state = json.load(fh)
        self.assertNotIn("a.bin", state.get("uploaded", {}))

        SERVER.on_put = None
        with SERVER.lock:
            SERVER.puts.clear()
        rc, out = self.run_upload()
        self.assertEqual(rc, 0, out)
        bodies = {rec["rel"]: rec["body"] for rec in self.puts()}
        self.assertEqual(bodies["a.bin"], b"YYYY")

    def test_force_reuploads_a_finished_section_and_keeps_scan_data(self):
        payload = b"hello-force"
        write(os.path.join(self.root, "a.bin"), payload)
        write(os.path.join(self.root, "keep.tmp"), b"data")
        write(os.path.join(self.root, "pause.flag"), b"")
        # pause.flag would stop a run; the clear itself must leave it.
        write(os.path.join(self.root, "manifest.json"), b'{"files":[]}')
        write(os.path.join(self.root, "upload_state.json"), b"{}")
        write(os.path.join(self.root, "manifest.json.tmp"), b"x")
        write(os.path.join(self.root, "upload_state.json.tmp"), b"y")
        self.assertIsNone(uploader.discard_bookkeeping(self.root))
        self.assertTrue(os.path.exists(os.path.join(self.root, "a.bin")))
        self.assertTrue(os.path.exists(os.path.join(self.root, "keep.tmp")))
        self.assertTrue(os.path.exists(os.path.join(self.root, "pause.flag")))
        self.assertFalse(os.path.exists(os.path.join(self.root, "manifest.json")))
        self.assertFalse(os.path.exists(
            os.path.join(self.root, "upload_state.json")))
        os.remove(os.path.join(self.root, "pause.flag"))

        rc, out = self.run_upload()
        self.assertEqual(rc, 0, out)
        with SERVER.lock:
            SERVER.puts.clear()
        self.complete_calls.clear()

        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = uploader.upload_run(self.root, ROBOT, RUN, force=True)
        out = buf.getvalue()
        self.assertEqual(rc, 0, out)
        self.assertIn("Re-uploading: operator requested a fresh upload.", out)
        rels = {rec["rel"] for rec in self.puts()}
        self.assertEqual(rels, {"a.bin", "keep.tmp", uploader.MANIFEST_FILENAME})
        bodies = {rec["rel"]: rec["body"] for rec in self.puts()}
        self.assertEqual(bodies["a.bin"], payload)
        self.assertTrue(os.path.exists(os.path.join(self.root, "a.bin")))
        self.assertTrue(os.path.exists(os.path.join(self.root, "keep.tmp")))
        with open(os.path.join(self.root, "manifest.json")) as fh:
            manifest = json.load(fh)
        by_rel = {entry["relpath"]: entry for entry in manifest["files"]}
        self.assertEqual(by_rel["a.bin"]["md5"], md5_hex(payload))
        self.assertEqual(by_rel["keep.tmp"]["md5"], md5_hex(b"data"))

    def test_pause_flag_sends_nothing(self):
        write(os.path.join(self.root, "a.bin"), b"abc")
        write(os.path.join(self.root, "pause.flag"), b"")
        rc, out = self.run_upload()
        self.assertEqual(rc, 0, out)
        self.assertIn("Manual pause detected.", out)
        self.assertEqual(self.puts(), [])


if __name__ == "__main__":
    unittest.main()
