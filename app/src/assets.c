/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "assets.h"

#include "anim.h"
#include "api.h"
#include "img.h"
#include "platform.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEY_MAX 620
#define QUERY_MAX 1400
#define COVER_SLOTS 48             /* the list's covers and the details' */
#define COVER_SIDE 384           /* icon0.png is 512: sharp at every size the screens use */
#define COVER_RETRY_MS 6000
#define JOBS 32
#define SOURCES 10
#define FADE_MS 450
#define ART_W 960                /* key art kept at half the screen size, then scaled up */
#define ART_H 540
#define BLUR_W 240               /* covers become a blurred colour field */
#define BLUR_H 135

static void copy_str(char *dst, size_t cap, const char *src) {
  size_t i = 0;
  for (; src && src[i] && i + 1 < cap; i++) dst[i] = src[i];
  dst[i] = 0;
}

static inline uint32_t mix_px(uint32_t a, uint32_t b, unsigned t) {   /* a + (b - a) * t / 256 */
  unsigned it = 256 - t;
  uint32_t rb = (((a & 0xff00ffu) * it + (b & 0xff00ffu) * t) >> 8) & 0xff00ffu;
  uint32_t ag = (((a >> 8) & 0xff00ffu) * it + ((b >> 8) & 0xff00ffu) * t) & 0xff00ff00u;
  return rb | ag;
}

static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static bool g_thread;

/* ----------------------------------------------------------------- covers -- */
/* the cache belongs to the main thread; the loader only hands finished pictures over */
typedef struct { char key[KEY_MAX]; image_t img; bool loaded; uint64_t used, loaded_at, retry_at; } cover_slot;
static cover_slot g_slots[COVER_SLOTS];
static uint64_t g_tick;

typedef struct { char key[KEY_MAX], query[QUERY_MAX]; } cover_job;
typedef struct { char key[KEY_MAX]; image_t img; bool ok; } cover_done;
static cover_job g_jobs[JOBS];
static cover_done g_done[JOBS];
static int g_njobs, g_ndone;

static bool load_cover(const char *query, image_t *img) {
  char path[QUERY_MAX + 16];
  snprintf(path, sizeof path, "/api/icon?%s", query);
  uint8_t *png;
  size_t len;
  if (api_get_raw(path, &png, &len) != 0) return false;
  int rc = img_decode_png(png, len, COVER_SIDE, img);
  free(png);
  return rc == 0;
}

static void queue_cover(const char *key, const char *query) {
  pthread_mutex_lock(&g_mx);
  bool queued = false;
  for (int i = 0; i < g_njobs && !queued; i++) queued = !strcmp(g_jobs[i].key, key);
  if (!queued && g_njobs < JOBS) {
    copy_str(g_jobs[g_njobs].key, KEY_MAX, key);
    copy_str(g_jobs[g_njobs].query, QUERY_MAX, query);
    g_njobs++;
    pthread_cond_signal(&g_cv);
  }
  pthread_mutex_unlock(&g_mx);
}

const image_t *assets_cover(const char *key, const char *query, uint64_t *loaded_at) {
  if (loaded_at) *loaded_at = 0;
  if (!key || !key[0]) return NULL;
  cover_slot *slot = NULL, *lru = &g_slots[0];
  for (int i = 0; i < COVER_SLOTS; i++) {
    if (!strcmp(g_slots[i].key, key)) { slot = &g_slots[i]; break; }
    if (g_slots[i].used < lru->used) lru = &g_slots[i];
  }
  uint64_t now = plat_ms();
  if (!slot) {
    slot = lru;
    img_free(&slot->img);
    copy_str(slot->key, sizeof slot->key, key);
    slot->loaded = false;
    slot->retry_at = 0;
    if (g_thread) queue_cover(key, query);
    else {   /* no loader thread: load it here */
      slot->loaded = true;
      slot->loaded_at = now;
      if (!load_cover(query, &slot->img)) slot->retry_at = now + COVER_RETRY_MS;
    }
  } else if (slot->loaded && !slot->img.px && slot->retry_at && now >= slot->retry_at) {
    slot->loaded = false;   /* the engine may have been busy: ask again */
    slot->retry_at = 0;
    if (g_thread) queue_cover(key, query);
  }
  slot->used = ++g_tick;
  if (loaded_at) *loaded_at = slot->loaded_at;
  return slot->img.px ? &slot->img : NULL;
}

/* -------------------------------------------------------------- wallpaper -- */
static uint32_t *g_wp[3];
static int g_cur = 0, g_prev = 1, g_work = 2;
static bool g_wp_ok, g_fading;
static uint64_t g_fade_at;
static char g_wanted[KEY_MAX];   /* "" = the default wallpaper, composed at start */

