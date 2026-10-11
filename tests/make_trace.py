#!/usr/bin/env python3
"""Write a synthetic AMPRCMD1 command journal (what the debug AMPR emulator records in
/app0/ampr_commands.bin) for a converted test game, using its ampr_emu.index:

  - data/level4/world_4.dat2   sequential 512 KiB reads      -> streaming, 512 KiB blocks
  - data/level5/world_5.dat2   repeated random 2 KiB reads   -> random, 16 KiB blocks, hot
  - audio/bank.wem             a few reads                   -> must stay loose ("keep" rules)

usage: make_trace.py INDEX OUT_JOURNAL"""
import random
import struct
import sys

index_path, out_path = sys.argv[1], sys.argv[2]
data = open(index_path, "rb").read()
magic, version, entry_size, count, path_bytes = struct.unpack_from("<8sIIQQ", data, 0)
assert magic == b"AMPRIDX3" and version == 3 and entry_size == 24, "not an AMPRIDX3 index"
blob = 48 + count * 24
ids = {}
for i in range(count):
    at, ln, size, _mtime = struct.unpack_from("<IIQq", data, 48 + i * 24)
    path = data[blob + at:blob + at + ln].decode("utf-8").lower()
    ids[path.removeprefix("/app0/")] = (i + 1, size)


def read_file(file_id, offset, length):
    """AprReadFile, 5 dwords (ampr_emu tools/parse_ampr_command_log.py layout)."""
    assert offset < (1 << 32)
    w0 = 40 | (4 << 8) | ((offset & 0x3FFFF) << 12)
    return struct.pack("<5I", w0, length - 1, file_id & 0x7FFFFFFF, 0x10000000, offset & 0xFFFC0000)


def fnv1a64(b):
    h = 1469598103934665603
    for x in b:
        h = ((h ^ x) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h or 1


commands = []
fid, size = ids["data/level4/world_4.dat2"]
for off in range(0, size - (512 << 10), 512 << 10):
    commands.append(read_file(fid, off, 512 << 10))
rng = random.Random(7)
fid, size = ids["data/level5/world_5.dat2"]
hot_offsets = [rng.randrange(0, 2 << 20) & ~0xFFF for _ in range(150)]
for _ in range(4):
    for off in hot_offsets:
        commands.append(read_file(fid, off, 2048))
fid, size = ids["audio/bank.wem"]
for off in range(0, 4 * 4096, 4096):
    commands.append(read_file(fid, off, 4096))

with open(out_path, "wb") as f:
    seq = 1
    for i in range(0, len(commands), 32):   # 32 commands per submit, like a small APR batch
        batch = commands[i:i + 32]
        payload = b"".join(batch)
        header = struct.pack("<8sHHIQQQQQQIIIIIIII", b"AMPRCMD1", 1, 96, 96 + len(payload), seq, 0, seq * 1000, seq, 0x200000000,
                             fnv1a64(payload), len(payload), len(payload), len(batch), 0, 1, 0, 0, 0)
        assert len(header) == 96
        f.write(header + payload)
        seq += 1
print(f"{len(commands)} commands in {seq - 1} records -> {out_path}")
