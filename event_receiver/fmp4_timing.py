"""Restore omitted fragment defaults from the first explicitly recorded sample.

GStreamer 1.20 atoms.c can clear a tfhd default after a differing sample but omit
that field from later truns when samples match the original default again.
The first sample still records the original value explicitly. No frame-rate
estimate, timestamp interpolation, payload change, or sample deletion is used.
Only the single-track, index-free streaming layout produced by this app is fixed.
"""
from pathlib import Path
import shutil
import struct
import uuid

MAX_METADATA = 32 * 1024 * 1024


class FragmentError(ValueError):
    pass


def _children(data):
    offset = 0
    while offset < len(data):
        if len(data) - offset < 8:
            raise FragmentError("truncated child box")
        size, kind = struct.unpack_from(">I4s", data, offset)
        header = 8
        if size == 1:
            if len(data) - offset < 16:
                raise FragmentError("truncated extended box size")
            size = struct.unpack_from(">Q", data, offset + 8)[0]
            header = 16
        if size == 0:
            size = len(data) - offset
        if size < header or size > len(data) - offset:
            raise FragmentError("invalid child box size")
        yield kind, data[offset:offset + header], data[offset + header:offset + size]
        offset += size


def _box(header, payload):
    kind = header[4:8]
    if len(header) == 16:
        return struct.pack(">I4sQ", 1, kind, len(payload) + 16) + payload
    return struct.pack(">I4s", len(payload) + 8, kind) + payload


def _trex_defaults(moov):
    result = {}
    for kind, _, payload in _children(moov):
        if kind != b"mvex":
            continue
        for child, _, entry in _children(payload):
            if child == b"trex":
                if len(entry) != 24 or entry[0] != 0:
                    raise FragmentError("unsupported trex")
                track, _, duration, size, _ = struct.unpack_from(">IIIII", entry, 4)
                result[track] = (duration, size)
    return result


def _tfhd(payload):
    if len(payload) < 8 or payload[0] != 0:
        raise FragmentError("unsupported tfhd")
    flags, track = struct.unpack_from(">II", payload)
    if flags & ~0x03003B:
        raise FragmentError("unsupported tfhd flags")
    fields = {}
    offset = 8
    for bit, size in ((1, 8), (2, 4), (8, 4), (16, 4), (32, 4)):
        if flags & bit:
            if offset + size > len(payload):
                raise FragmentError("truncated tfhd field")
            fields[bit] = payload[offset:offset + size]
            offset += size
    if offset != len(payload):
        raise FragmentError("unexpected tfhd data")
    return flags, track, fields


def _trun(payload):
    if len(payload) < 8 or payload[0] not in (0, 1):
        raise FragmentError("unsupported trun")
    flags = struct.unpack_from(">I", payload)[0] & 0xFFFFFF
    if flags & ~0xF05:
        raise FragmentError("unsupported trun flags")
    count = struct.unpack_from(">I", payload, 4)[0]
    offset = 8 + (4 if flags & 1 else 0) + (4 if flags & 4 else 0)
    entry_size = 4 * sum(bool(flags & bit) for bit in (0x100, 0x200, 0x400, 0x800))
    if offset + count * entry_size != len(payload):
        raise FragmentError("invalid trun sample table length")
    first = {}
    if count:
        for bit in (0x100, 0x200, 0x400, 0x800):
            if flags & bit:
                first[bit] = struct.unpack_from(">I", payload, offset)[0]
                offset += 4
    return flags, count, first


