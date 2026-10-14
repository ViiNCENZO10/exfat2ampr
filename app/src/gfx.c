/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE. */
#include "gfx.h"

#include "anim.h"
#include "img.h"

#include <stdlib.h>
#include <string.h>
#include "mem.h"   /* last: the allocations go through the app's own memory */

static uint32_t *g_fb;

/* translation, opacity and clip rectangle (absolute pixels, x1/y1 excluded) */
typedef struct { int ox, oy, op, x0, y0, x1, y1; } gstate;
#define STACK_MAX 24
static gstate g_s = {0, 0, 255, 0, 0, SCREEN_W, SCREEN_H};
static gstate g_stack[STACK_MAX];
static int g_depth;

int gfx_init(void) {
  g_fb = malloc((size_t)SCREEN_W * SCREEN_H * 4);
  return g_fb ? 0 : -1;
}
uint32_t *gfx_frame(void) { return g_fb; }
void gfx_present(void) { plat_present(g_fb); }

void gfx_push(int dx, int dy, int opacity) {
  if (g_depth < STACK_MAX) g_stack[g_depth] = g_s;
  g_depth++;
  g_s.ox += dx;
  g_s.oy += dy;
  if (opacity < 0) opacity = 0;
  if (opacity > 255) opacity = 255;
  g_s.op = g_s.op * opacity / 255;
}

void gfx_clip(int x, int y, int w, int h) {
  x += g_s.ox;
  y += g_s.oy;
  if (x > g_s.x0) g_s.x0 = x;
  if (y > g_s.y0) g_s.y0 = y;
  if (x + w < g_s.x1) g_s.x1 = x + w;
  if (y + h < g_s.y1) g_s.y1 = y + h;
}

void gfx_pop(void) {
  if (g_depth <= 0) return;
  g_depth--;
  if (g_depth < STACK_MAX) g_s = g_stack[g_depth];
}

static inline uint32_t blend(uint32_t d, uint32_t c, unsigned a) {   /* a = 0..255 coverage * alpha */
  if (a >= 255) return c | 0xff000000u;
  if (a == 0) return d;
  unsigned ia = 255 - a;
  uint32_t rb = ((((c & 0xff00ffu) * a) + ((d & 0xff00ffu) * ia)) >> 8) & 0xff00ffu;
  uint32_t g = ((((c & 0x00ff00u) * a) + ((d & 0x00ff00u) * ia)) >> 8) & 0x00ff00u;
  return 0xff000000u | rb | g;
}

/* a + (b - a) * t / 256 on all four channels */
static inline uint32_t mix_px(uint32_t a, uint32_t b, unsigned t) {
  unsigned it = 256 - t;
  uint32_t rb = (((a & 0xff00ffu) * it + (b & 0xff00ffu) * t) >> 8) & 0xff00ffu;
  uint32_t ag = (((a >> 8) & 0xff00ffu) * it + ((b >> 8) & 0xff00ffu) * t) & 0xff00ff00u;
  return rb | ag;
}

static inline void plot(int x, int y, uint32_t c, unsigned cov) {   /* absolute coordinates */
  if (x < g_s.x0 || y < g_s.y0 || x >= g_s.x1 || y >= g_s.y1) return;
  uint32_t *p = &g_fb[(size_t)y * SCREEN_W + (unsigned)x];
  *p = blend(*p, c, (cov * (c >> 24) * (unsigned)g_s.op + 32512u) / 65025u);
}

static void rect_abs(int x0, int y0, int x1, int y1, uint32_t c) {
  if (x0 < g_s.x0) x0 = g_s.x0;
  if (y0 < g_s.y0) y0 = g_s.y0;
  if (x1 > g_s.x1) x1 = g_s.x1;
  if (y1 > g_s.y1) y1 = g_s.y1;
  if (x0 >= x1 || y0 >= y1) return;
  unsigned a = ((c >> 24) * (unsigned)g_s.op + 127u) / 255u;
  if (!a) return;
  for (int yy = y0; yy < y1; yy++) {
    uint32_t *row = g_fb + (size_t)yy * SCREEN_W;
    if (a >= 255) {
      uint32_t v = c | 0xff000000u;
      for (int xx = x0; xx < x1; xx++) row[xx] = v;
    } else
      for (int xx = x0; xx < x1; xx++) row[xx] = blend(row[xx], c, a);
  }
}

