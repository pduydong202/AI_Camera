"""Integration harness: production C++ writer -> real HTTP -> receiver -> full decode."""
import argparse
import json
import sys
from pathlib import Path
import subprocess
import shutil
import tempfile
import threading
import time
from unittest import mock

from server import Handler, Receiver, Rejected
from recheck_failed import verify_sample


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    parser.add_argument("fixture")
    parser.add_argument("--keep-failed", type=Path)
    parser.add_argument("--fixture", type=Path, help="override the 80-frame 20-fps test fixture")
    cases = ("good", "event", "jitter", "reject", "remux_reject", "recovery", "budget", "stall", "recheck")
    parser.add_argument("--case", choices=cases, action="append", help="run selected integration case(s)")
    args = parser.parse_args()
    binary, fixture = str(Path(args.binary).resolve()), Path(args.fixture).resolve()
    if args.fixture:
        fixture = args.fixture.resolve()
    for mode in args.case or cases:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            server = Receiver(("127.0.0.1", 0), root / "remote", "test-secret")
            patcher = None
            if mode == "recheck":
                server.keep_failed_count = 1
                patcher = mock.patch("server.finalize_mp4", side_effect=Rejected(422, "retain raw upload for offline regression"))
                patcher.start()

            class FaultHandler(Handler):
                def do_PUT(self):
                    if mode == "recovery" and self.path.endswith("/1.mp4"):
                        with tempfile.TemporaryFile() as body:
                            self.copy_body(body)
                        self.reply(503, {"error": "injected outage token=test-secret"})
                    elif mode == "remux_reject":
                        with tempfile.TemporaryFile() as body:
                            self.copy_body(body)
                        self.reply(422, {"error": "fragmented MP4 remux failed",
                                         "detail": "exit=1 stderr=injected not-negotiated"})
                    elif mode == "stall":
                        time.sleep(5)
                        self.close_connection = True
                    else:
                        super().do_PUT()

            server.RequestHandlerClass = FaultHandler
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            worker.start()
            try:
                command = [binary, str(fixture), str(root / "local"),
                           f"http://127.0.0.1:{server.server_port}",
                           "wrong-token" if mode == "reject" else "test-secret",
                           "reject" if mode in ("stall", "remux_reject", "recheck") else mode]
                result = subprocess.run(command, capture_output=True, text=True, timeout=20)
                print(result.stdout, end="", flush=True)
                print(result.stderr, end="", file=sys.stderr, flush=True)
                result.check_returncode()
                if mode in ("remux_reject", "recovery"):
                    expected = {"remux_reject": "422", "recovery": "503"}[mode]
                    assert "HTTP " + expected in result.stderr, "missing HTTP status diagnostic"
                if mode in ("reject", "remux_reject", "recovery"):
                    assert "test-secret" not in result.stderr, "receiver response leaked token"
                if mode == "remux_reject":
                    assert "injected not-negotiated" in result.stderr, "missing receiver diagnostic"
                clips = list((root / "remote").rglob("clip.mp4"))
                assert len(clips) == (0 if mode in ("reject", "remux_reject", "stall", "recheck") else 1), (mode, clips)
                if mode == "recheck":
                    patcher.stop()
                    patcher = None
                    samples = list((root / "remote/_failed").glob("*/upload.mp4.part"))
                    assert len(samples) == 1
                    report = verify_sample(samples[0], root / "offline")
                    assert report["packets"] == 80
                    assert samples[0].exists(), "offline verification modified original sample"
                    clips.append(root / "offline/clip.mp4")
                for clip in clips:
                    if mode == "jitter":
                        metadata = json.loads(clip.with_name("event.json").read_text())
                        assert metadata["probe"]["fragment_defaults"]["duration_defaults_restored"] > 0
                        assert metadata["probe"]["timing"]["unique_pts"]
                    subprocess.run(["ffmpeg", "-v", "error", "-xerror", "-i", str(clip),
                                    "-f", "null", "-"], check=True, timeout=15)
                    subprocess.run([binary, "--verify-event" if mode == "event" else "--verify-jitter" if mode == "jitter" else "--verify",
                                    str(fixture), str(clip)], check=True, timeout=10)
                if mode != "recheck":
                    assert not list((root / "remote").rglob("*.part")), "incomplete upload published"
            except BaseException:
                if args.keep_failed:
                    shutil.copytree(root, args.keep_failed / mode, dirs_exist_ok=True)
                raise
            finally:
                if patcher is not None:
                    patcher.stop()
                server.shutdown()
                server.server_close()
                worker.join()
            print(f"PASS: sender/receiver {mode}, full decode and compressed packet parity", flush=True)


if __name__ == "__main__":
    main()
