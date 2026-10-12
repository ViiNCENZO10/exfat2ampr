/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE. */
#include "diag.h"

#include "common.h"
#include "job.h"
#include "plugins.h"
#include "storage.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct { char *p; size_t n, cap; } tbuf;

static int tb_room(tbuf *b, size_t more) {
  if (b->n + more + 1 <= b->cap) return 0;
  size_t nc = b->cap ? b->cap : 65536;
  while (nc < b->n + more + 1) nc *= 2;
  char *np = realloc(b->p, nc);
  if (!np) return -1;
  b->p = np;
  b->cap = nc;
  return 0;
}

static void tb_raw(tbuf *b, const char *s, size_t n) {
  if (tb_room(b, n) != 0) return;
  memcpy(b->p + b->n, s, n);
  b->n += n;
  b->p[b->n] = 0;
}

static void tb_add(tbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void tb_add(tbuf *b, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0 || tb_room(b, (size_t)n) != 0) return;
  va_start(ap, fmt);
  vsnprintf(b->p + b->n, (size_t)n + 1, fmt, ap);
  va_end(ap);
  b->n += (size_t)n;
}

static bool digit(char c) { return c >= '0' && c <= '9'; }

/* one line with the private parts blanked: the pairing code ("code 123456") and the console's address in
 * "http://192.168.x.y:6972/" */
static void scrub_line(const char *in, size_t n, tbuf *b) {
  for (size_t i = 0; i < n;) {
    if (i + 11 <= n && memcmp(in + i, "code ", 5) == 0 && digit(in[i + 5]) && digit(in[i + 6]) && digit(in[i + 7]) && digit(in[i + 8]) &&
        digit(in[i + 9]) && digit(in[i + 10])) {
      tb_raw(b, "code ******", 11);
      i += 11;
      continue;
    }
    if (i + 8 <= n && memcmp(in + i, "http://", 7) == 0 && digit(in[i + 7])) {
      size_t e = i + 7;
      while (e < n && (digit(in[e]) || in[e] == '.')) e++;
      tb_raw(b, "http://<console>", 16);
      i = e;
      continue;
    }
    tb_raw(b, in + i, 1);
    i++;
  }
}

static void tb_text(tbuf *b, const char *text, size_t len) {
  size_t i = 0;
  while (i < len) {
    size_t e = i;
    while (e < len && text[e] != '\n') e++;
    scrub_line(text + i, e - i, b);
    tb_raw(b, "\n", 1);
    i = e + 1;
  }
}

/* the last `max` bytes of a file (from the next line start) */
static void tb_file_tail(tbuf *b, const char *path, size_t max) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) { tb_add(b, "(%s: %s)\n", path, strerror(errno)); return; }
  struct stat st;
  if (fstat(fd, &st) != 0) { close(fd); return; }
  uint64_t size = (uint64_t)st.st_size, from = size > max ? size - max : 0;
  char *buf = malloc(max + 1);
  if (!buf) { close(fd); return; }
  ssize_t got = pread(fd, buf, max, (off_t)from);
  close(fd);
  if (got <= 0) { free(buf); tb_add(b, "(empty)\n"); return; }
  size_t start = 0;
  if (from > 0) { while (start < (size_t)got && buf[start] != '\n') start++; if (start < (size_t)got) start++; }
  tb_add(b, "(%llu bytes in all, the last %zu shown)\n", (unsigned long long)size, (size_t)got - start);
  tb_text(b, buf + start, (size_t)got - start);
  free(buf);
}

static void section(tbuf *b, const char *title) { tb_add(b, "\n== %s ==\n", title); }