/* nothing of the box (absolute) can be visible */
static bool outside(int x, int y, int w, int h) {
  return x >= g_s.x1 || y >= g_s.y1 || x + w <= g_s.x0 || y + h <= g_s.y0 || g_s.op <= 0;
}

static float fsqrt(float x) {
  if (x <= 0) return 0;
  union { float f; uint32_t u; } v = {x};
  v.u = 0x1fbd1df5u + (v.u >> 1);
  float y = v.f;
  y = 0.5f * (y + x / y);
  y = 0.5f * (y + x / y);
  return 0.5f * (y + x / y);
}

void gfx_clear(uint32_t c) {
  for (size_t i = 0; i < (size_t)SCREEN_W * SCREEN_H; i++) g_fb[i] = c;
}

void gfx_blit_full(const uint32_t *src) { memcpy(g_fb, src, (size_t)SCREEN_W * SCREEN_H * 4); }

void gfx_blend_full(const uint32_t *a, const uint32_t *b, unsigned t) {
  if (t >= 256) { gfx_blit_full(b); return; }
  for (size_t i = 0; i < (size_t)SCREEN_W * SCREEN_H; i++) g_fb[i] = mix_px(a[i], b[i], t) | 0xff000000u;
}

void gfx_rect(int x, int y, int w, int h, uint32_t c) {
  x += g_s.ox;
  y += g_s.oy;
  rect_abs(x, y, x + w, y + h, c);
}

static uint32_t lerp_col(uint32_t a, uint32_t b, int t, int n) {
  uint32_t out = 0;
  for (int sh = 0; sh < 32; sh += 8) {
    int ca = (a >> sh) & 0xff, cb = (b >> sh) & 0xff;
    out |= (uint32_t)(ca + (cb - ca) * t / (n ? n : 1)) << sh;
  }
  return out;
}

void gfx_gradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
  for (int i = 0; i < h; i++) gfx_rect(x, y + i, w, 1, lerp_col(top, bottom, i, h - 1));
}

/* coverage (0..255) of pixel (px, py) by the disc of radius r centred on (cx, cy) */
static unsigned corner_cov(int px, int py, int cx, int cy, int r) {
  int fx = 2 * (px - cx) + 1, fy = 2 * (py - cy) + 1;   /* pixel centre, doubled */
  int d2 = fx * fx + fy * fy, rin = 2 * r - 2, rout = 2 * r + 2;
  if (rin > 0 && d2 <= rin * rin) return 255;
  if (d2 >= rout * rout) return 0;
  unsigned hits = 0;   /* edge pixel: 4x4 supersampling */
  for (int sy = 0; sy < 4; sy++)
    for (int sx = 0; sx < 4; sx++) {
      int dx2 = (px - cx) * 8 + sx * 2 + 1, dy2 = (py - cy) * 8 + sy * 2 + 1;   /* 1/8 pixel units */
      if (dx2 * dx2 + dy2 * dy2 <= r * r * 64) hits++;
    }
  return hits * 255 / 16;
}

void gfx_round_rect(int x, int y, int w, int h, int r, uint32_t c) {
  x += g_s.ox;
  y += g_s.oy;
  if (w <= 0 || h <= 0 || outside(x, y, w, h)) return;
  if (r * 2 > w) r = w / 2;
  if (r * 2 > h) r = h / 2;
  if (r <= 0) { rect_abs(x, y, x + w, y + h, c); return; }
  rect_abs(x, y + r, x + w, y + h - r, c);
  rect_abs(x + r, y, x + w - r, y + r, c);
  rect_abs(x + r, y + h - r, x + w - r, y + h, c);
  for (int yy = 0; yy < r; yy++)
    for (int xx = 0; xx < r; xx++) {
      unsigned cov = corner_cov(xx, yy, r, r, r);
      if (!cov) continue;
      plot(x + xx, y + yy, c, cov);
      plot(x + w - 1 - xx, y + yy, c, cov);
      plot(x + xx, y + h - 1 - yy, c, cov);
      plot(x + w - 1 - xx, y + h - 1 - yy, c, cov);
    }
}

