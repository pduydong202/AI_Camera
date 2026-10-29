#!/usr/bin/env python3
"""LAN/VPN event receiver. Python 3.10+, ffprobe, GStreamer; no Python packages required."""
import argparse
import datetime as dt
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from fmp4_timing import FragmentError, repair_streaming_defaults


class Rejected(Exception):
    def __init__(self, status, message, detail=""):
        self.status, self.message, self.detail = status, message, detail


def media_failure(exc):
    stderr = getattr(exc, "stderr", None) or b""
    if isinstance(stderr, bytes):
        stderr = stderr.decode("utf-8", errors="replace")
    if isinstance(exc, subprocess.TimeoutExpired):
        label = f"timeout after {exc.timeout}s"
    elif isinstance(exc, subprocess.CalledProcessError):
        label = f"exit={exc.returncode}"
    else:
        label = type(exc).__name__
    # Keep one bounded line; no subprocess command line or environment is logged.
    detail = " ".join(str(stderr).split())
    return (label + (" stderr=" + detail if detail else ""))[:4096]


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for data in iter(lambda: stream.read(65536), b""):
            digest.update(data)
    return digest.hexdigest()


def inspect_mp4(path, strict_timing=False):
    """Reject truncated top-level boxes, then probe video metadata (not a full decode)."""
    length = path.stat().st_size
    found = set()
    with path.open("rb") as stream:
        offset = 0
        while offset < length:
            header = stream.read(8)
            if len(header) != 8:
                raise Rejected(422, "truncated MP4 box")
            size, kind = struct.unpack(">I4s", header)
            header_size = 8
            if size == 1:
                large = stream.read(8)
                if len(large) != 8:
                    raise Rejected(422, "truncated MP4 box size")
                size, header_size = struct.unpack(">Q", large)[0], 16
            if size == 0:
                size = length - offset
            if size < header_size or offset + size > length:
                raise Rejected(422, "incomplete MP4 box")
            found.add(kind)
            offset += size
            stream.seek(offset)
    if not {b"ftyp", b"moov", b"mdat"}.issubset(found):
        raise Rejected(422, "missing MP4 video structures")
    try:
        command = ["ffprobe", "-v", "error", "-select_streams", "v:0"]
        entries = "stream=codec_name,width,height:format=duration"
        if strict_timing:
            command += ["-show_packets"]
            entries += ":packet=pts,dts,pts_time,duration"
        result = subprocess.run(command + ["-show_entries", entries, "-of", "json", str(path)],
                                capture_output=True, timeout=10, check=True)
        info = json.loads(result.stdout)
        video = info["streams"][0]
        if video["codec_name"] not in ("h264", "hevc") or video["width"] <= 0 or video["height"] <= 0:
            raise ValueError("unsupported video")
        info["fragmented"] = b"moof" in found
        if strict_timing:
            packets = info.pop("packets", [])
            if not packets or any("pts" not in p or "dts" not in p for p in packets):
                raise Rejected(422, "missing packet PTS/DTS")
            pts = [p["pts"] for p in packets]
            dts = [p["dts"] for p in packets]
            if len(set(pts)) != len(pts):
                raise Rejected(422, "duplicate packet PTS")
            if any(b <= a for a, b in zip(dts, dts[1:])):
                raise Rejected(422, "non-increasing packet DTS")
            if result.stderr:
                raise Rejected(422, "video packet validation failed", result.stderr.decode(errors="replace")[:4096])
            times = sorted(float(p["pts_time"]) for p in packets)
            info["timing"] = {"packets": len(packets), "unique_pts": True, "strict_dts": True,
                              "max_pts_gap_sec": max((b-a for a, b in zip(times, times[1:])), default=0)}
        return info
    except (subprocess.SubprocessError, ValueError, KeyError, IndexError) as exc:
        raise Rejected(422, "video metadata validation failed", media_failure(exc)) from exc


