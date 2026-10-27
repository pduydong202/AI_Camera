import hashlib
import http.client
import json
from pathlib import Path
import socket
import tempfile
import threading
import time
import unittest
from unittest import mock
import subprocess

from server import Receiver, Rejected, finalize_mp4, inspect_mp4
from recheck_failed import verify_sample

FIXTURE = Path(__file__).resolve().parents[1] / "motion_filter_firefly/tests/fixtures/recorder_h264.mp4"
PATH = "/v1/events/firefly01/cam01/20261027T010101.123456Z_pid1/1.mp4"
TOKEN = "test-secret"


class ReceiverTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.server = Receiver(("127.0.0.1", 0), self.directory.name, TOKEN, timeout=1)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.port = self.server.server_port
        self.video = FIXTURE.read_bytes()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.directory.cleanup()

    def put(self, data, path=PATH, token=TOKEN, chunked=False):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        body = [data[i:i+137] for i in range(0, len(data), 137)] if chunked else data
        connection.request("PUT", path, body, {"Authorization": "Bearer " + token,
                           "Content-Type": "video/mp4"}, encode_chunked=chunked)
        response = connection.getresponse()
        result = response.status, json.loads(response.read())
        connection.close()
        return result

    def files(self, pattern):
        return list(Path(self.directory.name).rglob(pattern))

    def test_health(self):
        connection = http.client.HTTPConnection("127.0.0.1", self.port)
        connection.request("GET", "/health")
        response = connection.getresponse()
        self.assertEqual(response.status, 200)
        response.read()
        connection.close()

    def test_chunked_and_fixed_body(self):
        for event, chunked in ((1, True), (2, False)):
            status, reply = self.put(self.video, PATH.replace("/1.mp4", f"/{event}.mp4"), chunked=chunked)
            self.assertEqual(status, 201, reply)
            self.assertEqual(reply["sha256"], hashlib.sha256(self.video).hexdigest())
        self.assertEqual(len(self.files("clip.mp4")), 2)
        for path in self.files("clip.mp4"):
            self.assertEqual(path.read_bytes(), self.video)
            self.assertEqual(json.loads(path.with_name("event.json").read_text())["status"], "ready")

    def test_duplicate_does_not_overwrite(self):
        self.assertEqual(self.put(self.video)[0], 201)
        self.assertEqual(self.put(self.video)[0], 409)
        self.assertEqual(len(self.files("clip.mp4")), 1)

    def test_auth_path_and_invalid_video(self):
        self.assertEqual(self.put(b"bad", token="wrong")[0], 401)
        self.assertEqual(self.put(b"bad", path=PATH.replace("firefly01", ".."))[0], 400)
        self.assertEqual(self.put(b"bad")[0], 422)
        self.assertEqual(self.files("clip.mp4"), [])
        self.assertEqual(self.files("*.part"), [])

    def test_byte_limit(self):
        self.server.max_bytes = 16
        self.assertEqual(self.put(self.video)[0], 413)
        self.assertEqual(self.files("*.part"), [])

    def test_interrupted_chunk_is_not_published_and_retry_can_succeed(self):
        with socket.create_connection(("127.0.0.1", self.port)) as connection:
            connection.sendall((f"PUT {PATH} HTTP/1.1\r\nHost: localhost\r\n"
                f"Authorization: Bearer {TOKEN}\r\nContent-Type: video/mp4\r\n"
                "Transfer-Encoding: chunked\r\n\r\n1000\r\nincomplete").encode())
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            with self.server.active_lock:
                if not self.server.active:
                    break
            time.sleep(0.02)
        # Wait for handler startup too; a health check alone doesn't serialize requests.
        for _ in range(100):
            status, _ = self.put(self.video)
            if status != 409:
                break
            time.sleep(0.02)
        self.assertEqual(status, 201)
        self.assertEqual(len(self.files("clip.mp4")), 1)
        self.assertEqual(self.files("*.part"), [])

    def test_truncated_mp4_not_published(self):
        self.assertEqual(self.put(self.video[:-10])[0], 422)
        self.assertEqual(self.files("event.json"), [])

    def test_remux_failure_reports_exit_code_and_stderr(self):
        info = {"fragmented": True, "streams": [{"codec_name": "h264"}]}
        problem = subprocess.CalledProcessError(3, ["gst-launch-1.0"], stderr=b"streaming stopped, reason not-negotiated\n")
        with mock.patch("server.inspect_mp4", return_value=info), \
             mock.patch("server.repair_streaming_defaults", return_value={}), \
             mock.patch("server.subprocess.run", side_effect=problem):
            with self.assertRaises(Rejected) as caught:
                finalize_mp4(Path(self.directory.name) / "clip.mp4")
        self.assertIn("exit=3", caught.exception.detail)
        self.assertIn("not-negotiated", caught.exception.detail)

    def test_failed_samples_are_opt_in_bounded_and_not_ready(self):
        self.server.keep_failed_count = 2
        problem = Rejected(422, "fragmented MP4 remux failed", "exit=1 token=" + TOKEN)
        with mock.patch("server.finalize_mp4", side_effect=problem):
            for event in (1, 2, 3):
                status, reply = self.put(self.video, PATH.replace("/1.mp4", f"/{event}.mp4"))
                self.assertEqual(status, 422)
                self.assertNotIn(TOKEN, json.dumps(reply))
        samples = list((Path(self.directory.name) / "_failed").glob("*/failure.json"))
        self.assertEqual(len(samples), 2)
        for sample in samples:
            metadata = json.loads(sample.read_text())
            self.assertEqual(metadata["status"], "failed")
            self.assertNotIn(TOKEN, sample.read_text())
            self.assertEqual(sample.with_name("upload.mp4.part").read_bytes(), self.video)
        self.assertEqual(self.files("event.json"), [])

    def test_failed_sample_byte_budget(self):
        self.server.keep_failed_count = 2
        self.server.keep_failed_max_bytes = 1
        with mock.patch("server.finalize_mp4", side_effect=Rejected(422, "remux failed")):
            self.assertEqual(self.put(self.video)[0], 422)
        self.assertEqual(self.files("failure.json"), [])

    def test_offline_recheck_preserves_original_and_checks_decode(self):
        source = Path(self.directory.name) / "retained.mp4.part"
        source.write_bytes(self.video)
        target = Path(self.directory.name) / "rechecked"
        report = verify_sample(source, target)
        self.assertEqual(source.read_bytes(), self.video)
        self.assertEqual(report["packets"], 80)
        self.assertEqual(report["status"], "offline_decode_pass")
        self.assertTrue(report["all_pts_dts_present"])
        self.assertTrue((target / "clip.mp4").exists())
        self.assertFalse((target / "event.json").exists())
        with self.assertRaises(FileExistsError):
            verify_sample(source, target)

    def test_ready_gate_rejects_duplicate_or_missing_timestamps(self):
        path = Path(self.directory.name) / "valid.mp4"
        path.write_bytes(self.video)
        for packets in ([{"pts": 0, "dts": 0}, {"pts": 0, "dts": 1}],
                        [{"pts": 0, "dts": 0}, {"pts": 1, "dts": 0}],
                        [{"pts": 0}]):
            result = subprocess.CompletedProcess([], 0, json.dumps({"streams": [
                {"codec_name": "h264", "width": 64, "height": 48}], "packets": packets}).encode(), b"")
            with self.subTest(packets=packets), mock.patch("server.subprocess.run", return_value=result):
                with self.assertRaises(Rejected):
                    inspect_mp4(path, strict_timing=True)


if __name__ == "__main__":
    unittest.main()
