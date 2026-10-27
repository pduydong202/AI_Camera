#!/usr/bin/env python3
"""Recheck retained uploads offline. Originals and ready events are never modified."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

from server import Rejected, file_sha256, finalize_mp4, media_failure


def verify_sample(source, output):
    """Remux a private copy, decode all frames and verify the original is untouched."""
    source_digest = file_sha256(source)
    with tempfile.TemporaryDirectory(prefix="motion-recheck-") as temporary:
        copy = Path(temporary) / "clip.mp4"
        shutil.copyfile(source, copy)
        probe = finalize_mp4(copy)
        try:
            subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-xerror", "-i", str(copy),
                            "-map", "0:v:0", "-vsync", "0", "-enc_time_base", "-1", "-f", "null", "-"],
                           capture_output=True, timeout=120, check=True)
            result = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0",
                "-show_packets", "-show_entries", "packet=pts,dts", "-of", "json", str(copy)],
                capture_output=True, timeout=30, check=True)
            packets = json.loads(result.stdout)["packets"]
        except (subprocess.SubprocessError, ValueError, KeyError) as exc:
            raise Rejected(422, "offline full-decode/packet check failed", media_failure(exc)) from exc
        if not packets or any("pts" not in p or "dts" not in p for p in packets):
            raise Rejected(422, "offline check found empty video or missing PTS/DTS")
        if len({p["pts"] for p in packets}) != len(packets):
            raise Rejected(422, "offline check found duplicate PTS")
        if any(b["dts"] <= a["dts"] for a, b in zip(packets, packets[1:])):
            raise Rejected(422, "offline check found non-increasing DTS")
        if file_sha256(source) != source_digest:
            raise RuntimeError("Original diagnostic sample changed")
        output.mkdir(parents=True, exist_ok=False)
        shutil.copyfile(copy, output / "clip.mp4")
        report = {"status": "offline_decode_pass", "source": str(source), "source_sha256": source_digest,
                  "output_sha256": file_sha256(output / "clip.mp4"), "packets": len(packets),
                  "all_pts_dts_present": True, "probe": probe,
                  "note": "Offline result only; not an acknowledged/uploaded ready event"}
        (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--failed-root", type=Path, default=Path("received/_failed"))
    parser.add_argument("--output", type=Path, default=Path("rechecked"))
    args = parser.parse_args()
    samples = sorted(args.failed_root.glob("*/upload.mp4.part"))
    if not samples:
        parser.error("no retained upload.mp4.part samples found")
    failures = 0
    for source in samples:
        target = args.output / source.parent.name
        try:
            report = verify_sample(source, target)
            print(f"[RECHECK PASS] sample={source.parent.name} packets={report['packets']} output={target}", flush=True)
        except Rejected as exc:
            failures += 1
            print(f"[RECHECK FAILED] sample={source.parent.name} reason={exc.message} detail={exc.detail}", flush=True)
        except (OSError, RuntimeError) as exc:
            failures += 1
            print(f"[RECHECK FAILED] sample={source.parent.name} reason={exc}", flush=True)
    print(f"[RECHECK SUMMARY] samples={len(samples)} passed={len(samples)-failures} failed={failures}", flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