def finalize_mp4(path):
    info = inspect_mp4(path)
    if not info["fragmented"]:
        return inspect_mp4(path, strict_timing=True)
    # Older FFmpeg demuxers misread implicit offsets across multiple trun boxes
    # emitted by GStreamer 1.20. qtdemux reads these fragments correctly. Remux
    # on the server to a normal MP4 for broad player compatibility; never encode.
    normalized = path.with_name("normalized.mp4")
    # qtdemux already supplies MP4-framed access units and container PTS/DTS.
    # Re-parsing H.264/H.265 here is unnecessary and can clear PTS on affected
    # streams. Link the video pad directly; never synthesize PTS or drop buffers.
    try:
        repairs = repair_streaming_defaults(path)
        # Keep the sender's microsecond resolution; an automatic timescale can
        # merge distinct close timestamps during this otherwise lossless remux.
        subprocess.run(["gst-launch-1.0", "-q", "filesrc", "location=" + json.dumps(str(path)),
                        "!", "qtdemux", "name=demux", "demux.video_0", "!", "mp4mux", "faststart=true", "trak-timescale=1000000",
                        "!", "filesink", "location=" + json.dumps(str(normalized))],
                       capture_output=True, timeout=10, check=True)
        normalized.replace(path)
        result = inspect_mp4(path, strict_timing=True)
        result["fragment_defaults"] = repairs
        return result
    except FragmentError as exc:
        raise Rejected(422, "fragment default metadata invalid", str(exc)) from exc
    except (OSError, subprocess.SubprocessError) as exc:
        raise Rejected(422, "fragmented MP4 remux failed", media_failure(exc)) from exc


