#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
"""Check a converted folder the way the AMPR emulator will read it.

usage: check_ampr_index.py GAME_DIR AMPR_EMU_TOOLS_DIR

- ampr_emu.index: its hash table must be exactly the one drakmor/ampr_emu's build_ampr_index.py builds from the
  same records (an index whose lookups miss is called invalid by the emulator, rebuilt from the folder, and every
  packed fileId then points at the wrong file: the game aborts at start), its records sorted the same way;
- the index and the pack manifest (ampr_assets.index) describe the same files in the same order: fileId N of
  the pack is record N of the index;
- eboot.bin and the modules carry an execute bit (on UFS the console refuses to start or load them otherwise).
exit 0 = all good, 1 = a check failed (each failure printed)."""
import os
import struct
import sys

game, tools = sys.argv[1], sys.argv[2]
sys.path.insert(0, tools)
import build_ampr_index as ref  # noqa: E402  (drakmor/ampr_emu, the reference)

failures = []


def fail(msg):
    failures.append(msg)
    print("FAIL", msg)


# ---- ampr_emu.index ----
b = open(os.path.join(game, "ampr_emu.index"), "rb").read()
magic, ver, esz, n, pbytes, hoff, ssz, scnt = struct.unpack_from("<8sIIQQQII", b, 0)
if magic != b"AMPRIDX3" or ver != 3 or esz != 24 or ssz != 16:
    fail(f"ampr_emu.index header {magic!r} v{ver} entry {esz} slot {ssz}")
blob = 48 + n * 24
rows = []
for i in range(n):
    at, pl, size, mtime = struct.unpack_from("<IIQq", b, 48 + i * 24)
    rows.append((size, mtime, b[blob + at: blob + at + pl].decode("utf-8")))
if [r[2] for r in rows] != [r[2] for r in sorted(rows, key=lambda r: ref.key_for(r[2]))]:
    fail("ampr_emu.index records are not in the reference order")
expected = ref.build_hash_slots(rows)
expected = expected[0] if isinstance(expected, tuple) else expected   # (slots, stats) in newer releases
found = [struct.unpack_from("<QII", b, hoff + k * 16) for k in range(scnt)]
if scnt != len(expected):
    fail(f"ampr_emu.index has {scnt} hash slots, the reference {len(expected)}")
else:
    bad = [k for k in range(scnt) if tuple(found[k]) != tuple(expected[k])]
    if bad:
        k = bad[0]
        fail(f"ampr_emu.index hash table differs from build_ampr_index.py in {len(bad)} slot(s); "
             f"slot {k}: ours {found[k]}, reference {expected[k]}")
print(f"ampr_emu.index: {n} records, {scnt} slots checked against build_ampr_index.py")

# ---- the pack manifest: same files, same order ----
m = open(os.path.join(game, "ampr_assets.index"), "rb").read()
fcount = struct.unpack_from("<Q", m, 40)[0]
frs = struct.unpack_from("<I", m, 60)[0]
foff = struct.unpack_from("<Q", m, 72)[0]
soff, sbytes = struct.unpack_from("<QQ", m, 96)
strings = m[soff: soff + sbytes]
if fcount != n:
    fail(f"pack manifest lists {fcount} files, ampr_emu.index {n}")
for i in range(min(fcount, n)):
    _, lsize, mtime, _, _, po, pl, _, _, _, _ = struct.unpack_from("<QQqIIIIIBBH", m, foff + i * frs)
    path = strings[po: po + pl].decode("utf-8")
    if (lsize, mtime, path.lower()) != (rows[i][0], rows[i][1], rows[i][2].lower()):
        fail(f"fileId {i + 1}: pack {path} {lsize} {mtime}, index {rows[i][2]} {rows[i][0]} {rows[i][1]}")
        break
print(f"pack manifest: {fcount} files, same order as the index")

# ---- execute bits ----
checked = 0
for root, _, files in os.walk(game):
    for name in files:
        low = name.lower()
        if low == "eboot.bin" or low.endswith((".prx", ".sprx", ".elf", ".self")):
            p = os.path.join(root, name)
            checked += 1
            if not os.stat(p).st_mode & 0o111:
                fail(f"no execute bit: {os.path.relpath(p, game)}")
print(f"execute bits: {checked} executable file(s) checked")

sys.exit(1 if failures else 0)
