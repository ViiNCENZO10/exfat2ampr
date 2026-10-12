/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE. */
#include "common.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------- notify -- */
#ifndef E2A_HOST
typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
#endif

void e2a_notify(const char *fmt, ...) {
  char message[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(message, sizeof message, fmt, ap);
  va_end(ap);
#ifndef E2A_HOST
  notify_request_t req;
  memset(&req, 0, sizeof req);
  snprintf(req.message, sizeof req.message, "%s", message);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
#endif
  e2a_log("[notify] %s", message);
}

/* ------------------------------------------------------------------- log -- */
#define LOG_LINES 256
#define LOG_LINE_LEN 256
typedef struct { char lines[LOG_LINES][LOG_LINE_LEN]; int head, count; uint64_t seq; } logring;
static logring g_log;      /* the engine's own lines */
static logring g_applog;   /* the home-screen app's lines, delivered over the local API: they outlive the app */
static pthread_mutex_t g_log_mx = PTHREAD_MUTEX_INITIALIZER;

static void ring_push(logring *r, const char *line) {   /* under g_log_mx */
  snprintf(r->lines[r->head], LOG_LINE_LEN, "%.255s", line);
  r->head = (r->head + 1) % LOG_LINES;
  if (r->count < LOG_LINES) r->count++;
  r->seq++;
}

/* JSON array of the lines newer than `since` (as many as fit); returns the newest sequence number, so the
 * next call asks for what came after */
static uint64_t ring_since(logring *r, uint64_t since, char *out, size_t cap) {
  size_t pos = 0;
  if (cap < 3) { if (cap) out[0] = 0; return 0; }
  out[pos++] = '[';
  pthread_mutex_lock(&g_log_mx);
  uint64_t newest = r->seq, oldest = newest - (uint64_t)r->count;   /* the lines (oldest, newest] are kept */
  uint64_t skip = since > oldest ? since : oldest;
  int start = (r->head - r->count + LOG_LINES) % LOG_LINES;
  bool any = false;
  for (uint64_t s = skip; s < newest; s++) {
    char esc[LOG_LINE_LEN * 6 + 8];
    e2a_json_escape(r->lines[(start + (int)(s - oldest)) % LOG_LINES], esc, sizeof esc);
    size_t n = strlen(esc);
    if (pos + n + 5 > cap) break;
    if (any) out[pos++] = ',';
    out[pos++] = '"';
    memcpy(out + pos, esc, n);
    pos += n;
    out[pos++] = '"';
    any = true;
  }
  pthread_mutex_unlock(&g_log_mx);
  out[pos++] = ']';
  out[pos] = 0;
  return newest;
}

void e2a_log(const char *fmt, ...) {
  char line[LOG_LINE_LEN];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);

  pthread_mutex_lock(&g_log_mx);
  ring_push(&g_log, line);
  pthread_mutex_unlock(&g_log_mx);

  fprintf(stderr, "[exfat2ampr] %s\n", line);
  FILE *f = fopen(E2A_LOG_FILE, "a");
  if (f) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d %s\n", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec, line);
    fclose(f);
  }
}

/* copy of the newest log lines that fit, taken under the log mutex (safe from any thread) */
void e2a_log_tail_copy(char *out, size_t cap) {
  pthread_mutex_lock(&g_log_mx);
  if (cap) out[0] = 0;
  int start = (g_log.head - g_log.count + LOG_LINES) % LOG_LINES, first = g_log.count;
  size_t need = 0;
  for (int i = g_log.count - 1; i >= 0; i--) {   /* from the newest back: how many fit */
    size_t n = strlen(g_log.lines[(start + i) % LOG_LINES]) + 1;
    if (need + n + 1 > cap) break;
    need += n;
    first = i;
  }
  size_t pos = 0;
  for (int i = first; i < g_log.count; i++) {
    const char *l = g_log.lines[(start + i) % LOG_LINES];
    size_t n = strlen(l);
    memcpy(out + pos, l, n);
    pos += n;
    out[pos++] = '\n';
    out[pos] = 0;
  }
  pthread_mutex_unlock(&g_log_mx);
}

uint64_t e2a_log_since(uint64_t since, char *out, size_t cap) { return ring_since(&g_log, since, out, cap); }
uint64_t e2a_applog_since(uint64_t since, char *out, size_t cap) { return ring_since(&g_applog, since, out, cap); }

