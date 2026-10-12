#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
"""Draws docs/tested-on-ps5.svg, the "tested and working on PlayStation 5" card shown by the README and the
release notes.

The text is turned into outlines (DejaVu Sans from .deps/dejavu, see app/tools/fetch-deps.sh), so the card looks
the same in every browser and viewer, without fonts; the app icon is embedded. Standard library only:

  py tools/make_tested_badge.py"""
import base64
import os
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_R = os.path.join(ROOT, ".deps/dejavu/ttf/DejaVuSans.ttf")
FONT_B = os.path.join(ROOT, ".deps/dejavu/ttf/DejaVuSans-Bold.ttf")
ICON = os.path.join(ROOT, "app/sce_sys/icon0.png")
OUT = os.path.join(ROOT, "docs/tested-on-ps5.svg")

W, H = 1280, 640
X0, RIGHT = 480, 1210                     # the text column
TX, TY, TS = 85, 140, 330                 # the app icon's tile
GREEN, INK, SOFT, FAINT = "#1e9e4a", "#0f1b33", "#3d4b66", "#6b7894"


class Font:
    """the parts of a TrueType font needed to draw text: cmap (format 4), advances, kerning, glyph outlines"""

    def __init__(self, path):
        self.d = d = open(path, "rb").read()
        self.t = {}
        for i in range(struct.unpack(">H", d[4:6])[0]):
            tag, _, off, ln = struct.unpack(">4sIII", d[12 + 16 * i: 28 + 16 * i])
            self.t[tag.decode("latin-1")] = off
        head, hhea = self.t["head"], self.t["hhea"]
        self.upm = struct.unpack(">H", d[head + 18: head + 20])[0]
        self.long_loca = struct.unpack(">h", d[head + 50: head + 52])[0] == 1
        self.nhm = struct.unpack(">H", d[hhea + 34: hhea + 36])[0]
        self.cmap = self._cmap()
        self.kern = self._kern()

    def _u16(self, at, n=1):
        return struct.unpack(f">{n}H", self.d[at: at + 2 * n])

    def _cmap(self):
        d, base = self.d, self.t["cmap"]
        for i in range(self._u16(base + 2)[0]):
            pid, eid, off = struct.unpack(">HHI", d[base + 4 + 8 * i: base + 12 + 8 * i])
            sub = base + off
            if (pid, eid) == (3, 1) and self._u16(sub)[0] == 4:
                break
        else:
            raise SystemExit("no Unicode cmap (format 4) in the font")
        seg = self._u16(sub + 6)[0] // 2
        ends = self._u16(sub + 14, seg)
        starts = self._u16(sub + 16 + 2 * seg, seg)
        deltas = struct.unpack(f">{seg}h", d[sub + 16 + 4 * seg: sub + 16 + 6 * seg])
        ro = sub + 16 + 6 * seg
        ranges = self._u16(ro, seg)
        m = {}
        for k in range(seg):
            for c in range(starts[k], ends[k] + 1):
                if c == 0xFFFF:
                    continue
                if ranges[k] == 0:
                    g = (c + deltas[k]) & 0xFFFF
                else:
                    g = self._u16(ro + 2 * k + ranges[k] + 2 * (c - starts[k]))[0]
                    g = (g + deltas[k]) & 0xFFFF if g else 0
                m[c] = g
        return m

    def _kern(self):
        """pairs of the first horizontal subtable of the old 'kern' table, when the font has one"""
        if "kern" not in self.t:
            return {}
        base = self.t["kern"]
        version, count = self._u16(base, 2)
        if version != 0 or count == 0:
            return {}
        cov = self._u16(base + 8)[0]
        if cov >> 8 != 0 or not cov & 1:
            return {}
        pairs = {}
        p = base + 18
        for i in range(self._u16(base + 10)[0]):
            left, right, v = struct.unpack(">HHh", self.d[p + 6 * i: p + 6 * i + 6])
            pairs[(left, right)] = v
        return pairs

    def advance(self, g):
        return self._u16(self.t["hmtx"] + 4 * min(g, self.nhm - 1))[0]

    def contours(self, g):
        """the outline of glyph g: contours of (x, y, on_curve) in font units"""
        d, loca = self.d, self.t["loca"]
        if self.long_loca:
            a, b = struct.unpack(">II", d[loca + 4 * g: loca + 4 * g + 8])
        else:
            a, b = (2 * v for v in self._u16(loca + 2 * g, 2))
        if a == b:
            return []
        p = self.t["glyf"] + a
        nc = struct.unpack(">h", d[p: p + 2])[0]
        if nc < 0:   # composite: other glyphs, moved and scaled
            out, q = [], p + 10
            while True:
                flags, gi = self._u16(q, 2)
                q += 4
                if flags & 1:
                    dx, dy = struct.unpack(">hh", d[q: q + 4])
                    q += 4
                else:
                    dx, dy = struct.unpack(">bb", d[q: q + 2])
                    q += 2
                xx, yx, xy, yy = 1.0, 0.0, 0.0, 1.0
                if flags & 0x8:
                    xx = yy = struct.unpack(">h", d[q: q + 2])[0] / 16384
                    q += 2
                elif flags & 0x40:
                    xx, yy = (v / 16384 for v in struct.unpack(">hh", d[q: q + 4]))
                    q += 4
                elif flags & 0x80:
                    xx, yx, xy, yy = (v / 16384 for v in struct.unpack(">hhhh", d[q: q + 8]))
                    q += 8
                if not flags & 2:   # anchored by point numbers: not used by DejaVu
                    dx = dy = 0
                for c in self.contours(gi):
                    out.append([(xx * x + xy * y + dx, yx * x + yy * y + dy, on) for x, y, on in c])
                if not flags & 0x20:
                    return out
        ends = self._u16(p + 10, nc)
        npts = ends[-1] + 1
        q = p + 10 + 2 * nc
        q += 2 + self._u16(q)[0]   # instructions
        flags = []
        while len(flags) < npts:
            f = d[q]
            q += 1
            flags.append(f)
            if f & 8:
                flags.extend([f] * d[q])
                q += 1
        xs, ys = [], []
        for coords, short, same in ((xs, 2, 16), (ys, 4, 32)):
            v = 0
            for f in flags:
                if f & short:
                    v += d[q] if f & same else -d[q]
                    q += 1
                elif not f & same:
                    v += struct.unpack(">h", d[q: q + 2])[0]
                    q += 2
                coords.append(v)
        out, s = [], 0
        for e in ends:
            out.append([(xs[i], ys[i], bool(flags[i] & 1)) for i in range(s, e + 1)])
            s = e + 1
        return out


