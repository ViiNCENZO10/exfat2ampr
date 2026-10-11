#!/usr/bin/env python3
"""Rasterize the app's fonts into anti-aliased glyph tables for its CPU renderer.

usage: make_font.py DejaVuSans.ttf DejaVuSans-Bold.ttf OUT.c [EXTRA.bin]

Every script is drawn on DejaVu Sans' baseline and line height:
  Latin (Vietnamese included), Cyrillic, symbols   DejaVu Sans (Bitstream Vera / DejaVu license)
  Thai                                             Noto Sans Thai (SIL Open Font License 1.1)
  Chinese, Japanese, Korean                        Noto Sans CJK SC (SIL Open Font License 1.1)
The Noto fonts are looked for under /usr/share/fonts (Debian/Ubuntu: fonts-noto-core, fonts-noto-cjk), or
under the directories listed in E2A_FONT_DIRS.

OUT.c: FONT_SMALL, FONT_BODY, FONT_HEAD, FONT_TITLE and FONT_HUGE (font.h), with every character the app's
own sources write (all the interface languages) plus the Latin, Vietnamese, Cyrillic and Thai alphabets.
EXTRA.bin: the common CJK characters of game titles (GB2312, Big5 level 1, Japanese kana and level-1 kanji,
KS X 1001 Hangul) at the SMALL, BODY and HEAD sizes, 4-bit, zlib-compressed; read by gfx_load_extra()."""
import glob
import os
import struct
import sys
import unicodedata
import zlib

from PIL import Image, ImageDraw, ImageFont

regular, bold, out = sys.argv[1], sys.argv[2], sys.argv[3]
extra_out = sys.argv[4] if len(sys.argv) > 4 else None
HERE = os.path.dirname(os.path.abspath(__file__))
BASIC = getattr(getattr(ImageFont, "Layout", None), "BASIC", 0)   # no shaping: one glyph per character
FONT_DIRS = os.environ.get("E2A_FONT_DIRS", "/usr/share/fonts").split(os.pathsep)


def find_font(*names):
    for d in FONT_DIRS:
        for root, _, files in os.walk(d):
            for n in names:
                if n in files:
                    return os.path.join(root, n)
    seen = sorted({os.path.basename(p) for d in FONT_DIRS for p in glob.glob(os.path.join(d, "**", "Noto*"), recursive=True)})
    sys.exit(f"make_font.py: none of {names} under {FONT_DIRS} (install fonts-noto-core and fonts-noto-cjk). "
             f"Noto files there: {seen[:60]}")


THAI = (find_font("NotoSansThai-Regular.ttf", "NotoSansThai-Regular.otf", "NotoSansThaiUI-Regular.ttf"),
        find_font("NotoSansThai-Bold.ttf", "NotoSansThai-Bold.otf", "NotoSansThaiUI-Bold.ttf"))
CJK = (find_font("NotoSansCJK-Regular.ttc", "NotoSansCJKsc-Regular.otf", "NotoSansSC-Regular.otf"),
       find_font("NotoSansCJK-Bold.ttc", "NotoSansCJKsc-Bold.otf", "NotoSansSC-Bold.otf"))


def face_index(path):
    """the Simplified Chinese face of a collection (its glyph shapes for the characters shared with Japanese)"""
    if not path.endswith(".ttc"):
        return 0
    for i in range(16):
        try:
            name = ImageFont.truetype(path, 20, index=i).getname()[0]
        except OSError:
            break
        if " SC" in name and "Mono" not in name:
            return i
    return 0


INDEX = {p: face_index(p) for p in CJK}
print("fonts:", regular, bold, THAI, CJK, INDEX)


def script(cp):
    if 0x0E00 <= cp <= 0x0E7F:
        return "thai"
    if (0x1100 <= cp <= 0x11FF or 0x2E80 <= cp <= 0x9FFF or 0xAC00 <= cp <= 0xD7AF or 0xF900 <= cp <= 0xFAFF
            or 0xFE30 <= cp <= 0xFE4F or 0xFF00 <= cp <= 0xFFEF):
        return "cjk"
    return "latin"