/* lines from the home-screen app ('\n' separated), stamped with the engine's clock, kept in memory and
 * appended to E2A_APPLOG_FILE (rotated past a megabyte: the previous one stays as .old) */
void e2a_applog_add(const char *text, size_t len) {
  char ts[16];
  time_t t = time(NULL);
  struct tm tm;
  localtime_r(&t, &tm);
  strftime(ts, sizeof ts, "%H:%M:%S", &tm);
  struct stat st;
  if (stat(E2A_APPLOG_FILE, &st) == 0 && st.st_size > (1 << 20)) rename(E2A_APPLOG_FILE, E2A_APPLOG_FILE ".old");
  FILE *f = fopen(E2A_APPLOG_FILE, "a");
  size_t i = 0;
  while (i < len) {
    size_t e = i;
    while (e < len && text[e] != '\n') e++;
    size_t n = e - i;
    while (n && (text[i + n - 1] == '\r' || text[i + n - 1] == ' ')) n--;
    if (n) {
      char line[LOG_LINE_LEN];
      snprintf(line, sizeof line, "%.15s %.*s", ts, (int)(n < 238 ? n : 238), text + i);
      pthread_mutex_lock(&g_log_mx);
      ring_push(&g_applog, line);
      pthread_mutex_unlock(&g_log_mx);
      if (f) fprintf(f, "%04d-%02d-%02d %s\n", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, line);
#ifdef E2A_HOST
      fprintf(stderr, "[app] %s\n", line);
#endif
    }
    i = e + 1;
  }
  if (f) fclose(f);
}

void e2a_json_escape(const char *in, char *out, size_t cap) {
  size_t o = 0;
  for (; *in && o + 7 < cap; in++) {
    unsigned char c = (unsigned char)*in;
    if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
    else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
    else if (c < 0x20) { o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c); }
    else out[o++] = (char)c;
  }
  out[o] = 0;
}

double e2a_now(void) { struct timeval tv; gettimeofday(&tv, NULL); return (double)tv.tv_sec + tv.tv_usec / 1e6; }

/* ------------------------------------------------------------------ files -- */
int e2a_is_dir(const char *path) { struct stat st; return stat(path, &st) == 0 && S_ISDIR(st.st_mode); }
int e2a_is_file(const char *path) { struct stat st; return stat(path, &st) == 0 && S_ISREG(st.st_mode); }
int64_t e2a_file_size(const char *path) { struct stat st; return stat(path, &st) == 0 ? (int64_t)st.st_size : -1; }

int e2a_mkdir_p(const char *path) {
  char tmp[1024];
  size_t n = strlen(path);
  if (n >= sizeof tmp) return -1;
  memcpy(tmp, path, n + 1);
  for (size_t i = 1; i < n; i++) {
    if (tmp[i] == '/') {
      tmp[i] = 0;
      if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
      tmp[i] = '/';
    }
  }
  if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
  return 0;
}

int e2a_rm_rf(const char *path) {
  DIR *d = opendir(path);
  if (!d) return unlink(path) == 0 ? 0 : -1;
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
    char child[1100];
    snprintf(child, sizeof child, "%s/%s", path, de->d_name);
    struct stat st;
    if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) e2a_rm_rf(child); else unlink(child);
  }
  closedir(d);
  return rmdir(path);
}

int e2a_write_all(int fd, const void *buf, size_t len) {
  const uint8_t *p = buf;
  while (len) {
    ssize_t w = write(fd, p, len);
    if (w < 0) { if (errno == EINTR) continue; return -1; }
    p += w; len -= (size_t)w;
  }
  return 0;
}

int e2a_pwrite_all(int fd, const void *buf, size_t len, uint64_t off) {
  const uint8_t *p = buf;
  while (len) {
    ssize_t w = pwrite(fd, p, len, (off_t)off);
    if (w < 0) { if (errno == EINTR) continue; return -1; }
    p += w; len -= (size_t)w; off += (uint64_t)w;
  }
  return 0;
}

int e2a_pread_all(int fd, void *buf, size_t len, uint64_t off) {
  uint8_t *p = buf;
  while (len) {
    ssize_t r = pread(fd, p, len, (off_t)off);
    if (r < 0) { if (errno == EINTR) continue; return -1; }
    if (r == 0) return -1;
    p += r; len -= (size_t)r; off += (uint64_t)r;
  }
  return 0;
}

