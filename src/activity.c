/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "activity.h"
#include "job.h"
#include "storage.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#ifndef E2A_HOST
#include <stddef.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/user.h>

/* layout used by ShadowMountPlus (sm_platform.h) */
typedef struct {
  uint32_t app_id;
  uint64_t unknown1;
  char title_id[14];
  char unknown2[0x3c];
} e2a_app_info;
int sceKernelGetAppInfo(pid_t pid, e2a_app_info *info);
#endif

#define SETTINGS_FILE E2A_STATE_DIR "/settings.ini"
#define GAME_RATE (64.0 * 1024 * 1024)   /* disk traffic allowed while a game runs, bytes/s */
#define MAX_TITLES 8

static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static char g_titles[MAX_TITLES][16];
static int g_ntitles;
static volatile bool g_enabled = true;
static volatile int g_app_pid;
static volatile double g_app_seen;
static volatile uint64_t g_rev;
static char g_paused_for[16], g_paused_dest[600];

static bool is_game_title(const char *t) {
  if (strlen(t) != 9 || (strncmp(t, "PPSA", 4) && strncmp(t, "CUSA", 4))) return false;
  for (int i = 4; i < 9; i++)
    if (t[i] < '0' || t[i] > '9') return false;
  return strcmp(t, E2A_APP_TITLE_ID) != 0;   /* the home-screen app is not a game */
}

static int add_title(char out[][16], int n, int max, const char *t) {
  if (!is_game_title(t) || n >= max) return n;
  for (int k = 0; k < n; k++)
    if (!strcmp(out[k], t)) return n;
  snprintf(out[n], 16, "%s", t);
  return n + 1;
}

#ifdef E2A_HOST
/* host tests: E2A_FAKE_RUNNING="PPSA01234,PPSA02530" */
static int scan_titles(char out[][16], int max) {
  const char *env = getenv("E2A_FAKE_RUNNING");
  int n = 0;
  char buf[256];
  snprintf(buf, sizeof buf, "%s", env ? env : "");
  for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) n = add_title(out, n, max, t);
  return n;
}
#else
static int scan_titles(char out[][16], int max) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t len = 0;
  if (sysctl(mib, 4, NULL, &len, NULL, 0) != 0 || len == 0) return 0;
  len += len / 4;   /* room for processes started meanwhile */
  uint8_t *buf = malloc(len);
  if (!buf) return 0;
  if (sysctl(mib, 4, buf, &len, NULL, 0) != 0) { free(buf); return 0; }
  int n = 0;
  const size_t pid_end = offsetof(struct kinfo_proc, ki_pid) + sizeof(pid_t);
  for (uint8_t *p = buf; (size_t)(buf + len - p) >= sizeof(int);) {
    int size = 0;
    memcpy(&size, p, sizeof size);
    if (size <= 0 || (size_t)size > (size_t)(buf + len - p) || (size_t)size < pid_end) break;
    pid_t pid = 0;
    memcpy(&pid, p + offsetof(struct kinfo_proc, ki_pid), sizeof pid);
    p += size;
    if (pid <= 0) continue;
    e2a_app_info ai;
    memset(&ai, 0, sizeof ai);
    if (sceKernelGetAppInfo(pid, &ai) != 0) continue;
    char t[16];
    size_t l = strnlen(ai.title_id, sizeof ai.title_id);
    if (l >= sizeof t) continue;
    memcpy(t, ai.title_id, l);
    t[l] = 0;
    n = add_title(out, n, max, t);
  }
  free(buf);
  return n;
}
#endif

int activity_running_titles(char titles[][16], int max) {
  pthread_mutex_lock(&g_mx);
  int n = g_ntitles < max ? g_ntitles : max;
  for (int i = 0; i < n; i++) { memcpy(titles[i], g_titles[i], 16); titles[i][15] = 0; }
  pthread_mutex_unlock(&g_mx);
  return n;
}

bool activity_title_running(const char *title_id) {
  bool r = false;
  pthread_mutex_lock(&g_mx);
  for (int i = 0; i < g_ntitles && !r; i++) r = !strcmp(g_titles[i], title_id);
  pthread_mutex_unlock(&g_mx);
  return r;
}