_fonts = {}


def font_for(kind, is_bold, size):
    key = (kind, is_bold, size)
    if key not in _fonts:
        if kind == "latin":
            path, index = (bold if is_bold else regular), 0
        else:
            path = (THAI if kind == "thai" else CJK)[1 if is_bold else 0]
            index = INDEX.get(path, 0)
        f = ImageFont.truetype(path, size, index=index, layout_engine=BASIC)
        # pictures of a character that font does not have (.notdef), to leave them out
        _fonts[key] = (f, {raster(f, chr(c), 0)[2:] for c in (0x10FFFD, 0xE0FFF)})
    return _fonts[key]


def raster(font, ch, ascent):
    """(x, y, w, h, adv, coverage) of the ink: x from the pen, y from the top of a line whose baseline is at
    ascent. Measured on the drawn pixels: Pillow's own box for a lone mark (Thai vowels and tones) runs down to
    the baseline, which would lift the mark by a whole x-height once stacked by the app."""
    adv = 0 if unicodedata.category(ch) == "Mn" else int(round(font.getlength(ch)))
    s = int(font.size)
    ox, oy = s * 2, s * 3   # the pen on the baseline, with room on every side (marks reach left of the pen)
    img = Image.new("L", (s * 5, s * 4), 0)
    ImageDraw.Draw(img).text((ox, oy), ch, font=font, fill=255, anchor="ls")
    box = img.getbbox()
    if not box:
        return (0, 0, 0, 0, adv, b"")
    x0, y0, x1, y1 = box
    return (x0 - ox, ascent + y0 - oy, x1 - x0, y1 - y0, adv, img.crop(box).tobytes())


def glyph(cp, is_bold, size, ascent):
    ch = chr(cp)
    f, missing = font_for(script(cp), is_bold, size)
    g = raster(f, ch, ascent)
    if g[2:] in missing and unicodedata.category(ch) != "Zs":
        return None
    x, y, w, h, adv, _ = g
    assert -128 <= x <= 127 and -128 <= y <= 127 and w <= 255 and h <= 255 and adv <= 255, (hex(cp), size, g[:5])
    return g


# every character the app's sources write: the interface in all its languages
used = set()
for p in glob.glob(os.path.join(HERE, "..", "src", "*.[ch]")) + glob.glob(os.path.join(HERE, "..", "src", "*.inc")):
    if os.path.basename(p) != "font_data.c":
        used |= {ord(c) for c in unicodedata.normalize("NFC", open(p, encoding="utf-8").read()) if ord(c) >= 0xA0}

codepoints = set(range(0x20, 0x7F)) | set(range(0xA0, 0x180)) | set(range(0x400, 0x460))
codepoints |= {0x1A0, 0x1A1, 0x1AF, 0x1B0} | set(range(0x1EA0, 0x1EFA))      # Vietnamese (precomposed)
codepoints |= set(range(0x0E01, 0x0E3B)) | set(range(0x0E3F, 0x0E5C))       # Thai
codepoints |= {0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x20AC, 0x2039, 0x203A, 0x2248,
               0x2190, 0x2191, 0x2192, 0x2193, 0x2713, 0x2717, 0x25B6, 0x23F8, 0x00B7,
               0x25B3, 0x25CB, 0x25A1, 0x2715}   # △ ○ □ ✕: the face buttons named in sentences
codepoints |= set(range(0x3000, 0x3004)) | {0x300C, 0x300D, 0x300E, 0x300F} | set(range(0xFF01, 0xFF5F))
codepoints |= used
codepoints = sorted(codepoints)

digits = sorted(set(map(ord, "0123456789 %,.:/-?")))
FONTS = [("FONT_SMALL", False, 24, codepoints), ("FONT_BODY", False, 30, codepoints), ("FONT_HEAD", True, 34, codepoints),
         ("FONT_TITLE", True, 52, codepoints), ("FONT_HUGE", True, 112, digits)]