void gfx_round_gradient(int x, int y, int w, int h, int r, uint32_t top, uint32_t bottom) {
  if (w <= 0 || h <= 0 || outside(x + g_s.ox, y + g_s.oy, w, h)) return;
  if (r * 2 > w) r = w / 2;
  if (r * 2 > h) r = h / 2;
  for (int yy = 0; yy < h; yy++) {
    uint32_t c = mix_px(top, bottom, (unsigned)(yy * 256 / h));
    int band = yy < r ? yy : yy >= h - r ? h - 1 - yy : -1;   /* rows with rounded ends */
    if (band < 0) { gfx_rect(x, y + yy, w, 1, c); continue; }
    gfx_rect(x + r, y + yy, w - 2 * r, 1, c);
    for (int xx = 0; xx < r; xx++) {
      unsigned cov = corner_cov(xx, band, r, r, r);
      if (!cov) continue;
      plot(x + g_s.ox + xx, y + g_s.oy + yy, c, cov);
      plot(x + g_s.ox + w - 1 - xx, y + g_s.oy + yy, c, cov);
    }
  }
}

void gfx_round_frame(int x, int y, int w, int h, int r, int t, uint32_t c) {
  x += g_s.ox;
  y += g_s.oy;
  if (w <= 0 || h <= 0 || outside(x, y, w, h)) return;
  /* outline as a ring: outer minus inner, per pixel in the corners */
  if (r * 2 > w) r = w / 2;
  if (r * 2 > h) r = h / 2;
  rect_abs(x + r, y, x + w - r, y + t, c);
  rect_abs(x + r, y + h - t, x + w - r, y + h, c);
  rect_abs(x, y + r, x + t, y + h - r, c);
  rect_abs(x + w - t, y + r, x + w, y + h - r, c);
  int ri = r - t;
  for (int yy = 0; yy < r; yy++)
    for (int xx = 0; xx < r; xx++) {
      int outer = (int)corner_cov(xx, yy, r, r, r);
      if (!outer) continue;
      int inner = ri > 0 && xx >= t && yy >= t ? (int)corner_cov(xx - t, yy - t, ri, ri, ri) : 0;
      int cov = outer - inner;
      if (cov <= 0) continue;
      plot(x + xx, y + yy, c, (unsigned)cov);
      plot(x + w - 1 - xx, y + yy, c, (unsigned)cov);
      plot(x + xx, y + h - 1 - yy, c, (unsigned)cov);
      plot(x + w - 1 - xx, y + h - 1 - yy, c, (unsigned)cov);
    }
}

void gfx_glow(int x, int y, int w, int h, int r, uint32_t c, int size) {
  unsigned a0 = c >> 24;
  for (int i = 1; i <= size; i++) {
    unsigned f = (unsigned)(size + 1 - i);   /* quadratic fall-off */
    unsigned a = a0 * f * f / (unsigned)((size + 1) * (size + 1));
    if (a) gfx_round_frame(x - i, y - i, w + 2 * i, h + 2 * i, r + i, 1, WITH_ALPHA(c, a));
  }
}

void gfx_circle(int cx, int cy, int r, uint32_t c) {
  cx += g_s.ox;
  cy += g_s.oy;
  if (r <= 0 || outside(cx - r, cy - r, 2 * r, 2 * r)) return;
  for (int yy = -r; yy < r; yy++)
    for (int xx = -r; xx < r; xx++) {
      unsigned cov = corner_cov(xx + r, yy + r, r, r, r);
      if (cov) plot(cx + xx, cy + yy, c, cov);
    }
}

void gfx_line(int x0, int y0, int x1, int y1, int t, uint32_t c) {
  x0 += g_s.ox; y0 += g_s.oy;
  x1 += g_s.ox; y1 += g_s.oy;
  float hr = (float)t / 2.0f;
  int m = t / 2 + 2;
  int bx0 = (x0 < x1 ? x0 : x1) - m, bx1 = (x0 > x1 ? x0 : x1) + m;
  int by0 = (y0 < y1 ? y0 : y1) - m, by1 = (y0 > y1 ? y0 : y1) + m;
  if (outside(bx0, by0, bx1 - bx0, by1 - by0)) return;
  float dx = (float)(x1 - x0), dy = (float)(y1 - y0), len2 = dx * dx + dy * dy;
  for (int py = by0; py <= by1; py++)
    for (int px = bx0; px <= bx1; px++) {
      /* distance from the pixel centre to the segment */
      float qx = (float)px + 0.5f - (float)x0, qy = (float)py + 0.5f - (float)y0;
      float u = len2 > 0 ? (qx * dx + qy * dy) / len2 : 0;
      u = clampf(u, 0.0f, 1.0f);
      float ex = qx - u * dx, ey = qy - u * dy;
      float cov = hr + 0.5f - fsqrt(ex * ex + ey * ey);
      if (cov <= 0) continue;
      plot(px, py, c, cov >= 1 ? 255u : (unsigned)(cov * 255));
    }
}