def _repair_moof(payload, defaults):
    children = list(_children(payload))
    trafs = [(i, list(_children(p))) for i, (k, _, p) in enumerate(children) if k == b"traf"]
    changed = []
    duration_fixes = size_fixes = 0
    for index, entries in trafs:
        headers = [(i, p) for i, (k, _, p) in enumerate(entries) if k == b"tfhd"]
        if len(headers) != 1:
            raise FragmentError("expected one tfhd per traf")
        tfhd_index, tfhd_payload = headers[0]
        flags, track, fields = _tfhd(tfhd_payload)
        runs = [_trun(p) for k, _, p in entries if k == b"trun"]
        nonempty = [run for run in runs if run[1]]
        if not nonempty:
            continue
        inherited = defaults.get(track, (0, 0))
        additions = {}
        for tf_bit, tr_bit, inherited_value in ((8, 0x100, inherited[0]), (16, 0x200, inherited[1])):
            # A declared/inherited nonzero default is valid and is never replaced.
            if tf_bit in fields or inherited_value != 0:
                continue
            if not any(not (run[0] & tr_bit) for run in nonempty):
                continue
            value = nonempty[0][2].get(tr_bit)
            if value:
                additions[tf_bit] = struct.pack(">I", value)
        if not additions:
            continue
        if len(trafs) != 1 or flags & 1:
            raise FragmentError("default repair requires one track and relative data offsets")
        fields.update(additions)
        fixed_flags = flags | sum(additions)
        fixed = struct.pack(">II", fixed_flags, track) + b"".join(fields[bit] for bit in (1, 2, 8, 16, 32) if bit in fields)
        kind, header, _ = entries[tfhd_index]
        entries[tfhd_index] = kind, header, fixed
        changed.append((index, entries))
        duration_fixes += int(8 in additions)
        size_fixes += int(16 in additions)
    if not changed:
        return None, 0, 0
    if any(kind not in (b"mfhd", b"traf") for kind, _, _ in children):
        raise FragmentError("unsupported extra fragment box during repair")
    delta = 4 * (duration_fixes + size_fixes)
    for index, entries in changed:
        if any(kind not in (b"tfhd", b"tfdt", b"trun", b"sdtp") for kind, _, _ in entries):
            raise FragmentError("unsupported track box during repair")
        updated = []
        for kind, header, entry in entries:
            if kind == b"trun" and _trun(entry)[0] & 1:
                old = struct.unpack_from(">i", entry, 8)[0]
                if old < 0 or old + delta > 0x7FFFFFFF:
                    raise FragmentError("unsupported trun data offset")
                entry = entry[:8] + struct.pack(">i", old + delta) + entry[12:]
            updated.append(_box(header, entry))
        kind, header, _ = children[index]
        children[index] = kind, header, b"".join(updated)
    return b"".join(_box(header, entry) for _, header, entry in children), duration_fixes, size_fixes


def repair_streaming_defaults(path):
    path = Path(path)
    total = path.stat().st_size
    replacements = {}
    defaults = {}
    indexed = False
    absolute_offsets = False
    metadata_bytes = 0
    stats = {"fragments_fixed": 0, "duration_defaults_restored": 0, "size_defaults_restored": 0}
    with path.open("rb") as stream:
        offset = 0
        while offset < total:
            stream.seek(offset)
            header = stream.read(8)
            if len(header) != 8:
                raise FragmentError("truncated top-level box")
            size, kind = struct.unpack(">I4s", header)
            if size == 1:
                extension = stream.read(8)
                if len(extension) != 8:
                    raise FragmentError("truncated top-level extended size")
                header += extension
                size = struct.unpack(">Q", extension)[0]
            if size == 0:
                size = total - offset
            if size < len(header) or size > total - offset:
                raise FragmentError("invalid top-level box size")
            indexed |= kind in (b"sidx", b"mfra")
            if kind in (b"moov", b"moof"):
                if size > MAX_METADATA:
                    raise FragmentError("MP4 metadata exceeds diagnostic limit")
                payload = stream.read(size - len(header))
                if kind == b"moov":
                    defaults = _trex_defaults(payload)
                else:
                    if not defaults:
                        raise FragmentError("fragment arrived before track defaults")
                    for child, _, track_data in _children(payload):
                        if child == b"traf":
                            for entry, _, header_data in _children(track_data):
                                if entry == b"tfhd":
                                    absolute_offsets |= bool(_tfhd(header_data)[0] & 1)
                    fixed, durations, sizes = _repair_moof(payload, defaults)
                    if fixed is not None:
                        replacement = _box(header, fixed)
                        metadata_bytes += len(replacement)
                        if metadata_bytes > MAX_METADATA:
                            raise FragmentError("too much fragment metadata to repair")
                        replacements[offset] = (size, replacement)
                        stats["fragments_fixed"] += 1
                        stats["duration_defaults_restored"] += durations
                        stats["size_defaults_restored"] += sizes
            offset += size
    if not replacements:
        return stats
    if indexed or absolute_offsets:
        raise FragmentError("refusing to shift indexed or absolutely addressed MP4 fragments")
    temporary = path.with_name(".timing-" + uuid.uuid4().hex + ".part")
    try:
        with path.open("rb") as source, temporary.open("xb") as output:
            position = 0
            for offset, (old_size, replacement) in replacements.items():
                remaining = offset - position
                while remaining:
                    block = source.read(min(65536, remaining))
                    if not block:
                        raise FragmentError("MP4 changed during repair")
                    output.write(block)
                    remaining -= len(block)
                source.seek(offset + old_size)
                output.write(replacement)
                position = offset + old_size
            shutil.copyfileobj(source, output, 65536)
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)
    return stats
