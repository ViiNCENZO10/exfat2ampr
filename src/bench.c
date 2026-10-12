/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE. */
#include "bench.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SPEED_FILE E2A_STATE_DIR "/speed.txt"
#define MAX_KEPT 16
#define READ_CHUNK (256 * 1024)
#define READ_MIN_FILE (256LL << 20)
#define WRITE_CHUNK (1 << 20)
#define WRITE_MAX (96LL << 20)
#define PHASE_MS 2500

static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static struct { char base[300]; e2a_speed s; } g_kept[MAX_KEPT];
static int g_nkept;
static bool g_loaded;

static uint64_t now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* ------------------------------------------------------------ kept results */
static void load_kept(void) {
  if (g_loaded) return;
  g_loaded = true;
  FILE *f = fopen(SPEED_FILE, "r");
  if (!f) return;
  char line[512];
  while (g_nkept < MAX_KEPT && fgets(line, sizeof line, f)) {
    char *tab1 = strchr(line, '\t');
    if (!tab1) continue;
    *tab1 = 0;
    int r = -1, w = -1;
    long long at = 0;
    if (sscanf(tab1 + 1, "%d\t%d\t%lld", &r, &w, &at) != 3) continue;
    snprintf(g_kept[g_nkept].base, sizeof g_kept[0].base, "%.299s", line);
    g_kept[g_nkept].s.read_mbps = r;
    g_kept[g_nkept].s.write_mbps = w;
    g_kept[g_nkept].s.at = at;
    g_nkept++;
  }
  fclose(f);
}

static void save_kept(void) {
  char tmp[300];
  snprintf(tmp, sizeof tmp, "%s.tmp", SPEED_FILE);
  FILE *f = fopen(tmp, "w");
  if (!f) return;
  for (int i = 0; i < g_nkept; i++)
    fprintf(f, "%s\t%d\t%d\t%lld\n", g_kept[i].base, g_kept[i].s.read_mbps, g_kept[i].s.write_mbps, (long long)g_kept[i].s.at);
  if (fclose(f) == 0) rename(tmp, SPEED_FILE);
  else unlink(tmp);
}

bool bench_get(const char *base, e2a_speed *out) {
  pthread_mutex_lock(&g_mx);
  load_kept();
  bool found = false;
  for (int i = 0; i < g_nkept && !found; i++)
    if (!strcmp(g_kept[i].base, base)) {
      *out = g_kept[i].s;
      found = true;
    }
  pthread_mutex_unlock(&g_mx);
  return found;
}

static void keep(const char *base, const e2a_speed *s) {
  pthread_mutex_lock(&g_mx);
  load_kept();
  int i = 0;
  while (i < g_nkept && strcmp(g_kept[i].base, base) != 0) i++;
  if (i == g_nkept) {
    if (g_nkept == MAX_KEPT) {   /* the oldest measure makes room */
      int old = 0;
      for (int k = 1; k < g_nkept; k++)
        if (g_kept[k].s.at < g_kept[old].s.at) old = k;
      i = old;
    } else g_nkept++;
  }
  snprintf(g_kept[i].base, sizeof g_kept[0].base, "%s", base);
  g_kept[i].s = *s;
  save_kept();
  pthread_mutex_unlock(&g_mx);
}

/* ------------------------------------------------------------------- read */
/* the largest regular file under dir, at most depth folders down and 4000 entries looked at */
static void largest(const char *dir, int depth, char *best, size_t cap, int64_t *best_size, int *budget) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *de;
  while (*budget > 0 && (de = readdir(d)) != NULL) {
    if (de->d_name[0] == '.' || de->d_name[0] == '$') continue;
    (*budget)--;
    char path[700];
    int n = snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
    if (n < 0 || (size_t)n >= sizeof path) continue;
    struct stat st;
    if (lstat(path, &st) != 0) continue;
    if (S_ISREG(st.st_mode) && st.st_size > *best_size) {
      *best_size = st.st_size;
      snprintf(best, cap, "%s", path);
    } else if (S_ISDIR(st.st_mode) && depth > 0) {
      largest(path, depth - 1, best, cap, best_size, budget);
    }
  }
  closedir(d);
}

