/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE. */
#include "estimate.h"

#include "common.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lz4hc.h"

#define SIZES_FILE E2A_STATE_DIR "/sizes.txt"
#define VRATE_FILE E2A_STATE_DIR "/verify_rate.txt"
#define PRATE_FILE E2A_STATE_DIR "/pack_rate.txt"
#define SAMPLES 96
#define SBLOCK 65536u

static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;

/* the packer's rule (lz4_worth_it): a block is kept compressed only when it saves enough */
static uint32_t stored_size(uint32_t raw, int comp, int layout) {
  if (comp <= 0 || (uint32_t)comp >= raw) return raw;
  uint32_t saved = raw - (uint32_t)comp;
  double ratio = (double)saved / raw;
  uint32_t pages_raw = (raw + 65535) / 65536, pages_comp = ((uint32_t)comp + 65535) / 65536;
  bool strict = layout != E2A_LAYOUT_STREAMING && pages_comp >= pages_raw;
  bool worth = strict ? (saved >= 8192 && ratio >= 0.125) : (saved >= 64 && ratio >= 0.01);
  if (!worth) return raw;
  /* a small chunk never straddles an I/O page: on average it leaves s^2 / 2P behind it */
  return (uint32_t)comp + (uint32_t)((uint64_t)comp * (uint64_t)comp / (2ull * 65536));
}

/* ---------------------------------------------------------------- history -- */
typedef struct { char tid[16]; uint64_t content, image, final; } hist_row;

static int hist_load(hist_row *rows, int max) {
  char *txt = NULL;
  if (e2a_read_text(SIZES_FILE, &txt, NULL, 256 * 1024) != 0 || !txt) return 0;
  int n = 0;
  for (char *save = NULL, *line = strtok_r(txt, "\n", &save); line && n < max; line = strtok_r(NULL, "\n", &save)) {
    hist_row r;
    unsigned long long c, i, f;
    if (sscanf(line, "%15s %llu %llu %llu", r.tid, &c, &i, &f) == 4) { r.content = c; r.image = i; r.final = f; rows[n++] = r; }
  }
  free(txt);
  return n;
}

void est_remember(const char *tid, uint64_t content, uint64_t image, uint64_t final_bytes) {
  if (!tid || !tid[0] || !content) return;
  pthread_mutex_lock(&g_mx);
  static hist_row rows[512];
  int n = hist_load(rows, 512), k;
  for (k = 0; k < n && strcmp(rows[k].tid, tid) != 0; k++) {}
  if (k == n) {   /* a new title (the oldest one leaves when the table is full) */
    if (n == 512) { memmove(rows, rows + 1, sizeof rows[0] * 511); n--; }
    k = n++;
    memset(&rows[k], 0, sizeof rows[k]);
  }
  snprintf(rows[k].tid, sizeof rows[k].tid, "%s", tid);
  rows[k].content = content;
  if (image) rows[k].image = image;           /* what is not known now keeps its earlier value */
  if (final_bytes) rows[k].final = final_bytes;
  size_t cap = (size_t)n * 96 + 16, pos = 0;
  char *out = malloc(cap);
  if (out) {
    for (int i = 0; i < n; i++)
      pos += (size_t)snprintf(out + pos, cap - pos, "%s %llu %llu %llu\n", rows[i].tid, (unsigned long long)rows[i].content,
                              (unsigned long long)rows[i].image, (unsigned long long)rows[i].final);
    e2a_write_file_atomic(SIZES_FILE, out, pos);
    free(out);
  }
  pthread_mutex_unlock(&g_mx);
}

bool est_known_content(const char *tid, uint64_t *content, uint64_t *final_bytes) {
  static hist_row rows[512];
  pthread_mutex_lock(&g_mx);
  int n = hist_load(rows, 512);
  bool ok = false;
  for (int i = 0; i < n && !ok; i++)
    if (!strcmp(rows[i].tid, tid)) { *content = rows[i].content; *final_bytes = rows[i].final; ok = true; }
  pthread_mutex_unlock(&g_mx);
  return ok;
}