/* one range read by several threads at once (pread, no shared cursor). A compressed image mounted by
 * ShadowMountPlus (pfsc) decodes every request on the thread that asks: one reader waits on the decoder
 * between two blocks, four of them keep it busy. On a plain drive the requests are simply queued. */
typedef struct { int fd; uint8_t *dst; uint64_t off; size_t len; int err; } pseg;
static void *pseg_fn(void *a) {
  pseg *s = a;
  errno = 0;
  if (e2a_pread_all(s->fd, s->dst, s->len, s->off) != 0) s->err = errno ? errno : EIO;
  return NULL;
}
int e2a_pread_parallel(int fd, void *buf, size_t len, uint64_t off, int threads) {
  if (threads < 1) threads = 1;
  if (threads > 8) threads = 8;
  size_t seg = (len / (size_t)threads + 65535) & ~(size_t)65535;   /* 64 KiB multiples: whole blocks per thread */
  if (seg == 0 || len < 2u << 20) threads = 1;
  if (threads == 1) return e2a_pread_all(fd, buf, len, off);
  pseg s[8];
  pthread_t th[8];
  int n = 0, err = 0;
  for (size_t done = 0; done < len && n < threads; n++) {
    size_t l = len - done < seg ? len - done : seg;
    s[n].fd = fd; s[n].dst = (uint8_t *)buf + done; s[n].off = off + done; s[n].len = l; s[n].err = 0;
    done += l;
    if (n == threads - 1 && done < len) s[n].len += len - done, done = len;   /* rounding: the last one takes the rest */
    if (pthread_create(&th[n], NULL, pseg_fn, &s[n]) != 0) { pseg_fn(&s[n]); th[n] = 0; }
  }
  for (int i = 0; i < n; i++) {
    if (th[i]) pthread_join(th[i], NULL);
    if (s[i].err && !err) err = s[i].err;
  }
  if (err) { errno = err; return -1; }
  return 0;
}

int e2a_read_full(int fd, void *buf, size_t len, size_t *got) {
  uint8_t *p = buf;
  size_t n = 0;
  while (n < len) {
    ssize_t r = read(fd, p + n, len - n);
    if (r < 0) { if (errno == EINTR) continue; return -1; }
    if (r == 0) break;
    n += (size_t)r;
  }
  *got = n;
  return 0;
}

int e2a_read_text(const char *path, char **out, size_t *len, size_t max) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  char *buf = malloc(max + 1);
  size_t got = 0;
  if (!buf || e2a_read_full(fd, buf, max, &got) != 0) { close(fd); free(buf); return -1; }
  close(fd);
  buf[got] = 0;
  *out = buf;
  if (len) *len = got;
  return 0;
}

int e2a_write_file_atomic(const char *path, const void *data, size_t len) {
  char tmp[1100];
  snprintf(tmp, sizeof tmp, "%s.e2a-tmp", path);
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0) return -1;
  int rc = e2a_write_all(fd, data, len);
  if (fsync(fd) != 0) rc = -1;
  if (close(fd) != 0) rc = -1;
  if (rc == 0 && rename(tmp, path) != 0) rc = -1;
  if (rc != 0) unlink(tmp);
  return rc;
}

/* Thread-safe (per-call buffer): called from the job thread and from the HTTP thread. */
void (*e2a_copy_hook)(uint64_t bytes);

