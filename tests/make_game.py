#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
"""Generate a synthetic PS5 game folder that exercises every packer path:
compressible data, duplicated blocks (dedup), an incompressible 70 MiB file
(auto-loose), tiny and empty files, loose system files, dump leftovers."""
import json
import os
import random
import sys

root = sys.argv[1] if len(sys.argv) > 1 else "testgame"
rng = random.Random(1234)
BLOCK = 65536


def write(rel, data):
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def compressible(size, seed):
    r = random.Random(seed)
    words = [("w%03d" % r.randrange(500)).encode() for _ in range(300)]
    out = bytearray()
    while len(out) < size:
        out += b" ".join(r.choice(words) for _ in range(64)) + b"\n"
    return bytes(out[:size])


os.makedirs(root, exist_ok=True)
write("sce_sys/param.json", json.dumps({
    "titleId": "PPSA00001", "contentId": "UP0001-PPSA00001_00-EXFAT2AMPRTEST00",
    "contentVersion": "01.000.000", "titleName": "exfat2ampr test game",
    "localizedParameters": {"defaultLanguage": "en-US", "en-US": {"titleName": "exfat2ampr test game"}},
}).encode())
write("eboot.bin", os.urandom(1 << 20))
write("sce_module/libc.prx", os.urandom(300000))
write("sce_sys/icon0.png", os.urandom(50000))

# compressible assets of odd sizes (last block partial)
for i in range(6):
    write(f"data/level{i}/world_{i}.dat2", compressible(15 * 1024 * 1024 + i * 12345, i))
write("Data/Textures/Env_Castle.ktx2", compressible(5 * 1024 * 1024 + 777, 99))

# shared blocks: the same 64 KiB block repeated and present in two files (dedup)
shared = compressible(BLOCK, 42)
write("data/shared_a.bin2", shared * 120 + b"tail")
write("data/shared_b.bin2", b"head" + shared * 80)

# incompressible big file -> auto-loose
write("data/streaming/pak_world.bin2", os.urandom(70 * 1024 * 1024))
# incompressible small file -> packed RAW blocks
write("data/random_small.dat2", os.urandom(200000))

# tiny / empty / odd names
write("data/tiny.txt", b"0123456789")
write("data/empty.txt", b"")
write("data/with space/café été.txt", compressible(70000, 7))

# loose by rule
write("movies/intro.mp4", os.urandom(2 * 1024 * 1024))
write("audio/bank.wem", os.urandom(300000))
write("config/settings.json", b"{\"a\": 1}\n")

# dump leftovers that the cleanup option removes
write("_DUPLEX_/junk.txt", b"junk" * 1000)
write("MyProject/Saved/Logs/log.txt", b"log" * 1000)

# per-game profile picked up from <source>/exfat2ampr.toml: every block size / layout / runtime path
write("exfat2ampr.toml", b"""
[pack]
compression_level = 6
default_block_size = "64KiB"

[runtime]
decoded_cache_bytes = "96MiB"
physical_cache_bytes = "16MiB"
workers = 3
latency_reserve_workers = 1

[[rule]]
action = "compress"
include = ["data/level0/**", "data/level1/**"]
block_size = "1MiB"
layout = "streaming"

[[rule]]
action = "compress"
include = ["data/level2/**"]
block_size = "16KiB"
hot = true

[[rule]]
action = "store"
include = ["data/random_small.dat2"]
block_size = "32KiB"
layout = "random"

[[rule]]
action = "compress"
include = ["Data/Textures/**"]
block_size = "256KiB"
layout = "mixed"
""")

total = sum(os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(root) for f in fs)
print(f"{root}: {total / 1048576:.1f} MiB")