void gfx_image_a(const image_t *img, int x, int y, int w, int h, int radius, int alpha) {
  if (!img || !img->px || img->w <= 0 || img->h <= 0 || w <= 0 || h <= 0 || alpha <= 0) return;
  /* the smallest level still at least as big as the target: bilinear never skips texels */
  while (img->half && img->half->px && img->half->w >= w && img->half->h >= h) img = img->half;
  x += g_s.ox;
  y += g_s.oy;
  if (outside(x, y, w, h)) return;
  int xs = g_s.x0 > x ? g_s.x0 - x : 0, xe = g_s.x1 < x + w ? g_s.x1 - x : w;
  int ys = g_s.y0 > y ? g_s.y0 - y : 0, ye = g_s.y1 < y + h ? g_s.y1 - y : h;
  unsigned op = (unsigned)(alpha > 255 ? 255 : alpha) * (unsigned)g_s.op / 255u;
  int64_t stx = ((int64_t)img->w << 16) / w, sty = ((int64_t)img->h << 16) / h;
  int64_t maxx = (int64_t)(img->w - 1) << 16, maxy = (int64_t)(img->h - 1) << 16;
  for (int yy = ys; yy < ye; yy++) {
    int64_t sy = yy * sty + sty / 2 - 32768;
    if (sy < 0) sy = 0;
    if (sy > maxy) sy = maxy;
    int y0 = (int)(sy >> 16), y1 = y0 + 1 < img->h ? y0 + 1 : y0;
    unsigned fy = (unsigned)((sy >> 8) & 0xff);
    const uint32_t *r0 = img->px + (size_t)y0 * img->w, *r1 = img->px + (size_t)y1 * img->w;
    uint32_t *dst = g_fb + (size_t)(y + yy) * SCREEN_W;   /* x + xx is inside the clip, so >= 0 */
    int cy = yy < radius ? radius : yy >= h - radius ? h - radius : -1;
    for (int xx = xs; xx < xe; xx++) {
      int64_t sx = xx * stx + stx / 2 - 32768;
      if (sx < 0) sx = 0;
      if (sx > maxx) sx = maxx;
      int x0 = (int)(sx >> 16), x1 = x0 + 1 < img->w ? x0 + 1 : x0;
      unsigned fx = (unsigned)((sx >> 8) & 0xff);
      uint32_t c = mix_px(mix_px(r0[x0], r0[x1], fx), mix_px(r1[x0], r1[x1], fx), fy);
      unsigned cov = 255;
      if (radius > 0 && cy >= 0) {
        int cx = xx < radius ? radius : xx >= w - radius ? w - radius : -1;
        if (cx >= 0) {
          int px = xx < radius ? xx : radius - 1 - (xx - (w - radius));
          int py = yy < radius ? yy : radius - 1 - (yy - (h - radius));
          cov = corner_cov(px, py, radius, radius, radius);
          if (!cov) continue;
        }
      }
      dst[x + xx] = blend(dst[x + xx], c, (cov * (c >> 24) * op + 32512u) / 65025u);
    }
  }
}

void gfx_image(const image_t *img, int x, int y, int w, int h, int radius) { gfx_image_a(img, x, y, w, h, radius, 255); }

void gfx_spinner(int cx, int cy, int r, int dot, float phase, uint32_t c) {
  int head = (int)(phase * 12.0f) % 12;
  if (head < 0) head += 12;
  for (int i = 0; i < 12; i++) {
    double ang = i * M_TAU / 12 - M_TAU / 4;
    int px = cx + (int)(m_cos(ang) * r), py = cy + (int)(m_sin(ang) * r);
    unsigned age = (unsigned)((head - i + 12) % 12), k = 12 - age;
    unsigned a = (c >> 24) * k * k / 144;
    gfx_circle(px, py, dot, WITH_ALPHA(c, a < 26 ? 26 : a));
  }
}

