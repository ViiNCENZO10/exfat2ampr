/* SPDX-License-Identifier: GPL-3.0-or-later
 * PNG -> image_t: game covers (icon0.png, with half-size levels for small draws) and wallpaper
 * sources (pic0.png key art cropped to the screen's 16:9). */
#include "img.h"

#include <stdlib.h>
#include <string.h>

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ASSERT(x) ((void)0)   /* the app runtime has no __assert */
#define STBI_NO_THREAD_LOCALS   /* the app runtime keeps TLS minimal */
#define STB_IMAGE_IMPLEMENTATION
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "third_party/stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define MAX_SOURCE_SIDE 8192   /* bigger PNGs are not decoded (memory) */

void *img_inflate(const uint8_t *z, size_t len, size_t expect, size_t *out) {
  *out = 0;
  if (len > 0x7fffffff || expect > 0x7fffffff) return NULL;
  int n = 0;
  char *p = stbi_zlib_decode_malloc_guesssize((const char *)z, (int)len, expect ? (int)expect : (int)len * 4, &n);
  if (p) *out = (size_t)n;
  return p;
}

/* box-filtered resize of the (sx, sy, sw, sh) part of an RGBA picture to tw x th */
static uint32_t *box_resize(const unsigned char *rgba, int w, int sx, int sy, int sw, int sh, int tw, int th) {
  uint32_t *px = malloc((size_t)tw * th * 4);
  if (!px) return NULL;
  for (int y = 0; y < th; y++) {
    int y0 = sy + (int)((int64_t)y * sh / th), y1 = sy + (int)((int64_t)(y + 1) * sh / th);
    if (y1 <= y0) y1 = y0 + 1;
    for (int x = 0; x < tw; x++) {
      int x0 = sx + (int)((int64_t)x * sw / tw), x1 = sx + (int)((int64_t)(x + 1) * sw / tw);
      if (x1 <= x0) x1 = x0 + 1;
      uint32_t acc[4] = {0, 0, 0, 0}, cnt = 0;
      for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) {
          const unsigned char *p = rgba + ((size_t)yy * w + xx) * 4;
          acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3];
          cnt++;
        }
      px[(size_t)y * tw + x] = RGBA(acc[0] / cnt, acc[1] / cnt, acc[2] / cnt, acc[3] / cnt);
    }
  }
  return px;
}

static unsigned char *decode(const uint8_t *data, size_t len, int *w, int *h) {
  int n;
  if (!stbi_info_from_memory(data, (int)len, w, h, &n) || *w <= 0 || *h <= 0 || *w > MAX_SOURCE_SIDE || *h > MAX_SOURCE_SIDE) return NULL;
  /* RGBA bytes read as a little-endian uint32 are 0xAABBGGRR: the frame's format */
  return stbi_load_from_memory(data, (int)len, w, h, &n, 4);
}

/* half-size levels down to 32 pixels, each one the 2x2 average of the previous */
static void build_levels(image_t *img) {
  image_t *cur = img;
  while (cur->w >= 64 && cur->h >= 64) {
    image_t *half = calloc(1, sizeof *half);
    if (!half) return;
    half->w = cur->w / 2;
    half->h = cur->h / 2;
    half->px = malloc((size_t)half->w * half->h * 4);
    if (!half->px) { free(half); return; }
    for (int y = 0; y < half->h; y++)
      for (int x = 0; x < half->w; x++) {
        const uint32_t *a = cur->px + (size_t)(2 * y) * cur->w + 2 * x, *b = a + cur->w;
        uint32_t px = 0;
        for (int sh = 0; sh < 32; sh += 8)
          px |= ((((a[0] >> sh) & 0xff) + ((a[1] >> sh) & 0xff) + ((b[0] >> sh) & 0xff) + ((b[1] >> sh) & 0xff) + 2) / 4) << sh;
        half->px[(size_t)y * half->w + x] = px;
      }
    cur->half = half;
    cur = half;
  }
}

int img_decode_png(const uint8_t *data, size_t len, int max_side, image_t *out) {
  memset(out, 0, sizeof *out);
  int w, h;
  unsigned char *rgba = decode(data, len, &w, &h);
  if (!rgba) return -1;
  int tw = w, th = h;
  if (w > max_side || h > max_side) {
    if (w >= h) { tw = max_side; th = (int)((int64_t)h * max_side / w); }
    else { th = max_side; tw = (int)((int64_t)w * max_side / h); }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
  }
  uint32_t *px = box_resize(rgba, w, 0, 0, w, h, tw, th);
  stbi_image_free(rgba);
  if (!px) return -1;
  out->w = tw; out->h = th; out->px = px;
  build_levels(out);
  return 0;
}

int img_decode_fill(const uint8_t *data, size_t len, int tw, int th, image_t *out) {
  memset(out, 0, sizeof *out);
  int w, h;
  unsigned char *rgba = decode(data, len, &w, &h);
  if (!rgba) return -1;
  /* the largest centred part with the target's proportions ("cover" fit) */
  int sw = w, sh = (int)((int64_t)w * th / tw);
  if (sh > h) { sh = h; sw = (int)((int64_t)h * tw / th); }
  if (sw < 1) sw = 1;
  if (sh < 1) sh = 1;
  uint32_t *px = box_resize(rgba, w, (w - sw) / 2, (h - sh) / 2, sw, sh, tw, th);
  stbi_image_free(rgba);
  if (!px) return -1;
  out->w = tw; out->h = th; out->px = px;
  return 0;
}

int img_crop(const image_t *src, int x, int y, int w, int h, image_t *out) {
  memset(out, 0, sizeof *out);
  if (!src->px || x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > src->w || y + h > src->h) return -1;
  out->px = malloc((size_t)w * h * 4);
  if (!out->px) return -1;
  for (int r = 0; r < h; r++) memcpy(out->px + (size_t)r * w, src->px + (size_t)(y + r) * src->w + x, (size_t)w * 4);
  out->w = w;
  out->h = h;
  build_levels(out);
  return 0;
}

void img_free(image_t *img) {
  image_t *level = img->half;
  while (level) {
    image_t *next = level->half;
    free(level->px);
    free(level);
    level = next;
  }
  free(img->px);
  memset(img, 0, sizeof *img);
}
