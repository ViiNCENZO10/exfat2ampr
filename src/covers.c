/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE. */
#include "covers.h"

#include "common.h"
#include "smp.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define COVERS_DIR E2A_STATE_DIR "/covers"
#define WANT_MAX 256

static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static char g_want[WANT_MAX][16];
static int g_nwant;
static bool g_started, g_kick;
static unsigned g_found, g_missing, g_passes;

static bool tid_ok(const char *t) {
  bool ok = t && strlen(t) == 9;
  for (int i = 0; ok && i < 9; i++) ok = i < 4 ? (t[i] >= 'A' && t[i] <= 'Z') : (t[i] >= '0' && t[i] <= '9');
  return ok;
}

static void cover_path(const char *tid, bool bg, char *out, size_t cap) {
  snprintf(out, cap, "%s/%.9s%s.png", COVERS_DIR, tid, bg ? "_bg" : "");
}

bool covers_png_ok(const void *data, size_t len) {
  const uint8_t *p = data;
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  if (!p || len < 33 || memcmp(p, sig, 8) != 0 || memcmp(p + 12, "IHDR", 4) != 0) return false;
  uint32_t w = (uint32_t)p[16] << 24 | (uint32_t)p[17] << 16 | (uint32_t)p[18] << 8 | p[19];
  uint32_t h = (uint32_t)p[20] << 24 | (uint32_t)p[21] << 16 | (uint32_t)p[22] << 8 | p[23];
  return w >= 1 && h >= 1 && w <= 8192 && h <= 8192;
}

bool covers_get(const char *title_id, bool bg, uint8_t **png, size_t *len) {
  if (!tid_ok(title_id)) return false;
  char path[300];
  cover_path(title_id, bg, path, sizeof path);
  char *data = NULL;
  size_t n = 0;
  if (e2a_read_text(path, &data, &n, 32u << 20) != 0) return false;
  if (!covers_png_ok(data, n)) { free(data); return false; }
  *png = (uint8_t *)data;
  *len = n;
  return true;
}

void covers_put(const char *title_id, bool bg, const void *png, size_t len) {
  if (!tid_ok(title_id) || !covers_png_ok(png, len)) return;
  char path[300];
  cover_path(title_id, bg, path, sizeof path);
  int64_t have = e2a_file_size(path);
  if (have == (int64_t)len) return;   /* the same picture: nothing to rewrite */
  e2a_mkdir_p(COVERS_DIR);
  e2a_write_file_atomic(path, png, len);
}

static bool put_file(const char *tid, bool bg, const char *path) {
  char *data = NULL;
  size_t n = 0;
  if (e2a_read_text(path, &data, &n, 32u << 20) != 0) return false;
  bool ok = covers_png_ok(data, n);
  if (ok) covers_put(tid, bg, data, n);
  free(data);
  return ok;
}

/* ------------------------------------------------------------------ music -- */
static void sound_path(const char *tid, char *out, size_t cap) { snprintf(out, cap, "%s/%.9s.at9", COVERS_DIR, tid); }

/* a RIFF/WAVE whose "fmt " chunk is WAVE_FORMAT_EXTENSIBLE with the ATRAC9 sub-format */
bool covers_sound_ok(const void *data, size_t len) {
  static const uint8_t guid[16] = {0xd2, 0x42, 0xe1, 0x47, 0xba, 0x36, 0x8d, 0x4d, 0x88, 0xfc, 0x61, 0x65, 0x4f, 0x8c, 0x83, 0x6c};
  const uint8_t *p = data;
  if (!p || len < 64 || len > (32u << 20) || memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "WAVE", 4) != 0) return false;
  for (size_t at = 12; at + 8 <= len;) {
    size_t clen = (size_t)p[at + 4] | (size_t)p[at + 5] << 8 | (size_t)p[at + 6] << 16 | (size_t)p[at + 7] << 24;
    if (memcmp(p + at, "fmt ", 4) == 0)
      return clen >= 52 && at + 8 + 52 <= len && p[at + 8] == 0xFE && p[at + 9] == 0xFF && memcmp(p + at + 8 + 24, guid, 16) == 0;
    if (clen > len - at - 8) return false;
    at += 8 + clen + (clen & 1);
  }
  return false;
}

bool covers_get_sound(const char *title_id, uint8_t **at9, size_t *len) {
  if (!tid_ok(title_id)) return false;
  char path[300];
  sound_path(title_id, path, sizeof path);
  char *data = NULL;
  size_t n = 0;
  if (e2a_read_text(path, &data, &n, 32u << 20) != 0) return false;
  if (!covers_sound_ok(data, n)) { free(data); return false; }
  *at9 = (uint8_t *)data;
  *len = n;
  return true;
}

void covers_put_sound(const char *title_id, const void *at9, size_t len) {
  if (!tid_ok(title_id) || !covers_sound_ok(at9, len)) return;
  char path[300];
  sound_path(title_id, path, sizeof path);
  if (e2a_file_size(path) == (int64_t)len) return;
  e2a_mkdir_p(COVERS_DIR);
  e2a_write_file_atomic(path, at9, len);
}

