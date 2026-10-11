/* SPDX-License-Identifier: GPL-3.0-or-later */
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
#define LOG_LINES 64
#define LOG_LINE_LEN 256
static char g_log[LOG_LINES][LOG_LINE_LEN];
static int g_log_head = 0, g_log_count = 0;
static pthread_mutex_t g_log_mx = PTHREAD_MUTEX_INITIALIZER;

void e2a_log(const char *fmt, ...) {
  char line[LOG_LINE_LEN];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);

  pthread_mutex_lock(&g_log_mx);
  memcpy(g_log[g_log_head], line, LOG_LINE_LEN);
  g_log_head = (g_log_head + 1) % LOG_LINES;
  if (g_log_count < LOG_LINES) g_log_count++;
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

/* copy of the last log lines, taken under the log mutex (safe from any thread) */
void e2a_log_tail_copy(char *out, size_t cap) {
  pthread_mutex_lock(&g_log_mx);
  size_t pos = 0;
  if (cap) out[0] = 0;
  int start = (g_log_head - g_log_count + LOG_LINES) % LOG_LINES;
  for (int i = 0; i < g_log_count && cap; i++) {
    const char *l = g_log[(start + i) % LOG_LINES];
    size_t n = strlen(l);
    if (pos + n + 2 >= cap) break;
    memcpy(out + pos, l, n);
    pos += n;
    out[pos++] = '\n';
    out[pos] = 0;
  }
  pthread_mutex_unlock(&g_log_mx);
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
  const size_t BUF = 4u << 20;
  uint8_t *buf = malloc(BUF);
  if (!buf) return -1;
  int in = open(src, O_RDONLY);
  if (in < 0) { int e = errno; free(buf); errno = e; return -1; }
  char tmp[1100];
  snprintf(tmp, sizeof tmp, "%s.e2a-tmp", dst);
  int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (out < 0) { int e = errno; close(in); free(buf); errno = e; return -1; }
  int rc = 0, saved = 0;
  for (;;) {
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
static uint32_t g_crc_table[256];
static pthread_once_t g_crc_once = PTHREAD_ONCE_INIT;

static void crc_init(void) {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    g_crc_table[i] = c;
  }
}

/* zlib-compatible CRC32: e2a_crc32(0, buf, len) == zlib.crc32(buf) */
uint32_t e2a_crc32(uint32_t crc, const void *buf, size_t len) {
  pthread_once(&g_crc_once, crc_init);
  const uint8_t *p = buf;
  crc = ~crc;
  while (len--) crc = g_crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
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