/* ------------------------------------------------------------------ text -- */
static uint32_t utf8_next(const char **ps) {
  const unsigned char *s = (const unsigned char *)*ps;
  uint32_t cp = s[0];
  int n = 1;
  if (cp >= 0xf0 && s[1] && s[2] && s[3]) { cp = ((cp & 7u) << 18) | ((s[1] & 63u) << 12) | ((s[2] & 63u) << 6) | (s[3] & 63u); n = 4; }
  else if (cp >= 0xe0 && s[1] && s[2]) { cp = ((cp & 15u) << 12) | ((s[1] & 63u) << 6) | (s[2] & 63u); n = 3; }
  else if (cp >= 0xc0 && s[1]) { cp = ((cp & 31u) << 6) | (s[1] & 63u); n = 2; }
  else if (cp >= 0x80) cp = '?';
  *ps += n;
  return cp;
}

/* Glyph tables loaded at start by gfx_load_extra (the CJK characters of game titles), one per size,
 * 4-bit coverage. Published once complete (g_nextra), never freed. */
#define EXTRA_MAX 4
static font_t g_extra[EXTRA_MAX];
static int g_nextra, g_extra_seen;

static const font_glyph *find_glyph(const font_t *f, uint32_t cp) {
  int lo = 0, hi = f->count - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (f->glyphs[mid].cp == cp) return &f->glyphs[mid];
    if (f->glyphs[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
  }
  return NULL;
}

/* a glyph as drawn in font f: its own, else one of the extra tables' (enlarged when the table is smaller) */
typedef struct {
  const font_glyph *g;
  const font_t *src;      /* the table holding its bitmap */
  int x, y, w, h, adv;    /* in f's pixels, y from the top of f's line */
} glyph_ref;

static bool lookup(const font_t *f, uint32_t cp, glyph_ref *r) {
  const font_t *src = f;
  const font_glyph *g = find_glyph(f, cp);
  if (!g && cp >= 0x1100) {
    int n = __atomic_load_n(&g_nextra, __ATOMIC_ACQUIRE);
    const font_t *best = NULL;   /* the same size, else the largest smaller one */
    for (int i = 0; i < n; i++)
      if (g_extra[i].size <= f->size && (!best || g_extra[i].size > best->size)) best = &g_extra[i];
    if (best && (g = find_glyph(best, cp)) != NULL) src = best;
  }
  if (!g && (cp == '?' || !(g = find_glyph(f, '?')))) return false;
  r->g = g;
  r->src = src;
  int num = f->size, den = src->size;
  if (num == den) {
    r->x = g->x;
    r->y = g->y + f->ascent - src->ascent;
    r->w = g->w;
    r->h = g->h;
    r->adv = g->adv;
  } else {
    r->x = g->x * num / den;
    r->y = f->ascent + (g->y - src->ascent) * num / den;
    r->w = (g->w * num + den - 1) / den;
    r->h = (g->h * num + den - 1) / den;
    r->adv = (g->adv * num + den / 2) / den;
  }
  return true;
}

static inline unsigned coverage(const glyph_ref *r, int x, int y) {   /* 0..255, 0 outside the bitmap */
  const font_glyph *g = r->g;
  if (x < 0 || y < 0 || x >= g->w || y >= g->h) return 0;
  const uint8_t *px = r->src->px + g->off;
  int i = y * g->w + x;
  return r->src->nibbles ? ((px[i >> 1] >> ((i & 1) ? 0 : 4)) & 15u) * 17u : px[i];
}

static void draw_glyph(const glyph_ref *r, int x, int y, uint32_t c) {   /* absolute */
  const font_glyph *g = r->g;
  int gx = x + r->x, gy = y + r->y;
  if (!r->w || !r->h || outside(gx, gy, r->w, r->h)) return;
  if (r->w == g->w && r->h == g->h) {
    for (int yy = 0; yy < g->h; yy++)
      for (int xx = 0; xx < g->w; xx++) {
        unsigned cov = coverage(r, xx, yy);
        if (cov) plot(gx + xx, gy + yy, c, cov);
      }
    return;
  }
  /* a smaller table's glyph, enlarged (bilinear); positions in 1/256 pixel, kept positive for the shifts */
  for (int yy = 0; yy < r->h; yy++) {
    int sy = (2 * yy + 1) * g->h * 128 / r->h + 128, y0 = (sy >> 8) - 1, fy = sy & 255;
    for (int xx = 0; xx < r->w; xx++) {
      int sx = (2 * xx + 1) * g->w * 128 / r->w + 128, x0 = (sx >> 8) - 1, fx = sx & 255;
      unsigned top = coverage(r, x0, y0) * (256u - fx) + coverage(r, x0 + 1, y0) * (unsigned)fx;
      unsigned bot = coverage(r, x0, y0 + 1) * (256u - fx) + coverage(r, x0 + 1, y0 + 1) * (unsigned)fx;
      unsigned cov = (top * (256u - fy) + bot * (unsigned)fy) >> 16;
      if (cov) plot(gx + xx, gy + yy, c, cov);
    }
  }
}

/* Thai vowel and tone marks have no width. Without OpenType positioning they are centred on their
 * consonant (moved left of the tall ones: ป ฝ ฟ ฬ) and the ones above are stacked upwards. */
static bool thai_above(uint32_t c) { return c == 0x0E31 || (c >= 0x0E34 && c <= 0x0E37) || (c >= 0x0E47 && c <= 0x0E4E); }
static bool thai_below(uint32_t c) { return c >= 0x0E38 && c <= 0x0E3A; }
static bool thai_mark(uint32_t c) { return thai_above(c) || thai_below(c); }

typedef struct { bool set, tall; int left, w, stack; } mark_base;   /* the last consonant drawn */

static void set_base(const font_t *f, uint32_t cp, const glyph_ref *r, int pen, mark_base *b) {
  b->set = true;
  b->tall = cp == 0x0E1B || cp == 0x0E1D || cp == 0x0E1F || cp == 0x0E2C;
  b->left = pen + r->x;
  b->w = r->w;
  b->stack = r->y;
  glyph_ref n;   /* marks beside a tall consonant's stem sit at the height of the others' (น) */
  if (b->tall && lookup(f, 0x0E19, &n)) b->stack = n.y;
}

static void place_mark(const font_t *f, uint32_t cp, const glyph_ref *r, int pen, mark_base *b, int *dx, int *dy) {
  *dx = *dy = 0;
  if (!b->set || r->adv || !thai_mark(cp)) return;
  *dx = b->left + (b->w - r->w) / 2 - (pen + r->x) - (b->tall ? f->size / 7 : 0);
  if (thai_above(cp)) {
    *dy = b->stack - f->size / 14 - (r->y + r->h);
    b->stack = r->y + *dy;
  }
}

int gfx_text_width(const font_t *f, const char *s) {
  int w = 0;
  glyph_ref r;
  while (s && *s)
    if (lookup(f, utf8_next(&s), &r)) w += r.adv;
  return w;
}

int gfx_text(const font_t *f, int x, int y, const char *s, uint32_t c, int max_w) {
  if (!s) return 0;
  x += g_s.ox;
  y += g_s.oy;
  int start = x;
  glyph_ref ell;
  bool has_ell = lookup(f, 0x2026, &ell);
  bool clip = max_w > 0 && gfx_text_width(f, s) > max_w;
  int ell_w = clip && has_ell ? ell.adv : 0;
  mark_base base;
  memset(&base, 0, sizeof base);
  while (*s) {
    uint32_t cp = utf8_next(&s);
    glyph_ref r;
    if (!lookup(f, cp, &r)) continue;
    if (clip && r.adv && x - start + r.adv > max_w - ell_w) {
      if (has_ell) { draw_glyph(&ell, x, y, c); x += ell.adv; }
      break;
    }
    int dx, dy;
    place_mark(f, cp, &r, x, &base, &dx, &dy);
    draw_glyph(&r, x + dx, y + dy, c);
    if (!thai_mark(cp)) set_base(f, cp, &r, x, &base);
    x += r.adv;
  }
  return x - start;
}

static bool cjk_cp(uint32_t c) {
  return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) ||
         (c >= 0xFF00 && c <= 0xFFEF);
}
static bool thai_cp(uint32_t c) { return c >= 0x0E01 && c <= 0x0E5B; }