/* request for the loader */
static struct { char key[KEY_MAX], art[QUERY_MAX], cover[QUERY_MAX]; unsigned seq; } g_req;
static unsigned g_built_seq;
static bool g_ready;

/* sources the loader keeps (its own: no lock) */
typedef struct { char key[KEY_MAX]; image_t img; unsigned base; uint64_t used, failed_at; } wp_source;
static wp_source g_src[SOURCES];
static image_t g_default_src;
static uint64_t g_src_tick;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void blur_line(uint32_t *p, int n, int stride, int r, uint32_t *tmp) {
  for (int i = 0; i < n; i++) tmp[i] = p[(size_t)i * stride];
  unsigned s[4] = {0, 0, 0, 0}, win = (unsigned)(2 * r + 1);
  for (int k = -r; k <= r; k++) {
    uint32_t c = tmp[clampi(k, 0, n - 1)];
    for (int ch = 0; ch < 4; ch++) s[ch] += (c >> (8 * ch)) & 0xff;
  }
  for (int i = 0; i < n; i++) {
    p[(size_t)i * stride] = RGBA(s[0] / win, s[1] / win, s[2] / win, s[3] / win);
    uint32_t a = tmp[clampi(i - r, 0, n - 1)], b = tmp[clampi(i + r + 1, 0, n - 1)];
    for (int ch = 0; ch < 4; ch++) s[ch] += ((b >> (8 * ch)) & 0xff) - ((a >> (8 * ch)) & 0xff);
  }
}

/* three box passes: close to a gaussian */
static void blur(image_t *img, int r) {
  uint32_t *tmp = malloc((size_t)(img->w > img->h ? img->w : img->h) * 4);
  if (!tmp) return;
  for (int pass = 0; pass < 3; pass++) {
    for (int y = 0; y < img->h; y++) blur_line(img->px + (size_t)y * img->w, img->w, 1, r, tmp);
    for (int x = 0; x < img->w; x++) blur_line(img->px + x, img->h, img->w, r, tmp);
  }
  free(tmp);
}

/* the default wallpaper: deep blue night with two soft lights */
static void make_default(image_t *img) {
  img->w = BLUR_W;
  img->h = BLUR_H;
  img->half = NULL;
  img->px = malloc((size_t)BLUR_W * BLUR_H * 4);
  if (!img->px) return;
  static const struct { float u, v, r, a; int cr, cg, cb; } lights[] = {
    {0.16f, 0.10f, 0.62f, 0.60f, 46, 98, 214}, {0.88f, 0.86f, 0.55f, 0.48f, 112, 52, 176}, {0.58f, 0.42f, 0.36f, 0.16f, 20, 150, 180}};
  for (int y = 0; y < BLUR_H; y++)
    for (int x = 0; x < BLUR_W; x++) {
      float r = 9, g = 12, b = 24;
      for (size_t i = 0; i < sizeof lights / sizeof *lights; i++) {
        float du = ((float)x / (BLUR_W - 1) - lights[i].u) * 16.0f / 9.0f, dv = (float)y / (BLUR_H - 1) - lights[i].v;
        float d = (du * du + dv * dv) / (lights[i].r * lights[i].r);
        if (d >= 1) continue;
        float w = (1 - d) * (1 - d) * lights[i].a;
        r += (lights[i].cr - r) * w;
        g += (lights[i].cg - g) * w;
        b += (lights[i].cb - b) * w;
      }
      img->px[(size_t)y * BLUR_W + x] = RGB((int)r, (int)g, (int)b);
    }
  blur(img, 6);
}

/* scales src to the screen and darkens it (more on the left and at the bottom, where the lists
 * and the hints are, plus a vignette): base = overall brightness, 0..256 */