lines = ["/* Generated by app/tools/make_font.py from DejaVu Sans (Bitstream Vera / DejaVu license),",
         " * Noto Sans Thai and Noto Sans CJK SC (SIL Open Font License 1.1). */",
         '#include "font.h"', ""]
for name, is_bold, size, cps in FONTS:
    ascent, descent = font_for("latin", is_bold, size)[0].getmetrics()
    blob = bytearray()
    glyphs = []
    for cp in cps:
        g = glyph(cp, is_bold, size, ascent)
        if g is None:
            continue
        x, y, w, h, adv, data = g
        glyphs.append((cp, len(blob), w, h, x, y, adv))
        blob += data
    lines.append(f"static const uint8_t {name.lower()}_px[] = {{")
    for i in range(0, len(blob), 32):
        lines.append("  " + ",".join(str(b) for b in blob[i:i + 32]) + ",")
    lines.append("  0};")
    lines.append(f"static const font_glyph {name.lower()}_glyphs[] = {{")
    for cp, off, w, h, x, y, adv in glyphs:
        lines.append(f"  {{0x{cp:X}, {off}u, {w}, {h}, {x}, {y}, {adv}}},")
    lines.append("};")
    lines.append(f"const font_t {name} = {{{size}, {ascent}, {ascent + descent}, {len(glyphs)}, "
                 f"{name.lower()}_glyphs, {name.lower()}_px, 0}};")
    lines.append("")
    print(f"{name}: {size}px, {len(glyphs)} glyphs ({len(cps) - len(glyphs)} missing), {len(blob)} bytes")
open(out, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")

if extra_out:
    def decode_all(codec, rows, cells):
        found = set()
        for r in rows:
            for c in cells:
                try:
                    s = bytes([r, c]).decode(codec)
                except UnicodeDecodeError:
                    continue
                if len(s) == 1:
                    found.add(ord(s))
        return found

    ext = decode_all("gb2312", range(0xB0, 0xF8), range(0xA1, 0xFF))                                     # GB2312 hanzi
    ext |= decode_all("big5", range(0xA4, 0xC7), list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF)))     # Big5 level 1
    ext |= decode_all("euc_jp", range(0xB0, 0xD0), range(0xA1, 0xFF))                                   # JIS level-1 kanji
    ext |= decode_all("euc_kr", range(0xB0, 0xC9), range(0xA1, 0xFF))                                   # KS X 1001 Hangul
    ext |= set(range(0x3000, 0x3040)) | set(range(0x3041, 0x3097)) | set(range(0x309B, 0x3100))         # kana
    ext |= set(range(0xFF01, 0xFFA0))
    built_in = set(codepoints)
    ext = sorted(c for c in ext if script(c) == "cjk" and c not in built_in)
    payload = bytearray()
    for name, is_bold, size, _ in FONTS[:3]:
        ascent, descent = font_for("latin", is_bold, size)[0].getmetrics()
        table, px = bytearray(), bytearray()
        count = 0
        for cp in ext:
            g = glyph(cp, is_bold, size, ascent)
            if g is None:
                continue
            x, y, w, h, adv, data = g
            q = [(v * 15 + 127) // 255 for v in data]
            if len(q) % 2:
                q.append(0)
            table += struct.pack("<IIBBbbB3x", cp, len(px), w, h, x, y, adv)
            px += bytes((q[i] << 4) | q[i + 1] for i in range(0, len(q), 2))
            count += 1
        payload += struct.pack("<IIIII", size, ascent, ascent + descent, count, len(px)) + table + px
        print(f"extra {name}: {count} glyphs ({len(ext) - count} missing), {len(px)} bytes")
    z = zlib.compress(bytes(payload), 9)
    with open(extra_out, "wb") as f:
        f.write(b"E2AX" + struct.pack("<III", 1, 3, len(payload)) + z)
    print(f"{extra_out}: {len(payload)} bytes, {len(z)} compressed")
