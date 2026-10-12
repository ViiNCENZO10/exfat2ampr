/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE. */
#include "log.h"

#include "api.h"
#include "json.h"
#include "platform.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define L_LINES 256

static char g_lines[L_LINES][APP_LOG_LEN];
static uint64_t g_head;      /* lines ever logged: the next one goes to g_lines[g_head % L_LINES] */
static uint64_t g_shipped;   /* lines the engine has acknowledged */
static unsigned g_rev;
static uint64_t g_t0;
static bool g_started;
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;

void app_log(const char *fmt, ...) {
  char msg[APP_LOG_LEN];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  uint64_t ms = plat_ms() - g_t0;
  char line[APP_LOG_LEN];
  snprintf(line, sizeof line, "%u.%03u %.180s", (unsigned)(ms / 1000), (unsigned)(ms % 1000), msg);
  pthread_mutex_lock(&g_mx);
  memcpy(g_lines[g_head % L_LINES], line, APP_LOG_LEN);
  g_head++;
  if (g_head - g_shipped > L_LINES) g_shipped = g_head - L_LINES;   /* overwritten before the engine got them */
  g_rev++;
  pthread_mutex_unlock(&g_mx);
  plat_debug_text(line);   /* the kernel log too: readable even when the engine is not there */
#ifdef E2A_APP_HOST
  fprintf(stderr, "[app] %s\n", msg);
#endif
}

/* every 400 ms the lines not yet acknowledged go to the engine in one request; when it does not answer (not
 * started yet, busy) they wait and the thread tries again a little later; the renderer never waits */
static void *ship_thread(void *arg) {
  (void)arg;
  static char batch[12 * 1024];
  for (;;) {
    plat_sleep_ms(400);
    pthread_mutex_lock(&g_mx);
    uint64_t from = g_shipped, to = g_head, i = from;
    size_t n = 0;
    for (; i < to; i++) {
      const char *l = g_lines[i % L_LINES];
      size_t ll = strlen(l);
      if (n + ll + 2 > sizeof batch) break;
      memcpy(batch + n, l, ll);
      n += ll;
      batch[n++] = '\n';
    }
    pthread_mutex_unlock(&g_mx);
    if (n == 0) continue;
    jdoc d;
    int st = api_post_bytes("/api/applog", batch, n, &d);
    json_free(&d);
    if (st == 200) {
      pthread_mutex_lock(&g_mx);
      if (g_shipped < i) g_shipped = i;
      pthread_mutex_unlock(&g_mx);
    } else plat_sleep_ms(2500);
  }
  return NULL;
}

void app_log_start(void) {
  if (g_started) return;
  g_started = true;
  g_t0 = plat_ms();
  pthread_t th;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&at, 256 * 1024);
  if (pthread_create(&th, &at, ship_thread, NULL) != 0) g_started = false;
  pthread_attr_destroy(&at);
}

int app_log_copy(char (*out)[APP_LOG_LEN], int max, int skip_newest) {
  pthread_mutex_lock(&g_mx);
  uint64_t have = g_head < L_LINES ? g_head : L_LINES;
  if (skip_newest < 0) skip_newest = 0;
  if ((uint64_t)skip_newest > have) skip_newest = (int)have;
  uint64_t end = g_head - (uint64_t)skip_newest, avail = have - (uint64_t)skip_newest;
  int n = avail < (uint64_t)max ? (int)avail : max;
  for (int k = 0; k < n; k++) memcpy(out[k], g_lines[(end - (uint64_t)n + (uint64_t)k) % L_LINES], APP_LOG_LEN);
  pthread_mutex_unlock(&g_mx);
  return n;
}

int app_log_count(void) {
  pthread_mutex_lock(&g_mx);
  int n = g_head < L_LINES ? (int)g_head : L_LINES;
  pthread_mutex_unlock(&g_mx);
  return n;
}

unsigned app_log_rev(void) { return __atomic_load_n(&g_rev, __ATOMIC_RELAXED); }

const char *app_log_host(const char *url, char *out, size_t cap) {
  const char *p = strstr(url, "://");
  p = p ? p + 3 : url;
  size_t n = strcspn(p, "/?");
  if (n >= cap) n = cap - 1;
  memcpy(out, p, n);
  out[n] = 0;
  return out;
}