/* never first on a line: closing punctuation, Thai marks and the vowels written after their consonant */
static bool no_line_start(uint32_t c) {
  static const uint32_t closing[] = {',', '.', ';', ':', '!', '?', ')', ']', '%', 0xBB, 0x2019, 0x201D, 0x2026, 0x203A,
                                   0x3001, 0x3002, 0x300D, 0x300F, 0xFF01, 0xFF09, 0xFF0C, 0xFF0E, 0xFF1A, 0xFF1B, 0xFF1F};
  for (size_t i = 0; i < sizeof closing / sizeof closing[0]; i++)
    if (c == closing[i]) return true;
  return (c >= 0x0E30 && c <= 0x0E3A) || (c >= 0x0E45 && c <= 0x0E4E);
}

/* never last on a line: opening punctuation, the Thai vowels written before their consonant */
static bool no_line_end(uint32_t c) {
  static const uint32_t opening[] = {'(', '[', 0xAB, 0x2018, 0x201C, 0x2039, 0x300C, 0x300E, 0xFF08};
  for (size_t i = 0; i < sizeof opening / sizeof opening[0]; i++)
    if (c == opening[i]) return true;
  return c >= 0x0E40 && c <= 0x0E44;
}

int gfx_text_wrap(const font_t *f, int x, int y, const char *s, uint32_t c, int max_w, int max_lines) {
  char line[512];
  int lines = 0, lh = f->line + f->line / 5;
  while (s && *s && lines < max_lines) {
    /* the longest start of s that fits. Cuts: at a space (not before "?", ":"...) or next to a CJK character;
     * inside a Thai phrase (Thai writes no space between words) when the line would stay short otherwise;
     * anywhere as a last resort (a path, a long word) */
    const char *p = s, *word = NULL, *thai = NULL, *any = NULL;
    int w = 0, word_w = 0;
    uint32_t prev = 0;
    bool all = true;
    while (*p) {
      const char *q = p;
      uint32_t cp = utf8_next(&q);
      if (p > s) {
        if (cp == ' ') {
          const char *n = q;
          while (*n == ' ') n++;
          if (!*n || !no_line_start(utf8_next(&n))) { word = p; word_w = w; }
        } else if (prev != ' ' && !no_line_start(cp) && !no_line_end(prev)) {
          if (cjk_cp(cp) || cjk_cp(prev)) { word = p; word_w = w; }
          else if (thai_cp(cp) && thai_cp(prev)) thai = p;
          else any = p;
        }
      }
      glyph_ref r;
      int adv = lookup(f, cp, &r) ? r.adv : 0;
      if ((w + adv > max_w && p > s) || q - s >= (long)sizeof line) { all = false; break; }
      w += adv;
      prev = cp;
      p = q;
    }
    const char *cut = p;
    if (!all) {
      if (thai && (!word || (word_w < max_w * 3 / 5 && thai > word))) cut = thai;
      else if (word) cut = word;
      else if (any) cut = any;
    }
    if (lines == max_lines - 1 && *cut) {
      gfx_text(f, x, y + lines * lh, s, c, max_w);   /* the last line: the rest, cut with an ellipsis */
    } else {
      size_t len = (size_t)(cut - s);
      memcpy(line, s, len);
      line[len] = 0;
      gfx_text(f, x, y + lines * lh, line, c, max_w + 1);
    }
    lines++;
    s = cut;
    while (*s == ' ') s++;
  }
  return lines * lh;
}