static void compose(uint32_t *out, const image_t *src, unsigned base) {
  static int x0s[SCREEN_W], x1s[SCREEN_W], y0s[SCREEN_H], y1s[SCREEN_H];
  static unsigned fxs[SCREEN_W], fys[SCREEN_H], kx[SCREEN_W], ky[SCREEN_H];
  const uint32_t tint = RGB(5, 8, 16);
  for (int x = 0; x < SCREEN_W; x++) {
    int64_t sx = ((int64_t)(2 * x + 1) * src->w << 16) / (2 * SCREEN_W) - 32768;
    sx = sx < 0 ? 0 : sx > ((int64_t)(src->w - 1) << 16) ? (int64_t)(src->w - 1) << 16 : sx;
    x0s[x] = (int)(sx >> 16);
    x1s[x] = x0s[x] + 1 < src->w ? x0s[x] + 1 : x0s[x];
    fxs[x] = (unsigned)((sx >> 8) & 0xff);
    float u = (float)x / (SCREEN_W - 1), c = (u - 0.5f) * 2;
    kx[x] = (unsigned)(256.0f * (0.66f + 0.34f * u) * (1 - 0.16f * c * c));
  }
  for (int y = 0; y < SCREEN_H; y++) {
    int64_t sy = ((int64_t)(2 * y + 1) * src->h << 16) / (2 * SCREEN_H) - 32768;
    sy = sy < 0 ? 0 : sy > ((int64_t)(src->h - 1) << 16) ? (int64_t)(src->h - 1) << 16 : sy;
    y0s[y] = (int)(sy >> 16);
    y1s[y] = y0s[y] + 1 < src->h ? y0s[y] + 1 : y0s[y];
    fys[y] = (unsigned)((sy >> 8) & 0xff);
    float v = (float)y / (SCREEN_H - 1), c = (v - 0.5f) * 2;
    /* darker under the title (top) and the button hints (bottom), whatever the picture */
    float edge = v < 0.14f ? 0.72f + 0.28f * (v / 0.14f) : v > 0.89f ? 1.0f - 0.5f * ((v - 0.89f) / 0.11f) : 1.0f;
    ky[y] = (unsigned)(256.0f * (1.0f - 0.42f * v) * (1 - 0.22f * c * c) * edge);
  }
  for (int y = 0; y < SCREEN_H; y++) {
    const uint32_t *r0 = src->px + (size_t)y0s[y] * src->w, *r1 = src->px + (size_t)y1s[y] * src->w;
    uint32_t *dst = out + (size_t)y * SCREEN_W;
    unsigned fy = fys[y], kyb = ky[y] * base;
    for (int x = 0; x < SCREEN_W; x++) {
      uint32_t c = mix_px(mix_px(r0[x0s[x]], r0[x1s[x]], fxs[x]), mix_px(r1[x0s[x]], r1[x1s[x]], fxs[x]), fy);
      unsigned k = (kx[x] * kyb) >> 16;
      dst[x] = mix_px(tint, c, k > 256 ? 256 : k) | 0xff000000u;
    }
  }
}

static bool fetch_fill(const char *query, int w, int h, image_t *img) {
  char path[QUERY_MAX + 16];
  snprintf(path, sizeof path, "/api/icon?%s", query);
  uint8_t *png;
  size_t len;
  if (api_get_raw(path, &png, &len) != 0) return false;
  int rc = img_decode_fill(png, len, w, h, img);
  free(png);
  return rc == 0;
}

/* loader thread: the source picture for key, from its cache or the engine */
static const wp_source *source_for(const char *key, const char *art, const char *cover) {
  static wp_source dflt;
  if (!key[0]) {
    dflt.img = g_default_src;
    dflt.base = 256;
    return g_default_src.px ? &dflt : NULL;
  }
  wp_source *slot = NULL, *lru = &g_src[0];
  for (int i = 0; i < SOURCES; i++) {
    if (!strcmp(g_src[i].key, key)) { slot = &g_src[i]; break; }
    if (g_src[i].used < lru->used) lru = &g_src[i];
  }
  uint64_t now = plat_ms();
  if (slot && !slot->img.px && now - slot->failed_at > COVER_RETRY_MS) {   /* the engine may have been busy */
    slot->used = 0;
    lru = slot;
    slot = NULL;
  }
  if (!slot) {
    slot = lru;
    img_free(&slot->img);
    copy_str(slot->key, sizeof slot->key, key);
    slot->failed_at = 0;
    if (art[0] && fetch_fill(art, ART_W, ART_H, &slot->img)) slot->base = 176;   /* key art: keep it visible */
    else if (cover[0] && fetch_fill(cover, BLUR_W, BLUR_H, &slot->img)) {
      blur(&slot->img, 7);
      slot->base = 160;
    }
    if (!slot->img.px) slot->failed_at = now;
  }
  slot->used = ++g_src_tick;
  return slot->img.px ? slot : NULL;
}

