#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
"""Fake exfat2ampr engine API for the app's host screenshots (CI).

usage: mock_engine.py PORT VERSION

Scenarios (environment):
  MOCK_STORAGE=none|one|several   drives connected (default: several)
  MOCK_UNPLUG_AFTER=SECONDS        USB 1 is removed after that many seconds
  MOCK_GAME=PPSA01342              a game is running (game mode on)
  MOCK_RUNNING=1                   a conversion is already running at start
  MOCK_PAUSED_REASON="..."         the conversion is paused with this engine message
  MOCK_RATE=2                      percent per second while a conversion runs
  MOCK_START_PCT=37                where a started conversion begins
  MOCK_FINISH_AFTER=SECONDS        the conversion ends ("done") after running that long
  MOCK_SMP_WAIT=SECONDS            a started image first waits for ShadowMountPlus (busy, game_active)
  MOCK_FAIL=CODE                   a started conversion fails after 2 s with this result code
  MOCK_START_ERROR=CODE            the engine starts with a failed conversion (this code) to show
  MOCK_CJK=1                       three games titled in Chinese, Japanese and Korean come first
  MOCK_DEMO=1                      a made-up catalogue with drawn covers and key art (presentation video)

Key art (?kind=bg, the app's wallpapers) exists for some folder games only: the others fall back
to their blurred cover, like games without sce_sys/pic0.png."""
import io
import json
import os
import sys
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

PORT, VERSION = int(sys.argv[1]), sys.argv[2]
T0 = time.time()
GB = 1024 ** 3

GAMES = [
    {"type": "folder", "path": "/data/homebrew/PPSA01342", "title_id": "PPSA01342", "title": "Astro's Playroom Deluxe",
     "version": "01.003", "ampr": False, "partial": False, "selectable": True},
    {"type": "image", "path": "/mnt/usb0/homebrew/PPSA02530.exfat", "smp_title": "PPSA02530", "title_id": "PPSA02530",
     "title": "Ratchet & Clank: Rift Apart", "version": "01.004", "image_type": "exfat", "mounted": False, "mount": "",
     "size": 41234567890, "selectable": True, "reason": "", "fakelib": True},
    {"type": "image", "path": "/mnt/ext0/homebrew/PPSA03016.exfat", "smp_title": "PPSA03016", "title_id": "PPSA03016",
     "title": "Демо: Красный город (русская версия)", "version": "01.000", "image_type": "exfat", "mounted": False, "mount": "",
     "size": 18234567890, "selectable": True, "reason": "", "fakelib": True},
    {"type": "folder", "path": "/data/homebrew/PPSA04609", "title_id": "PPSA04609", "title": "Jeu déjà converti — édition complète",
     "version": "02.010", "ampr": True, "partial": False, "selectable": True},
    {"type": "folder", "path": "/mnt/usb1/homebrew/PPSA05540", "title_id": "PPSA05540", "title": "Conversion interrompue (test)",
     "version": "01.000", "ampr": False, "partial": True, "selectable": True},
    {"type": "image", "path": "/mnt/usb0/homebrew/copie/PPSA02530.exfat", "smp_title": "", "title_id": "PPSA02530",
     "title": "Ratchet & Clank: Rift Apart",
     "version": "", "image_type": "", "mounted": False, "mount": "", "size": 41234567890, "selectable": False,
     "reason": "not_managed", "fakelib": True},
    {"type": "folder", "path": "/data/homebrew/PPSA90003", "title_id": "PPSA90003", "title": "Jeu sans AMPR (test)",
     "version": "01.000", "ampr": False, "partial": False, "selectable": True},
] + [
    {"type": "folder", "path": f"/data/homebrew/PPSA9{i:04d}", "title_id": f"PPSA9{i:04d}", "title": f"Homebrew game {i}",
     "version": "01.000", "ampr": False, "partial": False, "selectable": True} for i in (1, 2, 4, 5, 6)
]
if os.environ.get("MOCK_CJK"):   # made-up titles: the glyphs the app loads at start (assets/glyphs_cjk.bin)
    GAMES[:0] = [
        {"type": "folder", "path": f"/data/homebrew/PPSA9001{i}", "title_id": f"PPSA9001{i}", "title": t,
         "version": "01.000", "ampr": False, "partial": False, "selectable": True}
        for i, t in enumerate(("示例游戏：星海远征 豪华版", "サンプルゲーム：ほしのかなた", "샘플 게임: 별의 바다"))
    ]