int e2a_copy_file(const char *src, const char *dst, int64_t mtime) {
  const size_t BUF = 16u << 20;   /* read by four threads at once, written in one piece */
  uint8_t *buf = malloc(BUF);
  if (!buf) return -1;
  int in = open(src, O_RDONLY);
  if (in < 0) { int e = errno; free(buf); errno = e; return -1; }
  char tmp[1100];
  snprintf(tmp, sizeof tmp, "%s.e2a-tmp", dst);
  int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (out < 0) { int e = errno; close(in); free(buf); errno = e; return -1; }
  int rc = 0, saved = 0;
  struct stat st;
  uint64_t size = fstat(in, &st) == 0 ? (uint64_t)st.st_size : 0, pos = 0;
  while (pos < size) {
    size_t n = size - pos < BUF ? (size_t)(size - pos) : BUF;
    if (e2a_pread_parallel(in, buf, n, pos, E2A_READ_THREADS) != 0) { saved = errno; rc = -1; break; }
    if (e2a_write_all(out, buf, n) != 0) { saved = errno; rc = -1; break; }
    pos += n;
    if (e2a_copy_hook) e2a_copy_hook(n);
  }
  if (rc == 0 && lseek(in, (off_t)pos, SEEK_SET) < 0) { saved = errno; rc = -1; }   /* pread left the cursor at 0 */
  for (; rc == 0;) {   /* a file that grew meanwhile: the rest, the old way */
    ssize_t r = read(in, buf, BUF);
    if (r < 0) { if (errno == EINTR) continue; saved = errno; rc = -1; break; }
    if (r == 0) break;
    if (e2a_write_all(out, buf, (size_t)r) != 0) { saved = errno; rc = -1; break; }
    if (e2a_copy_hook) e2a_copy_hook((uint64_t)r);
  }
  close(in);
  if (close(out) != 0 && rc == 0) { saved = errno; rc = -1; }
  if (rc == 0) {
    struct timeval tv[2] = {{(time_t)mtime, 0}, {(time_t)mtime, 0}};
    utimes(tmp, tv);
    if (rename(tmp, dst) != 0) { saved = errno; rc = -1; }
  }
  free(buf);
  if (rc != 0) { unlink(tmp); errno = saved; }
  return rc;
}

/* ------------------------------------------------------------------ hash -- */
static uint32_t g_crc_table[8][256];   /* slicing-by-8: table t folds a byte t positions ahead */
static pthread_once_t g_crc_once = PTHREAD_ONCE_INIT;

static void crc_init(void) {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    g_crc_table[0][i] = c;
  }
  for (uint32_t i = 0; i < 256; i++)
    for (int t = 1; t < 8; t++) g_crc_table[t][i] = g_crc_table[0][g_crc_table[t - 1][i] & 0xFF] ^ (g_crc_table[t - 1][i] >> 8);
}

/* zlib-compatible CRC32: e2a_crc32(0, buf, len) == zlib.crc32(buf). Eight bytes per step (eight independent
 * table lookups instead of a chain of one lookup per byte): about four times faster on the packer's blocks. */
uint32_t e2a_crc32(uint32_t crc, const void *buf, size_t len) {
  pthread_once(&g_crc_once, crc_init);
  const uint8_t *p = buf;
  crc = ~crc;
  while (len >= 8) {
    uint32_t a = crc ^ ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
    uint32_t b = (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
    crc = g_crc_table[7][a & 0xFF] ^ g_crc_table[6][(a >> 8) & 0xFF] ^ g_crc_table[5][(a >> 16) & 0xFF] ^ g_crc_table[4][a >> 24] ^
          g_crc_table[3][b & 0xFF] ^ g_crc_table[2][(b >> 8) & 0xFF] ^ g_crc_table[1][(b >> 16) & 0xFF] ^ g_crc_table[0][b >> 24];
    p += 8;
    len -= 8;
  }
  while (len--) crc = g_crc_table[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void e2a_fold_ascii(char *s) {
  for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
}

/* FNV-1a 64 over ASCII-folded bytes; 0 maps to 1 (ampr_pack_format.asset_path_hash) */
uint64_t e2a_fnv1a64_fold(const void *buf, size_t len) {
  const uint8_t *p = buf;
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < len; i++) {
    uint8_t c = p[i];
    if (c >= 'A' && c <= 'Z') c = (uint8_t)(c - 'A' + 'a');
    h ^= c;
    h *= 0x100000001B3ull;
  }
  return h ? h : 1;
}

int e2a_sha256_file(const char *path, char hex[65]) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  e2a_sha256 c;
  e2a_sha256_init(&c);
  uint8_t *buf = malloc(1 << 20);
  if (!buf) { close(fd); return -1; }
  for (;;) {
    ssize_t r = read(fd, buf, 1 << 20);
    if (r < 0) { if (errno == EINTR) continue; free(buf); close(fd); return -1; }
    if (r == 0) break;
    e2a_sha256_update(&c, buf, (size_t)r);
  }
  free(buf);
  close(fd);
  uint8_t d[32];
  e2a_sha256_final(&c, d);
  for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", d[i]);
  return 0;
}