/* converted / content over every finished run (0.6 before the first) */
static double average_ratio(void) {
  static hist_row rows[512];
  pthread_mutex_lock(&g_mx);
  int n = hist_load(rows, 512);
  double c = 0, f = 0;
  for (int i = 0; i < n; i++) if (rows[i].final && rows[i].content) { c += (double)rows[i].content; f += (double)rows[i].final; }
  pthread_mutex_unlock(&g_mx);
  double r = c > 0 ? f / c : 0.6;
  return r < 0.2 ? 0.2 : r > 1.05 ? 1.05 : r;
}

void est_from_history(const char *tid, uint64_t image_bytes, bool compressed_image, e2a_estimate *out) {
  memset(out, 0, sizeof *out);
  uint64_t content = 0, final_bytes = 0;
  if (tid && tid[0] && est_known_content(tid, &content, &final_bytes) && content) {
    out->content = content;
    out->predicted = final_bytes ? final_bytes : (uint64_t)((double)content * average_ratio());
    out->method = "history";
  } else if (compressed_image) {
    /* a compressed image hides its real size: its own size is the nearest guess for the converted game */
    out->content = 0;
    out->predicted = image_bytes;
    out->method = "rough";
  } else {
    out->content = image_bytes;
    out->predicted = (uint64_t)((double)image_bytes * average_ratio());
    out->method = "average";
  }
  out->ratio = out->content ? (double)out->predicted / (double)out->content : 0;
}

/* ---------------------------------------------------------------- sampling -- */
int est_from_list(const char *root, const e2a_list *l, int level, e2a_estimate *out) {
  memset(out, 0, sizeof *out);
  uint64_t comp_total = 0, nfiles = 0;
  for (size_t i = 0; i < l->n; i++) {
    const e2a_entry *e = &l->v[i];
    out->content += e->size;
    if (e->action == E2A_LOOSE || e->size == 0) out->loose += e->size;
    else if (e->action == E2A_STORE) { comp_total += e->size; nfiles++; }
    else { comp_total += e->size; nfiles++; }
  }
  if (level < 1) level = 9;
  if (level > 12) level = 12;
  uint8_t *raw = malloc(SBLOCK), *dst = malloc((size_t)LZ4_compressBound((int)SBLOCK));
  if (!raw || !dst) { free(raw); free(dst); return -1; }
  double s_raw = 0, s_stored = 0;
  uint64_t step = comp_total / SAMPLES, acc = 0, next = step / 2;
  for (size_t i = 0; i < l->n && step; i++) {
    const e2a_entry *e = &l->v[i];
    if (e->action == E2A_LOOSE || e->size == 0) continue;
    uint64_t start = acc;
    acc += e->size;
    if (next >= acc) continue;
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", root, e->rel);
    int fd = open(path, O_RDONLY);
    if (fd < 0) { while (next < acc) next += step; continue; }
    while (next < acc) {
      uint64_t off = (next - start) & ~(uint64_t)(SBLOCK - 1);
      uint32_t len = e->size - off < SBLOCK ? (uint32_t)(e->size - off) : SBLOCK;
      if (e2a_pread_all(fd, raw, len, off) == 0) {
        uint32_t st = len;
        if (e->action != E2A_STORE) {
          int c = LZ4_compress_HC((const char *)raw, (char *)dst, (int)len, LZ4_compressBound((int)SBLOCK), level);
          st = stored_size(len, c, e->layout);
        }
        s_raw += len;
        s_stored += st;
      }
      next += step;
    }
    close(fd);
  }
  free(raw);
  free(dst);
  double r = s_raw > 0 ? s_stored / s_raw : average_ratio();
  /* each packed file starts on an I/O page (32 KiB lost on average), 16 bytes of index per block, the fakelib */
  double overhead = (double)nfiles * 32768.0 + (double)comp_total / 65536.0 * 16.0 + (2u << 20);
  out->predicted = out->loose + (uint64_t)((double)comp_total * r + overhead);
  out->ratio = out->content ? (double)out->predicted / (double)out->content : 0;
  out->method = "sampled";
  return 0;
}