INTERNAL = {"path": "/data/homebrew", "mount": "/data", "device": "/dev/ssd0.user", "kind": "internal", "index": -1,
            "label": "PS5 internal storage", "fstype": "UFS", "total": int(628.4 * GB), "free": int(199.1 * GB), "writable": True, "reason": ""}
USB0 = {"path": "/mnt/usb0/homebrew", "mount": "/mnt/usb0", "device": "/dev/da0s1", "kind": "usb", "index": 0, "label": "USB 0",
        "fstype": "exFAT", "total": int(931.5 * GB), "free": int(119.3 * GB), "writable": True, "reason": ""}
USB1 = {"path": "/mnt/usb1/homebrew", "mount": "/mnt/usb1", "device": "/dev/da1s1", "kind": "usb", "index": 1, "label": "USB 1",
        "fstype": "exFAT", "total": int(1863 * GB), "free": int(1402.7 * GB), "writable": True, "reason": ""}
USB3 = {"path": "/mnt/usb3/homebrew", "mount": "/mnt/usb3", "device": "/dev/da2s1", "kind": "usb", "index": 3, "label": "USB 3",
        "fstype": "NTFS", "total": int(58.6 * GB), "free": int(12.0 * GB), "writable": False, "reason": "read_only"}
EXT0 = {"path": "/mnt/ext0/homebrew", "mount": "/mnt/ext0", "device": "/dev/nvme1", "kind": "extended", "index": 0, "label": "Extended storage 0",
        "fstype": "UFS", "total": int(931.5 * GB), "free": int(40.2 * GB), "writable": True, "reason": ""}

RESUME = [
    {"dest": "/mnt/usb1/homebrew/PPSA05540", "source": "/mnt/usb1/homebrew/PPSA05540", "name": "Conversion interrompue (test)",
     "stage": "pack", "source_available": True, "active": False, "level": 9},
    {"dest": "/data/homebrew/PPSA07777", "source": "/mnt/usb2/homebrew/PPSA07777.exfat", "name": "Source débranchée",
     "stage": "copy", "source_available": False, "active": False, "level": 12},
]
if os.environ.get("MOCK_DEMO"):   # invented titles only: nothing that belongs to a real game
    def _demo(kind, path, tid, title, size, **kw):
        g = {"type": kind, "path": path, "title_id": tid, "title": title, "version": "01.000", "size": size}
        if kind == "image":
            g.update(smp_title=tid, image_type="exfat", mounted=False, mount="", selectable=True, reason="", fakelib=True)
        else:
            g.update(ampr=False, partial=False, selectable=True, plugins="ok")
        g.update(kw)
        return g
    GAMES[:] = [
        _demo("folder", "/data/homebrew/PPSA90101", "PPSA90101", "Starfall Odyssey", 38 * GB, plugins="update"),
        _demo("image", "/mnt/usb0/homebrew/PPSA90102.exfat", "PPSA90102", "Neon Drift Racing", 46 * GB),
        _demo("image", "/mnt/ext0/homebrew/PPSA90103.exfat", "PPSA90103", "Kingdom of Ash", 72 * GB),
        _demo("folder", "/mnt/usb0/homebrew/PPSA90104", "PPSA90104", "Echoes of the Deep", 29 * GB, ampr=True),
        _demo("folder", "/data/homebrew/PPSA90105", "PPSA90105", "Skyline Tactics", 21 * GB, plugins="missing"),
        _demo("image", "/mnt/usb1/backup/PPSA90102.exfat", "PPSA90102", "Neon Drift Racing", 46 * GB,
              smp_title="", selectable=False, reason="not_managed"),
        _demo("folder", "/mnt/ext0/homebrew/PPSA90106", "PPSA90106", "Moonlit Harbor", 33 * GB, plugins="unknown"),
        _demo("folder", "/data/homebrew/PPSA90107", "PPSA90107", "Pixel Garden", 2 * GB, plugins="none"),
    ]