def outline(contours, ox, oy, s):
    """SVG path data of contours with their origin at (ox, oy) (on the baseline), s pixels per font unit"""
    def pt(x, y):
        return f"{ox + x * s:.1f} {oy - y * s:.1f}"
    parts = []
    for c in contours:
        if not c:
            continue
        k = next((i for i, p in enumerate(c) if p[2]), None)
        if k is None:   # only control points: start between the last and the first
            start, pts = ((c[-1][0] + c[0][0]) / 2, (c[-1][1] + c[0][1]) / 2, True), c
        else:
            start, pts = c[k], c[k + 1:] + c[:k]
        parts.append("M" + pt(start[0], start[1]))
        ctrl = None
        for x, y, on in pts + [start]:
            if on:
                parts.append(("Q" + pt(*ctrl) + " " if ctrl else "L") + pt(x, y))
                ctrl = None
            else:
                if ctrl:   # two control points in a row: an on-curve point between them
                    parts.append("Q" + pt(*ctrl) + " " + pt((ctrl[0] + x) / 2, (ctrl[1] + y) / 2))
                ctrl = (x, y)
        parts.append("Z")
    return "".join(parts)


def text(font, s, x, y, size, spacing=0.0):
    """(path data, width in pixels) of s with its baseline at y"""
    sc = size / font.upm
    out, pen, prev = [], 0.0, None
    for ch in s:
        g = font.cmap.get(ord(ch), 0)
        if prev is not None:
            pen += font.kern.get((prev, g), 0) + spacing / sc
        out.append(outline(font.contours(g), x + pen * sc, y, sc))
        pen += font.advance(g)
        prev = g
    return "".join(out), pen * sc


