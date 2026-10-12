/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE. */
#include "update.h"

#include "api.h"
#include "json.h"
#include "platform.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_include)
#if __has_include("update_config.h")
#include "update_config.h"   /* written by the CI: the manifest of the repository that publishes the releases */
#endif
#endif
#ifndef E2A_UPDATE_URL
#define E2A_UPDATE_URL "https://raw.githubusercontent.com/exFAT2AMPR/exfat2ampr/updates/update.json"
#endif

#define PIECE (1u << 20)   /* what goes to the engine at a time (its requests stop at 2 MiB) */

static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static upd_state g_state = UPD_IDLE;
static char g_current[32], g_version[32], g_package[600], g_sha[80], g_err[200];
static uint64_t g_size;
static int g_progress;
static unsigned g_rev, g_seen;

static void set_state(upd_state s, const char *err) {
  pthread_mutex_lock(&g_mx);
  g_state = s;
  if (err) snprintf(g_err, sizeof g_err, "%s", err);
  g_rev++;
  pthread_mutex_unlock(&g_mx);
}

static const char *manifest_url(char *buf, size_t cap) {
#ifdef E2A_APP_HOST
  /* the CI: the mock engine publishes a manifest too */
  const char *e = getenv("E2A_UPDATE_URL"), *port = getenv("E2A_ENGINE_PORT");
  if (e && *e) return e;
  snprintf(buf, cap, "http://127.0.0.1:%s/update/update.json", port ? port : "6972");
  return buf;
#else
  (void)buf;
  (void)cap;
  return E2A_UPDATE_URL;
#endif
}

/* "1.45" -> comparable number; -1 when it is not a version ("dev") */
static long vnum(const char *v) {
  char *e;
  long a = strtol(v, &e, 10);
  if (e == v || *e != '.') return -1;
  const char *m = e + 1;
  long b = strtol(m, &e, 10);
  if (e == m) return -1;
  return a * 100000 + b;
}

static void start_thread(void *(*fn)(void *)) {
  pthread_t th;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&at, 256 * 1024);
  if (pthread_create(&th, &at, fn, NULL) != 0) fn(NULL);
  pthread_attr_destroy(&at);
}

/* --------------------------------------------------------------- check -- */
typedef struct { char *buf; size_t len, cap; } membuf;

static int to_memory(void *ctx, const uint8_t *data, size_t len) {
  membuf *m = ctx;
  if (m->len + len > m->cap) return -1;
  memcpy(m->buf + m->len, data, len);
  m->len += len;
  return 0;
}

static void *check_thread(void *arg) {
  (void)arg;
  static char buf[16385];
  char url[200], err[200];
  membuf m = {buf, 0, sizeof buf - 1};
  int st = plat_https_get(manifest_url(url, sizeof url), to_memory, &m, false);
  if (st != 200) {
    snprintf(err, sizeof err, st < 0 ? "no answer from the update server (%d)" : "update server answered HTTP %d", st);
    set_state(UPD_UNREACHABLE, err);
    return NULL;
  }
  buf[m.len] = 0;
  jdoc d;
  if (json_parse(buf, m.len, &d) != 0 || !d.root) { set_state(UPD_UNREACHABLE, "the update manifest cannot be read"); return NULL; }
  pthread_mutex_lock(&g_mx);
  snprintf(g_version, sizeof g_version, "%s", json_str(d.root, "version", ""));
  snprintf(g_package, sizeof g_package, "%s", json_str(d.root, "package", ""));
  snprintf(g_sha, sizeof g_sha, "%s", json_str(d.root, "sha256", ""));
  g_size = (uint64_t)json_num(d.root, "size", 0);
  long have = vnum(g_current), found = vnum(g_version);
  bool usable = g_package[0] && strlen(g_sha) == 64 && g_size > 0;
  pthread_mutex_unlock(&g_mx);
  json_free(&d);
  set_state(have >= 0 && found > have && usable ? UPD_AVAILABLE : UPD_LATEST, NULL);
  return NULL;
}

void update_check(const char *current) {
  pthread_mutex_lock(&g_mx);
  bool busy = g_state == UPD_CHECKING || g_state == UPD_DOWNLOADING || g_state == UPD_INSTALLED;
  if (!busy) {
    snprintf(g_current, sizeof g_current, "%s", current ? current : "");
    g_state = UPD_CHECKING;
    g_rev++;
  }
  pthread_mutex_unlock(&g_mx);
  if (!busy) start_thread(check_thread);
}

/* ------------------------------------------------------------- install -- */
typedef struct { uint8_t *piece; size_t used; uint64_t off, total; char err[200]; } upload;

