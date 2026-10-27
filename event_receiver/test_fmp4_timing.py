import hashlib
from pathlib import Path
import struct
import tempfile
import unittest

from fmp4_timing import FragmentError, _children, _tfhd, _trun, repair_streaming_defaults


def box(kind, data, extended=False):
    return (struct.pack(">I4sQ", 1, kind, len(data) + 16) if extended else
            struct.pack(">I4s", len(data) + 8, kind)) + data


def fixture(extended=False, inherited=False, absolute=False):
    # The intended durations and sizes return to their first value after a
    # differing sample: the exact branch lost by GStreamer 1.20's defaults.
    durations, sizes = [100, 101, 100, 100, 99], [3, 4, 3, 5, 3]
    trex = struct.pack(">IIIIII", 0, 1, 1, 100 if inherited else 0, 3 if inherited else 0, 0)
    moov = box(b"moov", box(b"mvex", box(b"trex", trex)))
    tfhd = box(b"tfhd", struct.pack(">II", 1 if absolute else 0, 1) + (struct.pack(">Q", 0) if absolute else b""), extended)
    tfdt = box(b"tfdt", struct.pack(">II", 0, 0))

    def fragment(data_offset):
        runs = []
        for i, (duration, size) in enumerate(zip(durations, sizes)):
            flags = int(i == 0)
            if duration != durations[0] or i == 0:
                flags |= 0x100
            if size != sizes[0] or i == 0:
                flags |= 0x200
            data = struct.pack(">II", flags, 1)
            if flags & 1:
                data += struct.pack(">i", data_offset)
            if flags & 0x100:
                data += struct.pack(">I", duration)
            if flags & 0x200:
                data += struct.pack(">I", size)
            runs.append(box(b"trun", data))
        return box(b"moof", box(b"mfhd", struct.pack(">II", 0, 1)) +
                   box(b"traf", tfhd + tfdt + b"".join(runs), extended), extended)

    moof = fragment(0)
    moof = fragment(len(moof) + 8)
    payload = b"".join(bytes([i + 1]) * n for i, n in enumerate(sizes))
    return box(b"ftyp", b"isom\0\0\0\0isom") + moov + moof + box(b"mdat", payload), durations, sizes, payload


def read_tables(data):
    for kind, header, payload in _children(data):
        if kind != b"moof":
            continue
        for child, _, track in _children(payload):
            if child != b"traf":
                continue
            entries = list(_children(track))
            tfhd_payload = next(p for k, _, p in entries if k == b"tfhd")
            _, _, defaults = _tfhd(tfhd_payload)
            durations, sizes = [], []
            offset = None
            for k, _, p in entries:
                if k != b"trun":
                    continue
                flags, count, first = _trun(p)
                assert count == 1
                durations.append(first.get(0x100, int.from_bytes(defaults.get(8, b"\0"), "big")))
                sizes.append(first.get(0x200, int.from_bytes(defaults.get(16, b"\0"), "big")))
                if flags & 1:
                    offset = struct.unpack_from(">i", p, 8)[0]
            return durations, sizes, offset, len(header) + len(payload)


class FragmentDefaultsTests(unittest.TestCase):
    def test_restores_original_defaults_and_keeps_media_bytes(self):
        for extended in (False, True):
            with self.subTest(extended=extended), tempfile.TemporaryDirectory() as directory:
                original, durations, sizes, media = fixture(extended=extended)
                path = Path(directory) / "upload.mp4"
                path.write_bytes(original)
                self.assertIn(0, read_tables(original)[0])
                stats = repair_streaming_defaults(path)
                fixed = path.read_bytes()
                actual_durations, actual_sizes, offset, moof_size = read_tables(fixed)
                self.assertEqual(actual_durations, durations)
                self.assertEqual(actual_sizes, sizes)
                self.assertEqual(offset, moof_size + 8)
                self.assertEqual(next(p for k, _, p in _children(fixed) if k == b"mdat"), media)
                self.assertEqual(stats["duration_defaults_restored"], 1)
                self.assertEqual(stats["size_defaults_restored"], 1)
                digest = hashlib.sha256(fixed).hexdigest()
                self.assertEqual(repair_streaming_defaults(path)["fragments_fixed"], 0)
                self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), digest)

    def test_valid_inherited_defaults_are_unchanged(self):
        with tempfile.TemporaryDirectory() as directory:
            data, *_ = fixture(inherited=True)
            path = Path(directory) / "valid.mp4"
            path.write_bytes(data)
            self.assertEqual(repair_streaming_defaults(path)["fragments_fixed"], 0)
            self.assertEqual(path.read_bytes(), data)

    def test_indexed_absolute_and_truncated_inputs_are_not_modified(self):
        for mode in ("indexed", "absolute", "truncated"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                data, *_ = fixture(absolute=mode == "absolute")
                if mode == "indexed":
                    data += box(b"mfra", b"")
                if mode == "truncated":
                    data = data[:-1]
                path = Path(directory) / "invalid.mp4"
                path.write_bytes(data)
                with self.assertRaises(FragmentError):
                    repair_streaming_defaults(path)
                self.assertEqual(path.read_bytes(), data)
                self.assertEqual(list(Path(directory).glob(".timing-*.part")), [])


if __name__ == "__main__":
    unittest.main()