static void put_sound_file(const char *tid, const char *path) {
  char *data = NULL;
  size_t n = 0;
  if (e2a_read_text(path, &data, &n, 32u << 20) != 0) return;
  covers_put_sound(tid, data, n);
  free(data);
}

void covers_put_from_dir(const char *title_id, const char *root) {
  static const char *const icons[] = {"sce_sys/icon0.png", "sce_sys/icon0_00.png", NULL};
  static const char *const art[] = {"sce_sys/pic0.png", "sce_sys/pic1.png", NULL};
  char path[900];
  for (int i = 0; icons[i]; i++) { snprintf(path, sizeof path, "%s/%s", root, icons[i]); if (put_file(title_id, false, path)) break; }
  for (int i = 0; art[i]; i++) { snprintf(path, sizeof path, "%s/%s", root, art[i]); if (put_file(title_id, true, path)) break; }
  snprintf(path, sizeof path, "%s/sce_sys/snd0.at9", root);
  if (e2a_is_file(path)) put_sound_file(title_id, path);
}

void covers_want(const char *title_id) {
  if (!tid_ok(title_id)) return;
  pthread_mutex_lock(&g_mx);
  bool have = false;
  for (int i = 0; i < g_nwant && !have; i++) have = !strcmp(g_want[i], title_id);
  if (!have && g_nwant < WANT_MAX) { snprintf(g_want[g_nwant++], 16, "%s", title_id); g_kick = true; pthread_cond_signal(&g_cv); }
  pthread_mutex_unlock(&g_mx);
}

/* one title: a mounted image's folder first, then ShadowMountPlus with a minute to answer */
static bool fetch(const char *tid, const smp_game *g) {
  uint8_t *png = NULL;
  size_t len = 0;
  if (covers_get(tid, false, &png, &len)) { free(png); return true; }
  if (g && g->mounted && g->runtime_path[0] == '/') {
    covers_put_from_dir(tid, g->runtime_path);
    if (covers_get(tid, false, &png, &len)) { free(png); return true; }
  }
  if (smp_icon_wait(tid, &png, &len, 60) == 0) {
    bool ok = covers_png_ok(png, len);
    if (ok) covers_put(tid, false, png, len);
    free(png);
    return ok;
  }
  return false;
}

static void *covers_thread(void *arg) {
  (void)arg;
  sleep(8);   /* the engine's start first */
  for (;;) {
    smp_game *games = calloc(256, sizeof *games);
    int n = games ? smp_list_games(games, 256) : -1;
    unsigned found = 0, missing = 0;
    for (int i = 0; i < n; i++) {
      if (!tid_ok(games[i].title_id)) continue;
      if (fetch(games[i].title_id, &games[i])) found++;
      else missing++;
    }
    /* titles the listing asked for that ShadowMountPlus does not list (folders, duplicates) */
    for (;;) {
      char tid[16];
      pthread_mutex_lock(&g_mx);
      if (g_nwant == 0) { pthread_mutex_unlock(&g_mx); break; }
      snprintf(tid, sizeof tid, "%s", g_want[--g_nwant]);
      pthread_mutex_unlock(&g_mx);
      smp_game one, *gp = NULL;
      for (int i = 0; i < n && !gp; i++) if (!strcmp(games[i].title_id, tid)) gp = &games[i];
      if (!gp && smp_game_info(tid, &one) == 0) gp = &one;
      if (fetch(tid, gp)) found++;
      else missing++;
    }
    free(games);
    pthread_mutex_lock(&g_mx);
    g_found = found;
    g_missing = missing;
    g_passes++;
    if (missing) e2a_log("covers: %u found, %u still missing", found, missing);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 600;
    while (!g_kick) if (pthread_cond_timedwait(&g_cv, &g_mx, &ts) != 0) break;
    g_kick = false;
    pthread_mutex_unlock(&g_mx);
    sleep(2);   /* a burst of requests becomes one pass */
  }
  return NULL;
}

void covers_start(void) {
  pthread_mutex_lock(&g_mx);
  bool start = !g_started;
  g_started = true;
  pthread_mutex_unlock(&g_mx);
  if (!start) return;
  e2a_mkdir_p(COVERS_DIR);
  pthread_t th;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&at, 512 * 1024);
  pthread_create(&th, &at, covers_thread, NULL);
  pthread_attr_destroy(&at);
}

int covers_status_json(char *json, size_t cap) {
  pthread_mutex_lock(&g_mx);
  int n = snprintf(json, cap, "{\"found\":%u,\"missing\":%u,\"passes\":%u,\"queued\":%d}", g_found, g_missing, g_passes, g_nwant);
  pthread_mutex_unlock(&g_mx);
  return n > 0 && (size_t)n < cap ? 0 : -1;
}