static int send_piece(upload *u) {
  char path[64];
  snprintf(path, sizeof path, "/api/update/chunk?off=%llu", (unsigned long long)u->off);
  jdoc d;
  int st = api_post_bytes(path, u->piece, u->used, &d);
  if (st != 200) {
    snprintf(u->err, sizeof u->err, "%s", d.root ? json_str(d.root, "error", "engine refused the piece") : "engine not answering");
    json_free(&d);
    return -1;
  }
  json_free(&d);
  u->off += u->used;
  u->used = 0;
  pthread_mutex_lock(&g_mx);
  g_progress = u->total ? (int)(u->off * 100 / u->total) : 0;
  g_rev++;
  pthread_mutex_unlock(&g_mx);
  return 0;
}

static int to_engine(void *ctx, const uint8_t *data, size_t len) {
  upload *u = ctx;
  while (len) {
    size_t k = PIECE - u->used < len ? PIECE - u->used : len;
    memcpy(u->piece + u->used, data, k);
    u->used += k;
    data += k;
    len -= k;
    if (u->used == PIECE && send_piece(u) != 0) return -1;
  }
  return 0;
}

static void *install_thread(void *arg) {
  (void)arg;
  char form[256] = "", sizes[32], package[600], sha[80];
  uint64_t size;
  pthread_mutex_lock(&g_mx);
  snprintf(package, sizeof package, "%s", g_package);
  snprintf(sha, sizeof sha, "%s", g_sha);
  size = g_size;
  pthread_mutex_unlock(&g_mx);
  snprintf(sizes, sizeof sizes, "%llu", (unsigned long long)size);
  form_add(form, sizeof form, "size", sizes);
  form_add(form, sizeof form, "sha256", sha);
  jdoc d;
  int st = api_call("/api/update/begin", form, &d);
  char err[200];
  snprintf(err, sizeof err, "%s", d.root ? json_str(d.root, "error", "") : "engine not answering");
  json_free(&d);
  if (st != 200) { set_state(UPD_FAILED, err); return NULL; }
  upload u;
  memset(&u, 0, sizeof u);
  u.total = size;
  u.piece = malloc(PIECE);
  if (!u.piece) { set_state(UPD_FAILED, "out of memory"); return NULL; }
  for (int attempt = 0; attempt < 2; attempt++) {
    if (attempt) {   /* once more from the start: the engine truncates its file */
      st = api_call("/api/update/begin", form, &d);
      json_free(&d);
      if (st != 200) break;
      u.off = 0; u.used = 0; u.err[0] = 0;
    }
    st = plat_https_get(package, to_engine, &u, true);
    if (st == 200 && !u.err[0] && u.used) send_piece(&u);
    if (st == 200 && !u.err[0]) break;
    if (!u.err[0]) {
      if (st < 0) snprintf(u.err, sizeof u.err, "download failed (%d, 0x%08x)", st, plat_https_last_error());
      else snprintf(u.err, sizeof u.err, "download answered HTTP %d", st);
    }
  }
  free(u.piece);
  if (st != 200 || u.err[0]) {
    if (!u.err[0]) snprintf(u.err, sizeof u.err, "engine refused to restart the download (HTTP %d)", st);
    set_state(UPD_FAILED, u.err);
    return NULL;
  }
  st = api_call("/api/update/install", "", &d);
  snprintf(err, sizeof err, "%s", d.root ? json_str(d.root, "error", "") : "engine not answering");
  json_free(&d);
  set_state(st == 200 ? UPD_INSTALLED : UPD_FAILED, st == 200 ? NULL : err);
  return NULL;
}

void update_install(void) {
  pthread_mutex_lock(&g_mx);
  bool ok = g_state == UPD_AVAILABLE || (g_state == UPD_FAILED && g_package[0]);
  if (ok) {
    g_state = UPD_DOWNLOADING;
    g_progress = 0;
    g_rev++;
  }
  pthread_mutex_unlock(&g_mx);
  if (ok) start_thread(install_thread);
}

upd_state update_state(int *progress, char *version, size_t vcap, uint64_t *size, char *err, size_t ecap) {
  pthread_mutex_lock(&g_mx);
  upd_state s = g_state;
  if (progress) *progress = g_progress;
  if (version) snprintf(version, vcap, "%s", g_version);
  if (size) *size = g_size;
  if (err) snprintf(err, ecap, "%s", g_err);
  pthread_mutex_unlock(&g_mx);
  return s;
}

bool update_changed(void) {
  pthread_mutex_lock(&g_mx);
  bool changed = g_rev != g_seen;
  g_seen = g_rev;
  pthread_mutex_unlock(&g_mx);
  return changed;
}