def fit(font, s, size, width):
    """the size at which s fits in width"""
    return min(size, size * width / text(font, s, 0, 0, size)[1])


def check(cx, cy, r, color):
    return (f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="{color}"/>'
            f'<path d="M{cx - 0.45 * r:.1f} {cy + 0.02 * r:.1f} L{cx - 0.12 * r:.1f} {cy + 0.34 * r:.1f} '
            f'L{cx + 0.47 * r:.1f} {cy - 0.31 * r:.1f}" fill="none" stroke="#fff" stroke-width="{0.19 * r:.1f}" '
            f'stroke-linecap="round" stroke-linejoin="round"/>')


def main():
    reg, bold = Font(FONT_R), Font(FONT_B)
    icon = base64.b64encode(open(ICON, "rb").read()).decode()
    k = TS / 413   # the icon's dark tile spans x 49..461, y 55..461 of its 512 pixels: only the tile is shown
    el = []

    def write(font, s, x, y, size, color, spacing=0.0):
        d, w = text(font, s, x, y, size, spacing)
        el.append(f'<path fill="{color}" d="{d}"/>')
        return w

    # the badge line, the claim (English, then French), the setup it was tested on
    el.append(check(X0 + 24, 186, 24, GREEN))
    write(bold, "TESTED BY THE AUTHOR", X0 + 62, 195, 24, GREEN, spacing=1.5)
    head = min(fit(bold, "Tested & working", 66, RIGHT - X0), fit(bold, "on PlayStation 5", 66, RIGHT - X0))
    write(bold, "Tested & working", X0, 272, head, INK)
    write(bold, "on PlayStation 5", X0, 344, head, INK)
    fr = "Testé et fonctionnel sur PlayStation 5"
    write(reg, fr, X0, 398, fit(reg, fr, 30, RIGHT - X0), SOFT)
    x = X0
    for chip in ("Firmware 12.xx & 13.xx", "kstuff", "ShadowMountPlus 1.7"):
        w = text(reg, chip, 0, 0, 22)[1]
        el.append(f'<rect x="{x}" y="430" width="{w + 36:.1f}" height="46" rx="23" fill="#e9eef8" stroke="#c8d3e8" stroke-width="1.5"/>')
        write(reg, chip, x + 18, 461, 22, INK)
        x += w + 36 + 12

    el.append(f'<line x1="70" y1="532" x2="{W - 70}" y2="532" stroke="#dde4f0" stroke-width="2"/>')
    write(reg, "exFAT2AMPR  ·  by V-10  ·  LZ4 format: DLPSGAME  ·  free software, GPL-3.0", 70, 584, 22, FAINT)

    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="{W}" height="{H}" '
           f'viewBox="0 0 {W} {H}">\n<title>exFAT2AMPR: tested and working on PlayStation 5 (firmware 12.xx and 13.xx, kstuff, '
           f'ShadowMountPlus 1.7)</title>\n<defs>'
           '<linearGradient id="bg" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#ffffff"/>'
           '<stop offset="1" stop-color="#e8f0fb"/></linearGradient>'
           '<filter id="shadow" x="-20%" y="-20%" width="140%" height="140%"><feDropShadow dx="0" dy="10" '
           'stdDeviation="14" flood-color="#1b2a4a" flood-opacity="0.18"/></filter></defs>\n'
           f'<rect x="1" y="1" width="{W - 2}" height="{H - 2}" rx="38" fill="url(#bg)" stroke="#d3dcea" stroke-width="2"/>\n'
           f'<clipPath id="tile"><rect x="{TX}" y="{TY}" width="{TS}" height="{TS * 407 / 413:.1f}" rx="{48 * k:.1f}"/></clipPath>\n'
           f'<g filter="url(#shadow)"><image x="{TX - 49 * k:.1f}" y="{TY - 55 * k:.1f}" width="{512 * k:.1f}" '
           f'height="{512 * k:.1f}" clip-path="url(#tile)" xlink:href="data:image/png;base64,{icon}"/></g>\n'
           + "\n".join(el) + "\n</svg>\n")
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(svg)
    print(f"{OUT}: {len(svg) // 1024} KiB")


if __name__ == "__main__":
    main()