int est_from_tree(const char *root, int level, e2a_estimate *out) {
  e2a_list l;
  if (e2a_scan_tree(root, &l, true) != 0) { e2a_list_free(&l); return -1; }
  e2a_blind_tune(&l);
  int rc = est_from_list(root, &l, level, out);
  e2a_list_free(&l);
  return rc;
}

/* ------------------------------------------------------------ verify speed -- */
double est_verify_rate(void) {
  char *txt = NULL;
  double v = 0;
  if (e2a_read_text(VRATE_FILE, &txt, NULL, 64) == 0 && txt) { v = atof(txt); free(txt); }
  return v > 0 ? v : 0;
}

void est_set_verify_rate(double bps) {
  if (bps <= 0) return;
  char b[48];
  int n = snprintf(b, sizeof b, "%.0f\n", bps);
  e2a_write_file_atomic(VRATE_FILE, b, (size_t)n);
}

/* -------------------------------------------------------------- pack speed -- */
/* bytes of game per second through the packing phase, per LZ4 level: what this console measured (one line
 * "<level> <bytes/s>" each), else a first guess. Levels 10 to 12 switch LZ4HC to its optimal parser, several
 * times slower for less than 1 % of space. */
static const double PACK_GUESS[13] = {0, 220e6, 220e6, 220e6, 190e6, 180e6, 170e6, 150e6, 130e6, 110e6, 35e6, 30e6, 25e6};

static void prate_load(double rates[13]) {
  memset(rates, 0, 13 * sizeof *rates);
  char *txt = NULL;
  if (e2a_read_text(PRATE_FILE, &txt, NULL, 4096) != 0 || !txt) return;
  for (char *save = NULL, *line = strtok_r(txt, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    int lv = 0;
    double v = 0;
    if (sscanf(line, "%d %lf", &lv, &v) == 2 && lv >= 1 && lv <= 12 && v > 0) rates[lv] = v;
  }
  free(txt);
}

double est_pack_rate(int level) {
  if (level < 1 || level > 12) level = 9;
  double rates[13];
  pthread_mutex_lock(&g_mx);
  prate_load(rates);
  pthread_mutex_unlock(&g_mx);
  if (rates[level] > 0) return rates[level];
  /* another level measured here: scaled by the guesses' ratio (this console's disks, not the guess's) */
  for (int d = 1; d <= 11; d++)
    for (int s = -1; s <= 1; s += 2) {
      int o = level + s * d;
      if (o >= 1 && o <= 12 && rates[o] > 0) return rates[o] * PACK_GUESS[level] / PACK_GUESS[o];
    }
  return PACK_GUESS[level];
}

void est_set_pack_rate(int level, double bps) {
  if (level < 1 || level > 12 || bps <= 0) return;
  double rates[13];
  pthread_mutex_lock(&g_mx);
  prate_load(rates);
  rates[level] = rates[level] > 0 ? (rates[level] + bps) / 2 : bps;   /* smoothed: one odd run moves it halfway */
  char b[400];
  size_t at = 0;
  for (int i = 1; i <= 12; i++)
    if (rates[i] > 0) at += (size_t)snprintf(b + at, sizeof b - at, "%d %.0f\n", i, rates[i]);
  e2a_write_file_atomic(PRATE_FILE, b, at);
  pthread_mutex_unlock(&g_mx);
}

int64_t est_duration(uint64_t content, int level, bool verify) {
  if (!content) return -1;
  double s = (double)content / est_pack_rate(level);
  if (verify) {
    double vr = est_verify_rate();
    s += (double)content / (vr > 0 ? vr : 400e6);
  }
  return (int64_t)(s + 0.5);
}