# the game plugins (pgo_stub, dlc_emu) the engine keeps up to date: what /api/plugins answers
PLUGIN_FILES = [("libScePlayGo.sprx", "pgo_stub", "0.6"), ("libSceAppContent.sprx", "dlc_emu", "0.3"),
                ("libSceGameUpdate.sprx", "dlc_emu", "0.3"), ("libSceNpEntitlementAccess.sprx", "dlc_emu", "0.3")]
PLUGIN_LAST = {"at": int(time.time()) - 900, "running": False, "games": 5, "updated": 1, "failed": 0}


def plugins_info():
    return {"embedded": [{"name": n, "project": p, "version": v, "size": 20000} for n, p, v in PLUGIN_FILES],
            "emus": {"dir": "/data/shadowmount/emus", "written": 0, "report": ""}, "last": PLUGIN_LAST}


def plugins_detail(path):
    g = next((x for x in GAMES if x["path"] == path), None)
    state = (g or {}).get("plugins", "none")
    per = {"ok": "current", "update": "old", "missing": "absent", "unknown": "unknown", "none": "absent"}[state]
    files = []
    for i, (n, p, v) in enumerate(PLUGIN_FILES):
        st = per if i == 0 or state != "missing" else "current"   # "missing": the PlayGo stub is the absent one
        files.append({"name": n, "project": p, "state": st, "version": "0.5" if st == "old" else v if st == "current" else "",
                      "latest": v, "needed": state != "none"})
    return {"state": state, "files": files}
    RESUME[:] = []

TRACES = {}   # path -> {trace_mode, pending, collected, profile}: the optimization of a folder game


def traces_json():
    out = []
    for g in GAMES:
        if g["type"] != "folder" or "/homebrew/" not in g["path"] or g["path"].count("/") > 4:
            continue
        t = TRACES.setdefault(g["path"], {"trace_mode": False, "pending": False, "collected": 0, "profile": False})
        out.append({"path": g["path"], "title_id": g["title_id"], "title": g["title"], "converted": bool(g.get("ampr")), **t})
    return out


if os.environ.get("MOCK_TRACE_STATE"):   # e.g. "collected=2,pending=1": a game already halfway through
    traces_json()
    first = next(iter(TRACES.values()), None)
    for kv in os.environ["MOCK_TRACE_STATE"].split(","):
        k, v = kv.split("=")
        if first is not None:
            first[k] = int(v) if k == "collected" else v == "1"

# the app's update: the manifest (version MOCK_UPDATE, else none newer) and a small package
import hashlib  # noqa: E402
UPDATE_PKG = bytes((i * 7919) & 0xFF for i in range(3 * 1024 * 1024 + 12345))
UPDATE = {"got": b"", "size": 0, "sha": ""}


def update_manifest():
    return {"version": os.environ.get("MOCK_UPDATE", "0.0"), "package": f"http://127.0.0.1:{PORT}/update/PPSA99272.ffpkg",
            "size": len(UPDATE_PKG), "sha256": hashlib.sha256(UPDATE_PKG).hexdigest()}


PAIR = {"pin": "482913", "devices": 1}
SETTINGS = {"game_mode": True}
STATE = {"state": "idle", "started": 0.0, "queue": [], "title": "", "title_id": "", "elapsed": 0.0, "message": "", "code": "",
         "reason": "", "wait_until": 0.0, "fail_at": 0.0, "retry": False}
RUN_TITLE, RUN_TID = ("Neon Drift Racing", "PPSA90102") if os.environ.get("MOCK_DEMO") else ("Ratchet & Clank: Rift Apart", "PPSA02530")
if os.environ.get("MOCK_RUNNING") or os.environ.get("MOCK_PAUSED_REASON"):
    reason = os.environ.get("MOCK_PAUSED_REASON", "")
    STATE.update(state="paused" if reason else "running", started=time.time(),
                 title=RUN_TITLE, title_id=RUN_TID, message=reason,
                 code="paused_game" if reason.startswith("game launched") else "paused_dest" if "disconnected" in reason else "")