void activity_app_seen(int pid) { g_app_pid = pid; g_app_seen = e2a_now(); }
bool activity_app_alive(void) { return g_app_seen > 0 && e2a_now() - g_app_seen < 6.0; }

bool activity_game_mode_enabled(void) { return g_enabled; }
void activity_set_game_mode_enabled(bool on) {
  g_enabled = on;
  e2a_write_file_atomic(SETTINGS_FILE, on ? "game_mode=1\n" : "game_mode=0\n", 12);
  e2a_log("game mode %s", on ? "enabled" : "disabled");
}

bool activity_game_mode(void) {
  if (!g_enabled) return false;
  pthread_mutex_lock(&g_mx);
  bool r = g_ntitles > 0;
  pthread_mutex_unlock(&g_mx);
  return r;
}

int activity_workers(int requested) { return activity_game_mode() && requested > 2 ? 2 : requested; }

/* token bucket: in game mode the conversion moves at most GAME_RATE bytes per second */
void activity_throttle(uint64_t bytes) {
  if (!activity_game_mode()) return;
  static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
  static double next;
  pthread_mutex_lock(&mx);
  double now = e2a_now();
  if (next < now - 1.0) next = now;
  next += (double)bytes / GAME_RATE;
  double wait = next - now;
  pthread_mutex_unlock(&mx);
  if (wait > 0) usleep((useconds_t)((wait > 2.0 ? 2.0 : wait) * 1e6));
}

uint64_t activity_storage_rev(void) { return g_rev; }

static void *monitor(void *arg) {
  (void)arg;
  uint64_t last_sig = storage_signature();
  bool was_game = false;
  for (;;) {
    char t[MAX_TITLES][16];
    int n = scan_titles(t, MAX_TITLES);
    pthread_mutex_lock(&g_mx);
    memcpy(g_titles, t, sizeof t);
    g_ntitles = n;
    pthread_mutex_unlock(&g_mx);

    bool game = g_enabled && n > 0;
    if (game != was_game) {
      setpriority(PRIO_PROCESS, 0, game ? 20 : 10);
      if (game) e2a_log("game mode: %s is running, the conversion slows down", t[0]);
      else e2a_log("game mode off: full speed");
      was_game = game;
    }

    uint64_t sig = storage_signature();
    if (sig != last_sig) {
      last_sig = sig;
      __atomic_add_fetch(&g_rev, 1, __ATOMIC_RELAXED);
      e2a_log("storage changed (a drive was plugged in or removed)");
    }

    /* a game and its own conversion never run together: pause it, resume when the game is closed */
    job_status s;
    job_get_status(&s);
    /* (an image still waiting for ShadowMountPlus has nothing to pause: it simply keeps waiting) */
    if (s.state == JOB_RUNNING && !s.waiting && s.title_id[0] && !g_paused_for[0] && activity_title_running(s.title_id)) {
      snprintf(g_paused_for, sizeof g_paused_for, "%s", s.title_id);
      snprintf(g_paused_dest, sizeof g_paused_dest, "%s", s.dest);
      job_pause_because("game launched: its conversion is paused and resumes when the game is closed");
      e2a_notify("exfat2ampr: %s launched\nIts conversion is paused and resumes when you close the game", s.title_id);
    } else if (g_paused_for[0] && !activity_title_running(g_paused_for)) {
      if (s.state == JOB_PAUSED && !strcmp(s.dest, g_paused_dest) && job_resume(g_paused_dest) == 0)
        e2a_notify("exfat2ampr: %s closed\nIts conversion resumes", g_paused_for);
      g_paused_for[0] = 0;
    }
    job_watch_storage();
    sleep(2);
  }
  return NULL;
}

void activity_init(void) {
  char *doc = NULL;
  if (e2a_read_text(SETTINGS_FILE, &doc, NULL, 256) == 0) {
    if (strstr(doc, "game_mode=0")) g_enabled = false;
    free(doc);
  }
  pthread_t th;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&th, &at, monitor, NULL) != 0) e2a_log("activity monitor not started");
  pthread_attr_destroy(&at);
}
