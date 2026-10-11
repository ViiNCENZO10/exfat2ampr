#!/usr/bin/env python3
"""Draw the home-screen icon (sce_sys/icon0.png, 512x512): a hard drive seen from the inside,
its platter shining like a CD, and a blue embossed lightning bolt driving the spindle, on white.

usage: make_icon.py OUT.png [SIZE]"""
import colorsys
import math
import sys

from PIL import Image, ImageDraw, ImageFilter

out = sys.argv[1]
final = int(sys.argv[2]) if len(sys.argv) > 2 else 512
S = 2048                      # supersampled canvas
k = S / 512.0                 # design units are 512-based


def u(v):
    return int(round(v * k))


img = Image.new("RGBA", (S, S), (255, 255, 255, 255))
d = ImageDraw.Draw(img)

# soft white background with a faint vignette
bg = Image.new("RGBA", (S, S), (0, 0, 0, 0))
bd = ImageDraw.Draw(bg)
for i in range(40):
    a = int(10 * (i / 40) ** 2)
    bd.rounded_rectangle([u(i * 0.6), u(i * 0.6), S - u(i * 0.6), S - u(i * 0.6)], radius=u(70), outline=(120, 140, 170, a), width=u(1))
img.alpha_composite(bg)

# drive casing (drop shadow, body, inner bay)
sh = Image.new("RGBA", (S, S), (0, 0, 0, 0))
ImageDraw.Draw(sh).rounded_rectangle([u(58), u(70), u(462), u(474)], radius=u(46), fill=(20, 30, 50, 110))
img.alpha_composite(sh.filter(ImageFilter.GaussianBlur(u(14))))
d.rounded_rectangle([u(50), u(56), u(462), u(462)], radius=u(44), fill=(44, 49, 58, 255))
d.rounded_rectangle([u(50), u(56), u(462), u(462)], radius=u(44), outline=(96, 104, 118, 255), width=u(4))
d.rounded_rectangle([u(66), u(72), u(446), u(446)], radius=u(34), fill=(30, 34, 41, 255))
for cx, cy in ((80, 86), (432, 86), (80, 432), (432, 432)):          # screws
    d.ellipse([u(cx - 9), u(cy - 9), u(cx + 9), u(cy + 9)], fill=(150, 156, 166, 255))
    d.line([u(cx - 5), u(cy), u(cx + 5), u(cy)], fill=(70, 74, 82, 255), width=u(3))

# platter: silver disc with an iridescent CD sheen and track rings
cx, cy, R = 236, 238, 158
disc = Image.new("RGBA", (S, S), (0, 0, 0, 0))
dd = ImageDraw.Draw(disc)
steps = 720
for i in range(steps):
    a0 = 360.0 * i / steps
    hue = (a0 / 360.0 * 2.0) % 1.0
    r, g, b = colorsys.hsv_to_rgb(hue, 0.22, 0.97)
    shine = 0.5 + 0.5 * math.cos(math.radians(a0 * 2 - 70))          # two bright sectors
    m = 0.80 + 0.20 * shine
    dd.pieslice([u(cx - R), u(cy - R), u(cx + R), u(cy + R)], a0, a0 + 360.0 / steps + 0.6,
                fill=(int(255 * r * m), int(255 * g * m), int(255 * b * m), 255))
for rr in range(70, R, 7):                                          # tracks
    dd.ellipse([u(cx - rr), u(cy - rr), u(cx + rr), u(cy + rr)], outline=(255, 255, 255, 34), width=u(1.2))
dd.ellipse([u(cx - R), u(cy - R), u(cx + R), u(cy + R)], outline=(205, 212, 224, 255), width=u(5))
dd.ellipse([u(cx - 68), u(cy - 68), u(cx + 68), u(cy + 68)], fill=(222, 228, 236, 255))   # clear inner ring
dd.ellipse([u(cx - 68), u(cy - 68), u(cx + 68), u(cy + 68)], outline=(170, 178, 190, 255), width=u(3))
img.alpha_composite(disc)

# spin trails
trail = Image.new("RGBA", (S, S), (0, 0, 0, 0))
td = ImageDraw.Draw(trail)
for rr, start, span, w in ((122, 200, 70, 6), (100, 20, 60, 5), (140, 300, 45, 4), (80, 120, 55, 4)):
    td.arc([u(cx - rr), u(cy - rr), u(cx + rr), u(cy + rr)], start, start + span, fill=(255, 255, 255, 150), width=u(w))
img.alpha_composite(trail.filter(ImageFilter.GaussianBlur(u(1.5))))

# actuator arm from the bottom-right pivot to the head over the platter
px, py = 408, 404
arm_sh = Image.new("RGBA", (S, S), (0, 0, 0, 0))
ImageDraw.Draw(arm_sh).line([u(px + 4), u(py + 6), u(314), u(264)], fill=(0, 0, 0, 120), width=u(20))
img.alpha_composite(arm_sh.filter(ImageFilter.GaussianBlur(u(5))))
d.line([u(px), u(py), u(310), u(258)], fill=(176, 182, 192, 255), width=u(18))
d.line([u(px), u(py), u(310), u(258)], fill=(214, 219, 227, 255), width=u(6))
d.polygon([(u(298), u(242)), (u(320), u(250)), (u(312), u(272)), (u(290), u(264))], fill=(120, 126, 136, 255))
d.ellipse([u(px - 30), u(py - 30), u(px + 30), u(py + 30)], fill=(120, 127, 139, 255))
d.ellipse([u(px - 18), u(py - 18), u(px + 18), u(py + 18)], fill=(188, 194, 204, 255))

# spindle hub with the embossed blue lightning bolt
d.ellipse([u(cx - 58), u(cy - 58), u(cx + 58), u(cy + 58)], fill=(248, 250, 253, 255))
d.ellipse([u(cx - 58), u(cy - 58), u(cx + 58), u(cy + 58)], outline=(150, 160, 175, 255), width=u(3))
bolt = [(10, -46), (-22, 6), (-2, 6), (-12, 46), (24, -10), (4, -10), (16, -46)]


def poly(dx, dy, scale=1.0):
    scale *= 1.15
    return [(u(cx + x * scale + dx), u(cy + y * scale + dy)) for x, y in bolt]


emb = Image.new("RGBA", (S, S), (0, 0, 0, 0))
ed = ImageDraw.Draw(emb)
ed.polygon(poly(3.5, 4.5), fill=(8, 40, 110, 200))                 # depth shadow
img.alpha_composite(emb.filter(ImageFilter.GaussianBlur(u(2.5))))
d.polygon(poly(0, 0), fill=(24, 120, 255, 255))
hi = Image.new("RGBA", (S, S), (0, 0, 0, 0))
ImageDraw.Draw(hi).polygon(poly(-1.6, -1.8, 0.92), fill=(120, 190, 255, 255))
mask = Image.new("L", (S, S), 0)
ImageDraw.Draw(mask).polygon(poly(0, 0), fill=255)
cut = Image.new("L", (S, S), 0)
ImageDraw.Draw(cut).polygon(poly(1.4, 1.6, 0.98), fill=255)
hi_mask = Image.composite(Image.new("L", (S, S), 0), mask, cut)    # rim visible on the upper-left edge
hi.putalpha(hi_mask.filter(ImageFilter.GaussianBlur(u(0.8))))
img.alpha_composite(hi)
d.polygon(poly(0, 0), outline=(10, 70, 190, 255), width=u(2))

img = img.resize((final, final), Image.LANCZOS).convert("RGB")
img.save(out, optimize=True)
print(f"{out}: {final}x{final}")