int diag_build(char **out, size_t *len) {
  tbuf b = {0};
  char date[40];
  time_t t = time(NULL);
  struct tm tm;
  localtime_r(&t, &tm);
  strftime(date, sizeof date, "%Y-%m-%d %H:%M:%S", &tm);
  tb_add(&b, "exFAT2AMPR diagnostic — %s\nengine %s\n", date, E2A_VERSION);

  char *big = malloc(96 * 1024);
  if (!big) { free(b.p); return -1; }
  const size_t bigcap = 96 * 1024;

  section(&b, "runtime");
  if (job_runtime_json(big, bigcap) >= 0) tb_add(&b, "%s\n", big);

  section(&b, "state");
  job_status s;
  job_get_status(&s);
  static const char *names[] = {"idle", "running", "done", "error", "cancelled", "paused"};
  tb_add(&b, "state %s, phase %s%s\nmessage: %s\ncode: %s, reason: %s\ntitle: %s [%s] -> %s\nprofile: %s\n"
             "percent %d, bytes %llu / %llu, speed %.0f B/s, eta %lld s\nsource %llu, packed %llu, shared %llu\n"
             "started %llu, finished %llu\n",
         names[s.state], s.phase, s.waiting ? " (waiting for ShadowMountPlus)" : "", s.message, s.code, s.reason, s.title, s.title_id,
         s.dest, s.profile, s.percent, (unsigned long long)s.bytes_done, (unsigned long long)s.bytes_total, s.speed_bps,
         (long long)s.eta_seconds, (unsigned long long)s.source_bytes, (unsigned long long)s.packed_bytes,
         (unsigned long long)s.shared_bytes, (unsigned long long)s.started, (unsigned long long)s.finished);
  for (int i = 0; i < s.queue_len; i++) tb_add(&b, "queued: %s\n", s.queue[i]);

  section(&b, "storages");
  if (job_list_destinations(big, bigcap) >= 0) tb_add(&b, "%s\n", big);
  section(&b, "sources");
  if (job_list_sources(big, bigcap) >= 0) tb_add(&b, "%s\n", big);
  section(&b, "resumable");
  if (job_list_resumable(big, bigcap) >= 0) tb_add(&b, "%s\n", big);
  section(&b, "plugins");
  if (plugins_info_json(big, bigcap) >= 0) tb_add(&b, "%s\n", big);

  section(&b, "settings");
  {
    char *doc = NULL;
    if (e2a_read_text(E2A_STATE_DIR "/settings.ini", &doc, NULL, 4096) == 0 && doc) { tb_add(&b, "%s\n", doc); free(doc); }
    else tb_add(&b, "(none)\n");
  }

  section(&b, "engine log (memory)");
  e2a_log_tail_copy(big, bigcap);
  tb_text(&b, big, strlen(big));
  section(&b, "engine log file " E2A_LOG_FILE);
  tb_file_tail(&b, E2A_LOG_FILE, 64 * 1024);

  section(&b, "app log (memory)");
  e2a_applog_tail_copy(big, bigcap);
  tb_text(&b, big, strlen(big));
  section(&b, "app log file " E2A_APPLOG_FILE);
  tb_file_tail(&b, E2A_APPLOG_FILE, 64 * 1024);
  free(big);

  if (!b.p) return -1;
  *out = b.p;
  *len = b.n;
  return 0;
}

int diag_write(char *paths, size_t pcap, char *err, size_t ecap) {
  char *text = NULL;
  size_t len = 0;
  paths[0] = 0;
  if (diag_build(&text, &len) != 0) { snprintf(err, ecap, "out of memory"); return -1; }
  char name[80];
  time_t t = time(NULL);
  struct tm tm;
  localtime_r(&t, &tm);
  strftime(name, sizeof name, "exfat2ampr-diagnostic-%Y%m%d-%H%M%S.txt", &tm);
  int written = 0;
  char path[700];
  e2a_mkdir_p(E2A_HOMEBREW_DIR);
  snprintf(path, sizeof path, "%s/%s", E2A_HOMEBREW_DIR, name);
  if (e2a_write_file_atomic(path, text, len) == 0) { snprintf(paths, pcap, "%s", path); written++; }
  else snprintf(err, ecap, "%s: %s", path, strerror(errno));
  /* a USB or extended drive too: easy to take to a PC */
  e2a_storage stg[E2A_MAX_STORAGES];
  int n = storage_scan(stg, E2A_MAX_STORAGES, false);
  for (int i = 0; i < n; i++) {
    if (strcmp(stg[i].kind, "internal") == 0 || !stg[i].writable) continue;
    snprintf(path, sizeof path, "%.300s/%s", stg[i].base, name);
    if (e2a_write_file_atomic(path, text, len) == 0) {
      size_t have = strlen(paths);
      snprintf(paths + have, pcap > have ? pcap - have : 0, "%s%s", have ? ", " : "", path);
      written++;
      break;
    }
  }
  free(text);
  if (written) e2a_log("diagnostic written: %s", paths);
  return written ? 0 : -1;
}