static int read_test(const char *base, const char *mount) {
  char file[700] = "";
  int64_t size = 0;
  int budget = 4000;
  largest(base, 3, file, sizeof file, &size, &budget);
  if (size < READ_MIN_FILE && mount && mount[0]) largest(mount, 1, file, sizeof file, &size, &budget);
  if (size < READ_MIN_FILE) return -1;
  int fd = open(file, O_RDONLY);
  if (fd < 0) return -1;
  uint8_t *buf = malloc(READ_CHUNK);
  if (!buf) { close(fd); return -1; }
  uint64_t x = 0x9E3779B97F4A7C15ull ^ (uint64_t)size, t0 = now_us(), bytes = 0;
  for (int i = 0; i < 256; i++) {
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;   /* xorshift: offsets spread over the whole file */
    off_t off = (off_t)((x % (uint64_t)(size - READ_CHUNK)) & ~(uint64_t)4095);
    ssize_t r = pread(fd, buf, READ_CHUNK, off);
    if (r <= 0) break;
    bytes += (uint64_t)r;
    if (i >= 16 && now_us() - t0 > PHASE_MS * 1000ull) break;
  }
  uint64_t us = now_us() - t0;
  free(buf);
  close(fd);
  e2a_log("speed test: read %llu bytes of %s in %llu us", (unsigned long long)bytes, file, (unsigned long long)us);
  return bytes && us ? (int)(bytes / us) : -1;   /* bytes per microsecond = MB/s */
}

/* ------------------------------------------------------------------ write */
static int write_test(const char *base, char *err, size_t errcap) {
  char tmp[400];
  int n = snprintf(tmp, sizeof tmp, "%s/.e2a-speedtest.XXXXXX", base);
  if (n < 0 || (size_t)n >= sizeof tmp) { snprintf(err, errcap, "benchmark path too long"); return -1; }
  int fd = mkstemp(tmp);   /* a new file of its own: never truncates an existing one or follows a link */
  if (fd < 0) { snprintf(err, errcap, "cannot write in %s: %s", base, strerror(errno)); return -1; }
  uint8_t *buf = malloc(WRITE_CHUNK);
  if (!buf) { close(fd); unlink(tmp); snprintf(err, errcap, "out of memory"); return -1; }
  uint64_t x = 0x2545F4914F6CDD1Dull;
  for (size_t i = 0; i + 8 <= WRITE_CHUNK; i += 8) {   /* not compressible, like packed game data */
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    memcpy(buf + i, &x, 8);
  }
  uint64_t t0 = now_us(), bytes = 0;
  bool ok = true;
  while ((int64_t)bytes < WRITE_MAX) {
    if (e2a_write_all(fd, buf, WRITE_CHUNK) != 0) { ok = false; break; }
    bytes += WRITE_CHUNK;
    if (now_us() - t0 > PHASE_MS * 1000ull) break;
  }
  if (fsync(fd) != 0) ok = false;
  uint64_t us = now_us() - t0;
  if (close(fd) != 0) ok = false;
  unlink(tmp);
  free(buf);
  if (!ok) { snprintf(err, errcap, "write failed in %s (full or removed?)", base); return -1; }
  return us ? (int)(bytes / us) : -1;
}

int bench_run(const char *base, e2a_speed *out, char *err, size_t errcap) {
  err[0] = 0;
  out->read_mbps = out->write_mbps = -1;
  out->at = (int64_t)time(NULL);
  /* the mount point: <mount>/homebrew -> <mount> */
  char mount[300];
  snprintf(mount, sizeof mount, "%s", base);
  char *slash = strrchr(mount, '/');
  if (slash && slash != mount) *slash = 0;
  out->read_mbps = read_test(base, mount);
  out->write_mbps = write_test(base, err, errcap);
  if (out->write_mbps < 0 && out->read_mbps < 0) return -1;
  err[0] = 0;
  keep(base, out);
  e2a_log("speed test %s: read %d MB/s, write %d MB/s", base, out->read_mbps, out->write_mbps);
  return 0;
}