if os.environ.get("MOCK_START_ERROR"):
    STATE.update(state="error", title=RUN_TITLE, title_id=RUN_TID, code=os.environ["MOCK_START_ERROR"],
                 message="ShadowMountPlus is busy (Device busy): close the running game or unmount the other game first", retry=True)
COLORS = [(220, 60, 60), (60, 120, 230), (60, 180, 120), (230, 160, 40), (150, 80, 220), (40, 170, 200)]
ART_TITLES = {"PPSA01342": "Astro's Playroom Deluxe", "PPSA04609": "Édition complète", "PPSA90003": "Sans AMPR"}
if os.environ.get("MOCK_DEMO"):
    ART_TITLES = {g["title_id"]: g["title"] for g in GAMES}
ART_CACHE = {}
DELETE = {"active": False, "until": 0.0, "path": "", "title": "", "seq": 0, "ok": False, "code": ""}
RATE = float(os.environ.get("MOCK_RATE", "2"))
START_PCT = float(os.environ.get("MOCK_START_PCT", "37"))
FINISH_AFTER = float(os.environ.get("MOCK_FINISH_AFTER", "0"))


def unplugged():
    after = os.environ.get("MOCK_UNPLUG_AFTER")
    return bool(after) and time.time() - T0 > float(after)


# measured speeds (MB/s): what /api/speedtest finds on each drive; MOCK_SPEED=1 shows them from the start
TRUE_SPEED = {"/data/homebrew": (3150, 2380), "/mnt/usb0/homebrew": (392, 318), "/mnt/usb1/homebrew": (96, 84),
              "/mnt/usb3/homebrew": (-1, 41), "/mnt/ext0/homebrew": (5240, 4310)}
SPEED = dict(TRUE_SPEED) if os.environ.get("MOCK_SPEED") else {}


def with_speed(s):
    r, w = SPEED.get(s["path"], (-1, -1))
    return dict(s, read_mbps=r, write_mbps=w)


def storages():
    scenario = os.environ.get("MOCK_STORAGE", "several")
    if scenario == "none":
        return [INTERNAL]
    if scenario == "one":
        return [INTERNAL, USB0]
    out = [INTERNAL, USB0] + ([] if unplugged() else [USB1]) + [USB3, EXT0]
    return out


def storage_rev():
    return (1 if unplugged() else 0) + {"none": 0, "one": 10, "several": 20}.get(os.environ.get("MOCK_STORAGE", "several"), 0)