/* E2AX file: "E2AX", version 1, table count, payload size (little-endian u32), then the payload,
 * zlib-compressed. Per table: size, ascent, line height, glyph count, bitmap bytes (u32); the glyphs
 * (code point u32, bitmap offset u32, w, h, x, y, advance, 3 spare bytes) sorted by code point; their
 * 4-bit bitmaps (two pixels a byte, the first in the high half). */
static uint32_t rd32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int gfx_load_extra(const char *path) {
  if (__atomic_load_n(&g_nextra, __ATOMIC_ACQUIRE)) return 0;
  uint8_t *file = NULL, *data = NULL;
  size_t len = 0, size = 0, pos = 0;
  if (plat_read_file(path, &file, &len) != 0) return -1;
  int n = 0;
  if (len > 16 && !memcmp(file, "E2AX", 4) && rd32(file + 4) == 1 && rd32(file + 8) <= EXTRA_MAX) {
    data = img_inflate(file + 16, len - 16, rd32(file + 12), &size);
    if (data && size == rd32(file + 12)) n = (int)rd32(file + 8);
  }
  free(file);
  font_t t[EXTRA_MAX];
  memset(t, 0, sizeof t);
  int done = 0;
  for (; done < n; done++) {
    if (size - pos < 20) break;
    const uint8_t *h = data + pos;
    uint32_t count = rd32(h + 12), bytes = rd32(h + 16);
    pos += 20;
    if (!count || count > 65536 || (size - pos) / 16 < count || size - pos - (size_t)count * 16 < bytes) break;
    font_glyph *g = malloc(sizeof *g * count);
    if (!g) break;
    bool ok = true;
    for (uint32_t i = 0; i < count && ok; i++) {
      const uint8_t *e = data + pos + (size_t)i * 16;
      g[i] = (font_glyph){rd32(e), rd32(e + 4), e[8], e[9], (int8_t)e[10], (int8_t)e[11], e[12]};
      ok = (i == 0 || g[i].cp > g[i - 1].cp) && (size_t)g[i].off + ((size_t)g[i].w * g[i].h + 1) / 2 <= bytes;
    }
    if (!ok) { free(g); break; }
    pos += (size_t)count * 16;
    t[done] = (font_t){(int)rd32(h), (int)rd32(h + 4), (int)rd32(h + 8), (int)count, g, data + pos, 1};
    pos += bytes;
    if (t[done].size <= 0 || t[done].size > 255) { free(g); break; }
  }
  if (!n || done < n) {
    for (int i = 0; i < done; i++) free((void *)t[i].glyphs);
    free(data);
    return -1;
  }
  for (int i = 0; i < n; i++) g_extra[i] = t[i];
  __atomic_store_n(&g_nextra, n, __ATOMIC_RELEASE);
  return 0;
}