static void *loader(void *arg) {
  (void)arg;
  static char key[KEY_MAX], art[QUERY_MAX], cover[QUERY_MAX];
  static cover_job job;
  for (;;) {
    pthread_mutex_lock(&g_mx);
    while (!g_njobs && !(g_req.seq != g_built_seq && !g_ready)) pthread_cond_wait(&g_cv, &g_mx);
    if (g_njobs) {   /* covers first: they are content, the wallpaper is ambience */
      job = g_jobs[0];
      g_njobs--;
      memmove(g_jobs, g_jobs + 1, (size_t)g_njobs * sizeof *g_jobs);
      pthread_mutex_unlock(&g_mx);
      image_t img;
      memset(&img, 0, sizeof img);
      bool ok = load_cover(job.query, &img);
      pthread_mutex_lock(&g_mx);
      if (g_ndone < JOBS) {
        copy_str(g_done[g_ndone].key, KEY_MAX, job.key);
        g_done[g_ndone].img = img;
        g_done[g_ndone].ok = ok;
        g_ndone++;
      } else img_free(&img);
      pthread_mutex_unlock(&g_mx);
      continue;
    }
    unsigned seq = g_req.seq;
    copy_str(key, sizeof key, g_req.key);
    copy_str(art, sizeof art, g_req.art);
    copy_str(cover, sizeof cover, g_req.cover);
    uint32_t *dst = g_wp[g_work];
    pthread_mutex_unlock(&g_mx);
    const wp_source *src = source_for(key, art, cover);
    if (!src) src = source_for("", "", "");   /* nothing to show for this game: the default */
    if (src) compose(dst, &src->img, src->base);
    pthread_mutex_lock(&g_mx);
    g_built_seq = seq;
    if (src && seq == g_req.seq) g_ready = true;   /* else a newer request is already waiting */
    pthread_mutex_unlock(&g_mx);
  }
  return NULL;
}

void assets_init(void) {
  size_t bytes = (size_t)SCREEN_W * SCREEN_H * 4;
  g_wp_ok = true;
  for (int i = 0; i < 3; i++) g_wp_ok &= (g_wp[i] = malloc(bytes)) != NULL;
  make_default(&g_default_src);
  g_wp_ok &= g_default_src.px != NULL;
  if (g_wp_ok) compose(g_wp[g_cur], &g_default_src, 256);
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&at, 512 * 1024);
  pthread_t th;
  g_thread = pthread_create(&th, &at, loader, NULL) == 0;
  pthread_attr_destroy(&at);
}

void assets_wallpaper(const char *key, const char *art_query, const char *cover_query) {
  if (!g_wp_ok || !g_thread || !strcmp(key, g_wanted)) return;
  copy_str(g_wanted, sizeof g_wanted, key);
  pthread_mutex_lock(&g_mx);
  copy_str(g_req.key, sizeof g_req.key, key);
  copy_str(g_req.art, sizeof g_req.art, art_query ? art_query : "");
  copy_str(g_req.cover, sizeof g_req.cover, cover_query ? cover_query : "");
  g_req.seq++;
  pthread_cond_signal(&g_cv);
  pthread_mutex_unlock(&g_mx);
}

bool assets_update(uint64_t now) {
  if (!g_thread) return false;
  pthread_mutex_lock(&g_mx);
  bool fresh = g_ndone > 0 || g_ready;
  for (int i = 0; i < g_ndone; i++) {
    cover_slot *slot = NULL;
    for (int k = 0; k < COVER_SLOTS && !slot; k++)
      if (!g_slots[k].loaded && !strcmp(g_slots[k].key, g_done[i].key)) slot = &g_slots[k];
    if (!slot) { img_free(&g_done[i].img); continue; }   /* evicted meanwhile */
    slot->img = g_done[i].img;
    slot->loaded = true;
    slot->loaded_at = now;
    slot->retry_at = g_done[i].ok ? 0 : now + COVER_RETRY_MS;
  }
  g_ndone = 0;
  if (g_ready) {
    int old = g_prev;
    g_prev = g_cur;
    g_cur = g_work;
    g_work = old;
    g_ready = false;
    g_fading = true;
    g_fade_at = now;
    pthread_cond_signal(&g_cv);   /* a newer request may be waiting for the free buffer */
  }
  pthread_mutex_unlock(&g_mx);
  return fresh;
}

void assets_draw_wallpaper(void) {
  if (!g_wp_ok) {
    gfx_gradient(0, 0, SCREEN_W, SCREEN_H, RGB(16, 22, 38), RGB(6, 8, 14));
    return;
  }
  if (g_fading) {
    float t = anim_t(g_fade_at, FADE_MS);
    if (t < 1.0f) {
      gfx_blend_full(g_wp[g_prev], g_wp[g_cur], (unsigned)(ease_out(t) * 256.0f));
      return;
    }
    g_fading = false;
  }
  gfx_blit_full(g_wp[g_cur]);
}