def demo_cover(tid, title):
    """a drawn cover for the presentation: gradient, soft shapes, the title"""
    h = sum(map(ord, tid))
    top, bottom = COLORS[h % len(COLORS)], COLORS[(h // 5 + 3) % len(COLORS)]
    img = Image.new("RGB", (512, 512))
    d = ImageDraw.Draw(img)
    for y in range(512):
        t = y / 511
        d.line([(0, y), (512, y)], fill=tuple(int(a * (1 - t) * 0.9 + b * t * 0.45) for a, b in zip(top, bottom)))
    glow = Image.new("RGB", (512, 512), (0, 0, 0))
    g = ImageDraw.Draw(glow)
    for i in range(5):
        x, y, r = (h * (i + 3) * 37) % 512, (h * (i + 5) * 23) % 300, 40 + (h * (i + 1)) % 90
        g.ellipse([x - r, y - r, x + r, y + r], fill=(255, 235, 200))
    img = ImageChops.add(img, ImageChops.multiply(glow.filter(ImageFilter.GaussianBlur(28)), Image.new("RGB", (512, 512), (110, 100, 90))))
    d = ImageDraw.Draw(img)
    try:
        font = ImageFont.truetype("DejaVuSans-Bold.ttf", 58)
    except OSError:
        font = ImageFont.load_default()
    words, lines = title.upper().split(), []
    for w in words:
        if lines and d.textlength(lines[-1] + " " + w, font=font) < 440:
            lines[-1] += " " + w
        else:
            lines.append(w)
    y = 470 - 66 * len(lines)
    for ln in lines:
        d.text((40, y), ln, font=font, fill=(255, 255, 255), stroke_width=2, stroke_fill=(0, 0, 0))
        y += 66
    out = io.BytesIO()
    img.save(out, "PNG")
    return out.getvalue()


def cover(tid):
    if os.environ.get("MOCK_DEMO") and tid in ART_TITLES:
        return demo_cover(tid, ART_TITLES[tid])
    h = sum(map(ord, tid)) if tid else 0
    c = COLORS[h % len(COLORS)]
    img = Image.new("RGB", (512, 512), c)
    d = ImageDraw.Draw(img)
    for i in range(0, 512, 8):
        d.line([(0, i), (512, i)], fill=tuple(max(0, v - i // 6) for v in c))
    try:
        font = ImageFont.truetype("DejaVuSans-Bold.ttf", 120)
    except OSError:
        font = ImageFont.load_default()
    d.text((256, 256), tid[-4:] if tid else "?", font=font, fill=(255, 255, 255), anchor="mm")
    out = io.BytesIO()
    img.save(out, "PNG")
    return out.getvalue()


def key_art(tid):
    """1920x1080 'key art': sky gradient, soft lights, hills, the title (stands in for sce_sys/pic0.png)."""
    if tid in ART_CACHE:
        return ART_CACHE[tid]
    h = sum(map(ord, tid))
    top, bottom = COLORS[h % len(COLORS)], COLORS[(h // 7 + 2) % len(COLORS)]
    img = Image.new("RGB", (1920, 1080))
    d = ImageDraw.Draw(img)
    for y in range(1080):
        t = y / 1079
        d.line([(0, y), (1920, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)))
    glow = Image.new("RGB", (1920, 1080), (0, 0, 0))
    g = ImageDraw.Draw(glow)
    for i in range(9):
        x, y, r = (h * (i + 3) * 97) % 1920, (h * (i + 5) * 53) % 700, 60 + (h * (i + 1)) % 180
        g.ellipse([x - r, y - r, x + r, y + r], fill=(255, 240, 210))
    glow = glow.filter(ImageFilter.GaussianBlur(40))
    img = ImageChops.add(img, ImageChops.multiply(glow, Image.new("RGB", (1920, 1080), (120, 110, 100))))
    d = ImageDraw.Draw(img)
    for layer, shade in ((0, 0.55), (1, 0.32)):
        pts = [(0, 1080)]
        for x in range(0, 1921, 160):
            pts.append((x, 700 + layer * 120 + ((h * (x + 11 + layer * 37)) % 160)))
        pts.append((1920, 1080))
        d.polygon(pts, fill=tuple(int(c * shade) for c in bottom))
    try:
        font = ImageFont.truetype("DejaVuSans-Bold.ttf", 110)
    except OSError:
        font = ImageFont.load_default()
    d.text((1830, 960), ART_TITLES.get(tid, tid), font=font, fill=(255, 255, 255), anchor="rs", stroke_width=3, stroke_fill=(0, 0, 0))
    out = io.BytesIO()
    img.save(out, "PNG")
    ART_CACHE[tid] = out.getvalue()
    return ART_CACHE[tid]


def state_json():
    now = time.time()
    if DELETE["active"] and now >= DELETE["until"]:   # the copy is gone
        GAMES[:] = [g for g in GAMES if g["path"] != DELETE["path"]]
        DELETE.update(active=False, ok=True, code="deleted", seq=DELETE["seq"] + 1)
    if STATE["state"] == "running" and STATE["wait_until"] and now >= STATE["wait_until"]:
        STATE.update(wait_until=0.0, code="", reason="", started=now, elapsed=0)   # ShadowMountPlus mounted the image
    if STATE["state"] == "running" and STATE["fail_at"] and now >= STATE["fail_at"]:
        STATE.update(state="error", fail_at=0.0, code=os.environ.get("MOCK_FAIL", "unknown"), message="pack failed (see log)", retry=True)
    s = STATE["state"]
    waiting = s == "running" and bool(STATE["wait_until"])
    pct, done = 0, 0
    total = 41234567890.0
    if waiting:
        STATE["started"] = now
    elif s in ("running", "paused"):
        if s == "running":
            STATE["elapsed"] = STATE.get("elapsed", 0) + (time.time() - STATE["started"])
            STATE["started"] = time.time()
        pct = min(99, int(START_PCT + STATE.get("elapsed", 0) * RATE))
        done = total * pct / 100
        if s == "running" and FINISH_AFTER and STATE["elapsed"] >= FINISH_AFTER:
            STATE["state"] = s = "done"
    if s == "done":
        pct, done = 100, total
    game = os.environ.get("MOCK_GAME", "")
    if s == "done" and not STATE["code"]:
        STATE["code"] = "done"
    return {"version": VERSION, "state": s, "phase": "waiting" if waiting else "pack" if s != "idle" else "",
            "message": STATE["message"] or "data/level3/world_3.pak", "code": STATE["code"], "reason": STATE["reason"],
            "waiting": waiting, "can_retry": s in ("error", "cancelled") and STATE["retry"], "resumable": False,
            "deleting": DELETE["active"], "delete_seq": DELETE["seq"], "delete_ok": DELETE["ok"], "delete_code": DELETE["code"],
            "delete_path": DELETE["path"], "delete_title": DELETE["title"],
            "percent": pct, "bytes_total": total, "bytes_done": done, "speed_bps": 187e6 if not game else 61e6,
            "eta_seconds": 1342 if not game else 4120,
            "title_id": STATE["title_id"], "title": STATE["title"], "dest": "/mnt/usb0/homebrew/PPSA02530", "profile": "",
            "source_bytes": total, "packed_bytes": total * 0.61 if s == "done" else 0, "shared_bytes": 0, "started": 0, "finished": 0,
            "queue": STATE["queue"], "runtime": {}, "storage_rev": storage_rev(), "game_running": game,
            "game_mode": bool(game) and SETTINGS["game_mode"], "game_mode_enabled": SETTINGS["game_mode"], "log": ""}


def check(path, smp, dest):
    game = next((g for g in GAMES if g["path"] == path), None)
    errors, warnings = [], []
    if not game:
        errors.append("source_missing")
        game = {"title_id": "", "title": path, "type": "folder"}
    tid = game.get("title_id", "")
    src = game.get("size") or (23 * GB if tid != "PPSA01342" else 7 * GB)
    compat = "later" if game.get("type") == "image" else "yes"
    if game.get("type") == "image" and smp:   # the app is open: ShadowMountPlus will mount it once it is closed
        warnings.append("smp_wait_app")
    if tid == "PPSA90003":
        compat = "no"
        errors.append("not_ampr")
    if tid == "PPSA05540":
        compat = "unknown"
        warnings.append("ampr_unknown")
    if game.get("ampr"):
        warnings.append("already_ampr")
        warnings.append("already_exists")
    if tid and tid == os.environ.get("MOCK_GAME"):
        errors.append("game_running")
    d = next((s for s in storages() if s["path"] == (dest or "/data/homebrew")), None)
    if not d:
        errors.append("dest_missing")
        free = total = 0
    else:
        free, total = d["free"], d["total"]
        if not d["writable"]:
            errors.append("dest_readonly")
        elif free < src / 3:
            errors.append("space_insufficient")
        elif free < src + 256 * 1024 * 1024:
            warnings.append("space_low")
    return {"title_id": tid, "title": game.get("title", ""), "source_bytes": src, "need_typical": src // 10 * 6,
            "need_max": src + 256 * 1024 * 1024, "dest_free": free, "dest_total": total, "compatible": compat,
            "errors": errors, "warnings": warnings}


APPLOG = []        # (seq, line) received from the app
APPLOG_SEQ = [0]


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, code, body, ctype="application/json"):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        if u.path == "/api/state":
            return self.send(200, state_json())
        if u.path == "/api/applog" or u.path == "/api/log":   # incremental reads: the lines after since=
            since = int(q.get("since", ["0"])[0] or 0)
            lines = [l for n, l in APPLOG if n > since] if u.path == "/api/applog" else []
            return self.send(200, {"seq": APPLOG_SEQ[0], "lines": lines})
        if u.path == "/api/sources":
            return self.send(200, GAMES)
        if u.path == "/update/update.json":
            return self.send(200, update_manifest())
        if u.path == "/update/PPSA99272.ffpkg":
            return self.send(200, UPDATE_PKG, "application/octet-stream")
        if u.path == "/api/traces":
            return self.send(200, traces_json())
        if u.path == "/api/plugins":
            path = q.get("path", [""])[0]
            return self.send(200, plugins_detail(path) if path else plugins_info())
        if u.path == "/api/destinations":
            return self.send(200, [with_speed(s) for s in storages()])
        if u.path == "/api/resumable":
            return self.send(200, RESUME)
        if u.path == "/api/settings":
            return self.send(200, SETTINGS)
        if u.path == "/api/pair/info":
            return self.send(200, {"pin": PAIR["pin"], "ip": "192.168.1.20", "port": 6972, "devices": PAIR["devices"]})
        if u.path == "/api/icon":
            tid = q.get("title_id", [""])[0] or q.get("path", [""])[0].rsplit("/", 1)[-1]
            if q.get("kind", [""])[0] == "bg":
                if tid in ART_TITLES:   # folder games have key art; images only through the console's app metadata
                    return self.send(200, key_art(tid), "image/png")
                return self.send(404, {"error": "no key art"})
            if tid.endswith("PPSA04609"):
                return self.send(404, {"error": "no icon"})
            return self.send(200, cover(tid), "image/png")
        self.send(404, {"error": "not found"})

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(n)
        u = urllib.parse.urlparse(self.path)
        path = u.path
        if path == "/api/plugins/refresh":
            form = urllib.parse.parse_qs(raw.decode("utf-8", "replace"))
            p = form.get("path", [""])[0]
            if not p:   # every game, in the background
                PLUGIN_LAST.update(at=int(time.time()), running=False, games=5, updated=2)
                return self.send(200, {"ok": True, "started": True})
            g = next((x for x in GAMES if x["path"] == p), None)
            if not g:
                return self.send(400, {"error": "bad game folder"})
            written = 1 if g.get("plugins") in ("update", "missing") else 0
            if written:
                g["plugins"] = "ok"
            return self.send(200, {"ok": True, "written": written, "report": "libScePlayGo.sprx 0.5 -> 0.6" if written else ""})
        if path == "/api/update/chunk":   # binary pieces of the package, in order
            off = int(urllib.parse.parse_qs(u.query).get("off", ["-1"])[0])
            if off != len(UPDATE["got"]):
                return self.send(400, {"error": "piece out of order"})
            UPDATE["got"] += raw
            return self.send(200, {"ok": True})
        if path == "/api/applog":   # the app's own log lines, printed so the CI log shows them
            for line in raw.decode("utf-8", "replace").splitlines():
                if line.strip():
                    APPLOG_SEQ[0] += 1
                    APPLOG.append((APPLOG_SEQ[0], line))
                    print("[applog] " + line, flush=True)
            del APPLOG[:-400]
            return self.send(200, {"ok": True, "seq": APPLOG_SEQ[0]})
        body = urllib.parse.parse_qs(raw.decode())
        arg = lambda k: body.get(k, [""])[0]
        if path == "/api/update/begin":
            UPDATE.update(got=b"", size=int(arg("size") or 0), sha=arg("sha256"))
            return self.send(200, {"ok": True})
        if path == "/api/update/install":
            ok = len(UPDATE["got"]) == UPDATE["size"] and hashlib.sha256(UPDATE["got"]).hexdigest() == UPDATE["sha"]
            time.sleep(0.5)
            return self.send(200, {"ok": True, "path": "/data/homebrew/PPSA99272.ffpkg"}) if ok else \
                self.send(400, {"error": "the download does not match the published SHA-256"})
        if path == "/api/speedtest":
            p = arg("path")
            if p not in TRUE_SPEED:
                return self.send(400, {"error": "bad destination"})
            time.sleep(1.2)
            SPEED[p] = TRUE_SPEED[p]
            return self.send(200, {"ok": True, "read_mbps": SPEED[p][0], "write_mbps": SPEED[p][1]})
        if path.startswith("/api/trace/"):
            op = path.rsplit("/", 1)[-1]
            traces_json()
            t = TRACES.get(arg("path")) or next((TRACES[g["path"]] for g in GAMES if g["title_id"] == arg("title_id") and g["path"] in TRACES), None)
            if t is None:
                return self.send(400, {"error": "bad game folder"})
            if op == "enable":
                t.update(trace_mode=True)
            elif op == "disable":
                t.update(trace_mode=False)
            elif op == "collect":
                if not t["pending"]:
                    return self.send(400, {"error": "no new journal: play the game in trace mode first"})
                t.update(pending=False, collected=t["collected"] + 1)
                return self.send(200, {"ok": True, "info": str(t["collected"])})
            elif op == "generate":
                if not t["collected"]:
                    return self.send(400, {"error": "no trace collected"})
                t.update(profile=True)
            return self.send(200, {"ok": True, "info": ""})
        if path == "/api/check":
            return self.send(200, check(arg("path"), arg("smp"), arg("dest")))
        if path == "/api/settings":
            if arg("game_mode"):
                SETTINGS["game_mode"] = arg("game_mode") == "1"
            return self.send(200, SETTINGS)
        if path == "/api/start":
            src = arg("path")
            game = next((g for g in GAMES if g["path"] == src), {"title": src, "title_id": ""})
            if not any(s["path"] == arg("dest") and s["writable"] for s in storages()):
                return self.send(400, {"error": "destination storage not available", "code": "dest_unavailable"})
            if STATE["state"] != "running":
                wait = float(os.environ.get("MOCK_SMP_WAIT", "0")) if arg("smp") else 0
                STATE.update(state="running", started=time.time(), elapsed=0, title=game["title"], title_id=game["title_id"], message="",
                             code="smp_busy" if wait else "", reason="game_active" if wait else "", retry=True,
                             wait_until=time.time() + wait if wait else 0.0,
                             fail_at=time.time() + wait + 2 if os.environ.get("MOCK_FAIL") else 0.0)
            else:
                STATE["queue"].append(game["title"])
            return self.send(200, {"ok": True, "position": len(STATE["queue"])})
        if path == "/api/pause":
            STATE["state"] = "paused"
        elif path == "/api/resume":
            STATE.update(state="running", started=time.time(), message="")
        elif path == "/api/pair/reset":
            PAIR.update(pin="105377", devices=0)
            return self.send(200, {"pin": PAIR["pin"], "ip": "192.168.1.20", "port": 6972, "devices": 0})
        elif path == "/api/cancel":
            STATE.update(state="cancelled", code="cancelled", wait_until=0.0, fail_at=0.0)
        elif path == "/api/retry":
            if STATE["state"] not in ("error", "cancelled") or not STATE["retry"]:
                return self.send(409, {"error": "nothing to retry", "code": "nothing_to_retry"})
            STATE.update(state="running", started=time.time(), elapsed=0, code="", reason="", message="", fail_at=0.0)
        elif path == "/api/delete-source":
            game = next((g for g in GAMES if g["path"] == arg("path")), None)
            if not game:
                return self.send(409, {"error": "refused", "code": "not_deletable"})
            if STATE["state"] == "running" and game["title"] == STATE["title"] and game.get("selectable"):
                return self.send(409, {"error": "refused", "code": "in_use"})
            if DELETE["active"]:
                return self.send(409, {"error": "refused", "code": "delete_busy"})
            DELETE.update(active=True, until=time.time() + 1.5, path=game["path"], title=game["title"] or game["path"], code="")
        elif path == "/api/dismiss":
            if STATE["state"] == "running":
                return self.send(409, {"error": "a conversion is running", "code": "busy"})
            STATE.update(state="idle", title="", title_id="", code="", reason="", message="", retry=False)
        self.send(200, {"ok": True})


ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