class Receiver(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address, root, token, max_bytes=512 * 1024**2, timeout=15,
                 max_uploads=16, max_seconds=3600, keep_failed_count=0, keep_failed_max_mib=32):
        self.root = Path(root).resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        if not token or not token.isascii() or any(c in token for c in "\r\n"):
            raise ValueError("set a nonempty ASCII VIDEO_SERVER_TOKEN")
        if min(max_bytes, timeout, max_uploads, max_seconds) <= 0:
            raise ValueError("receiver limits must be positive")
        self.token, self.max_bytes, self.timeout = token, max_bytes, timeout
        self.max_seconds = max_seconds
        self.slots = threading.BoundedSemaphore(max_uploads)
        # TCP accept backlog is separate from the active-upload limit. Python's
        # default backlog of five is too small for synchronized camera events.
        self.request_queue_size = max(16, 2 * max_uploads)
        self.connection_lock = threading.Lock()
        self.connection_count = 0
        self.active, self.active_lock = set(), threading.Lock()
        if keep_failed_count < 0 or keep_failed_max_mib < 1:
            raise ValueError("invalid failed-sample diagnostic limits")
        self.keep_failed_count = keep_failed_count
        self.keep_failed_max_bytes = keep_failed_max_mib * 1024**2
        self.failed_lock = threading.Lock()
        super().__init__(address, Handler)

    def preserve_failed(self, staging, key, error):
        if not self.keep_failed_count:
            return False
        failed_root = self.root / "_failed"
        with self.failed_lock:
            metadata = {"status": "failed", "event_key": key, "http_status": error.status,
                        "error": error.message, "detail": error.detail.replace(self.token, "[redacted]"),
                        "received_utc": dt.datetime.now(dt.timezone.utc).isoformat()}
            metadata_bytes = json.dumps(metadata, indent=2).encode("utf-8")
            failed_root.mkdir(exist_ok=True)
            existing = [p for p in failed_root.iterdir() if p.is_dir()]
            used = sum(p.stat().st_size for p in failed_root.rglob("*") if p.is_file())
            sample_size = sum(p.stat().st_size for p in staging.iterdir() if p.is_file())
            if len(existing) >= self.keep_failed_count or used + sample_size + len(metadata_bytes) > self.keep_failed_max_bytes:
                print("[DIAG SKIPPED] failed-sample budget reached", flush=True)
                return False
            (staging / "failure.json").write_bytes(metadata_bytes)
            for name in ("clip.mp4", "normalized.mp4"):
                sample = staging / name
                if sample.exists():
                    sample.rename(sample.with_name("upload.mp4.part" if name == "clip.mp4" else "normalized.mp4.part"))
            target = failed_root / uuid.uuid4().hex
            staging.rename(target)
            print(f"[DIAG SAVED] path={target}", flush=True)
            return True

    def process_request(self, request, client_address):
        if not self.slots.acquire(blocking=False):
            print(f"[BUSY] client={client_address[0]}:{client_address[1]} active_limit_reached=1", flush=True)
            try:
                request.settimeout(1)
                request.sendall(b"HTTP/1.1 503 Busy\r\nConnection: close\r\nContent-Length: 0\r\n\r\n")
            except OSError:
                pass
            self.shutdown_request(request)
            return
        with self.connection_lock:
            self.connection_count += 1
            print(f"[ACCEPT] client={client_address[0]}:{client_address[1]} active={self.connection_count}", flush=True)
        try:
            super().process_request(request, client_address)
        except BaseException:
            with self.connection_lock:
                self.connection_count -= 1
            self.slots.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            with self.connection_lock:
                self.connection_count -= 1
            self.slots.release()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "EventReceiver/1"

    def setup(self):
        super().setup()
        self.connection.settimeout(self.server.timeout)

    def log_message(self, fmt, *args):
        # Avoid logging request headers or credential values.
        print("[HTTP] " + self.address_string() + " " + (fmt % args), flush=True)

    def reply(self, status, payload):
        data = json.dumps(payload).encode()
        self.close_connection = True
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        self.reply(200 if self.path == "/health" else 404,
                   {"status": "ok"} if self.path == "/health" else {"error": "not found"})

    def handle_expect_100(self):
        # The sender suppresses Expect. Do not accept an unauthenticated body.
        if not self.authorized():
            self.reply(401, {"error": "unauthorized"})
            return False
        return super().handle_expect_100()

    def authorized(self):
        values = self.headers.get_all("Authorization", [])
        return len(values) == 1 and hmac.compare_digest(
            values[0].encode("utf-8"), ("Bearer " + self.server.token).encode("ascii"))

    def copy_body(self, stream):
        lengths = self.headers.get_all("Content-Length", [])
        encodings = self.headers.get_all("Transfer-Encoding", [])
        if len(lengths) > 1 or len(encodings) > 1 or (lengths and encodings):
            raise Rejected(400, "ambiguous request framing")
        chunked = encodings and encodings[0].lower().strip() == "chunked"
        if encodings and not chunked:
            raise Rejected(400, "unsupported transfer encoding")
        if not chunked and not lengths:
            raise Rejected(411, "body length or chunked encoding required")
        digest, total = hashlib.sha256(), 0
        deadline = time.monotonic() + self.server.max_seconds

        def copy_exact(count):
            nonlocal total
            if count < 0 or count > self.server.max_bytes - total:
                raise Rejected(413, "event exceeds upload byte limit")
            while count:
                if time.monotonic() >= deadline:
                    raise Rejected(408, "event exceeds upload duration limit")
                data = self.rfile.read(min(count, 65536))
                if not data:
                    raise Rejected(400, "upload interrupted")
                stream.write(data)
                digest.update(data)
                total += len(data)
                count -= len(data)

        if chunked:
            while True:
                if time.monotonic() >= deadline:
                    raise Rejected(408, "upload duration limit")
                line = self.rfile.readline(1025)
                if len(line) > 1024 or not line.endswith(b"\r\n"):
                    raise Rejected(400, "invalid chunk header")
                number = line[:-2].split(b";", 1)[0]
                if not re.fullmatch(rb"[0-9a-fA-F]{1,16}", number):
                    raise Rejected(400, "invalid chunk size")
                count = int(number, 16)
                if count == 0:
                    # This API has no request trailers; reject rather than ignore them.
                    if self.rfile.read(2) != b"\r\n":
                        raise Rejected(400, "request trailers are unsupported")
                    break
                copy_exact(count)
                if self.rfile.read(2) != b"\r\n":
                    raise Rejected(400, "invalid chunk ending")
        else:
            if not re.fullmatch(r"[0-9]{1,16}", lengths[0]):
                raise Rejected(400, "invalid content length")
            copy_exact(int(lengths[0]))
        return total, digest.hexdigest()

    def do_PUT(self):
        staging, key = None, None
        body_complete = False

        def discard_staging():
            nonlocal staging
            if staging is not None:
                shutil.rmtree(staging, ignore_errors=True)
                staging = None

        try:
            if not self.authorized():
                raise Rejected(401, "unauthorized")
            match = re.fullmatch(
                r"/v1/events/([A-Za-z0-9_-]{1,100})/([A-Za-z0-9_-]{1,100})/"
                r"([A-Za-z0-9][A-Za-z0-9_.-]{0,99})/([1-9][0-9]{0,19})\.mp4", self.path)
            if not match:
                raise Rejected(400, "invalid event path")
            if self.headers.get("Content-Type", "").lower() != "video/mp4":
                raise Rejected(415, "expected video/mp4")
            device, camera, session, event = match.groups()
            key = (device, camera, session, event)
            print(f"[UPLOAD START] camera={camera} session={session} event={event} client={self.client_address[0]}:{self.client_address[1]}", flush=True)
            parent = self.server.root / device / camera / session
            final = parent / event
            with self.server.active_lock:
                if key in self.server.active or final.exists():
                    key = None
                    raise Rejected(409, "event already exists or is uploading")
                self.server.active.add(key)
            parent.mkdir(parents=True, exist_ok=True)
            staging = parent / ("." + event + "-" + uuid.uuid4().hex + ".part")
            staging.mkdir()
            video = staging / "clip.mp4"
            with video.open("xb") as stream:
                count, digest = self.copy_body(stream)
                stream.flush()
                os.fsync(stream.fileno())
            body_complete = True
            probe = finalize_mp4(video)
            repairs = probe.get("fragment_defaults", {})
            if repairs.get("fragments_fixed"):
                print(f"[TIMING REPAIR] device={device} camera={camera} event={event} "
                      f"fragments={repairs['fragments_fixed']} "
                      f"duration_defaults={repairs['duration_defaults_restored']} "
                      f"size_defaults={repairs['size_defaults_restored']}", flush=True)
            with video.open("rb") as stream:
                os.fsync(stream.fileno())
            metadata = {"status": "ready", "device": device, "camera": camera,
                        "session": session, "event": event, "upload_bytes": count,
                        "upload_sha256": digest, "bytes": video.stat().st_size,
                        "sha256": file_sha256(video),
                        "received_utc": dt.datetime.now(dt.timezone.utc).isoformat(), "probe": probe}
            with (staging / "event.json").open("x", encoding="utf-8") as stream:
                json.dump(metadata, stream, ensure_ascii=False, indent=2)
                stream.flush()
                os.fsync(stream.fileno())
            # Publish the clip and its metadata as one directory on the same filesystem.
            staging.rename(final)
            staging = None
            print(f"[READY] device={device} camera={camera} session={session} event={event} bytes={count}", flush=True)
            self.reply(201, {"status": "ready", "bytes": count, "sha256": digest})
        except Rejected as exc:
            if staging is not None and body_complete:
                try:
                    if self.server.preserve_failed(staging, key, exc):
                        staging = None
                except OSError:
                    print("[DIAG SKIPPED] diagnostic storage unavailable", flush=True)
            discard_staging()
            self.safe_error(exc.status, exc.message, exc.detail)
        except (socket.timeout, TimeoutError):
            discard_staging()
            self.safe_error(408, "upload timeout")
        except OSError:
            discard_staging()
            self.safe_error(503, "upload interrupted or storage unavailable")
        finally:
            # Cleanup also precedes error replies, so an acknowledged rejection
            # never leaves an upload directory racing a reader/retry.
            discard_staging()
            if key is not None:
                with self.server.active_lock:
                    self.server.active.discard(key)

    def safe_error(self, status, message, detail=""):
        detail = detail.replace(self.server.token, "[redacted]")[:4096]
        print(f"[REJECTED] status={status} reason={message} detail={json.dumps(detail)}", flush=True)
        try:
            self.reply(status, {"error": message, "detail": detail})
        except OSError:
            pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("RECEIVER_HOST", "127.0.0.1"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("RECEIVER_PORT", "8080")))
    parser.add_argument("--output", default=os.environ.get("RECEIVER_OUTPUT", "received"))
    parser.add_argument("--max-mib", type=int, default=int(os.environ.get("RECEIVER_MAX_MIB", "512")))
    parser.add_argument("--max-seconds", type=int, default=3600)
    parser.add_argument("--timeout", type=float, default=15)
    parser.add_argument("--max-uploads", type=int, default=16)
    parser.add_argument("--keep-failed-count", type=int, default=0,
                        help="opt-in: retain at most N completed rejected uploads for diagnosis")
    parser.add_argument("--keep-failed-max-mib", type=int, default=32,
                        help="total disk budget for rejected-upload diagnostics across restarts")
    args = parser.parse_args()
    if not shutil.which("ffprobe") or not shutil.which("gst-launch-1.0"):
        parser.error("install ffmpeg and GStreamer tools/good/bad plugins first")
    with Receiver((args.host, args.port), args.output, os.environ.get("VIDEO_SERVER_TOKEN", ""),
                  args.max_mib * 1024**2, args.timeout, args.max_uploads, args.max_seconds,
                  args.keep_failed_count, args.keep_failed_max_mib) as server:
        print(f"[LISTEN] {args.host}:{server.server_port} output={server.root}", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