bool gfx_extra_arrived(void) {
  int n = __atomic_load_n(&g_nextra, __ATOMIC_ACQUIRE);
  if (n == g_extra_seen) return false;
  g_extra_seen = n;
  return true;
}

int gfx_text_right(const font_t *f, int right, int y, const char *s, uint32_t c) {
  int w = gfx_text_width(f, s);
  gfx_text(f, right - w, y, s, c, 0);
  return w;
}

int gfx_text_center(const font_t *f, int cx, int y, const char *s, uint32_t c) {
  int w = gfx_text_width(f, s);
  gfx_text(f, cx - w / 2, y, s, c, 0);
  return w;
}

/* --------------------------------------------------------------- buttons -- */
int gfx_button(int icon, int x, int y, int size) {
  const uint32_t ring = RGB(235, 238, 245), bg = RGB(28, 32, 42);
  int r = size / 2, cx = x + r, cy = y + r, t = size / 10 > 2 ? size / 10 : 2, m = size * 3 / 10;
  if (icon == ICON_OPTIONS || icon == ICON_L1 || icon == ICON_R1) {
    const char *label = icon == ICON_OPTIONS ? "OPTIONS" : icon == ICON_L1 ? "L1" : "R1";
    int w = gfx_text_width(&FONT_SMALL, label) + size / 2;
    gfx_round_rect(x, y + size / 8, w, size * 3 / 4, size * 3 / 8, ring);
    gfx_text_center(&FONT_SMALL, x + w / 2, y + size / 2 - FONT_SMALL.line / 2, label, bg);
    return w;
  }
  gfx_circle(cx, cy, r, ring);
  gfx_circle(cx, cy, r - 2, bg);
  switch (icon) {
    case ICON_CROSS:
      gfx_line(cx - m, cy - m, cx + m, cy + m, t, RGB(124, 178, 255));
      gfx_line(cx - m, cy + m, cx + m, cy - m, t, RGB(124, 178, 255));
      break;
    case ICON_CIRCLE:
      gfx_circle(cx, cy, m + t / 2, RGB(255, 102, 102));
      gfx_circle(cx, cy, m - t / 2, bg);
      break;
    case ICON_SQUARE:
      gfx_rect(cx - m, cy - m, 2 * m, t, RGB(255, 135, 205));
      gfx_rect(cx - m, cy + m - t, 2 * m, t, RGB(255, 135, 205));
      gfx_rect(cx - m, cy - m, t, 2 * m, RGB(255, 135, 205));
      gfx_rect(cx + m - t, cy - m, t, 2 * m, RGB(255, 135, 205));
      break;
    case ICON_TRIANGLE:
      gfx_line(cx, cy - m, cx - m, cy + m * 3 / 4, t, RGB(80, 220, 180));
      gfx_line(cx, cy - m, cx + m, cy + m * 3 / 4, t, RGB(80, 220, 180));
      gfx_line(cx - m, cy + m * 3 / 4, cx + m, cy + m * 3 / 4, t, RGB(80, 220, 180));
      break;
    case ICON_DPAD:
      gfx_rect(cx - t, cy - m - t, 2 * t, 2 * m + 2 * t, ring);
      gfx_rect(cx - m - t, cy - t, 2 * m + 2 * t, 2 * t, ring);
      break;
    default: break;
  }
  return size;
}
