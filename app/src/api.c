/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "api.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int send_all(int s, const void *buf, size_t len) {
  const uint8_t *p = buf;
  while (len) {
    int n = plat_tcp_send(s, p, len);
    if (n <= 0) return -1;
    p += n; len -= (size_t)n;
  }
  return 0;
}

/* one HTTP/1.0 exchange; returns status, body in *out (malloc'd, NUL-terminated) */
static int http_body(const char *method, const char *path, const void *data, size_t blen, const char *ctype, uint8_t **out,
                     size_t *outlen) {
  *out = NULL; *outlen = 0;
  int s = plat_tcp_connect(ENGINE_PORT, 1500);
  if (s < 0) return -1;
  char head[4096];
  int hn = data ? snprintf(head, sizeof head,
                           "%s %s HTTP/1.0\r\nHost: 127.0.0.1\r\nContent-Type: %s\r\n"
                           "Content-Length: %zu\r\nConnection: close\r\n\r\n", method, path, ctype, blen)
                : snprintf(head, sizeof head, "%s %s HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n", method, path);
  if (hn <= 0 || (size_t)hn >= sizeof head || send_all(s, head, (size_t)hn) || (blen && send_all(s, data, blen))) {
    plat_tcp_close(s);
    return -1;
  }
  size_t cap = 16384, n = 0;
  uint8_t *buf = malloc(cap + 1);
  while (buf) {
    if (n == cap) {
      if (cap >= (32u << 20)) break;
      uint8_t *nb = realloc(buf, cap * 2 + 1);
      if (!nb) { free(buf); buf = NULL; break; }
      buf = nb; cap *= 2;
    }
    int r = plat_tcp_recv(s, buf + n, cap - n);
    if (r <= 0) break;
    n += (size_t)r;
  }
  plat_tcp_close(s);
  if (!buf) return -1;
  buf[n] = 0;
  int status = -1;
  if (n > 12 && !strncmp((char *)buf, "HTTP/1.", 7)) status = atoi((char *)buf + 9);
  char *body = strstr((char *)buf, "\r\n\r\n");
  if (status < 0 || !body) { free(buf); return -1; }
  body += 4;
  size_t blen2 = n - (size_t)(body - (char *)buf);
  memmove(buf, body, blen2);
  buf[blen2] = 0;
  *out = buf; *outlen = blen2;
  return status;
}

static int http(const char *method, const char *path, const char *form, uint8_t **out, size_t *outlen) {
  return http_body(method, path, form, form ? strlen(form) : 0, "application/x-www-form-urlencoded", out, outlen);
}

int api_post_bytes(const char *path, const void *data, size_t len, jdoc *doc) {
  uint8_t *body;
  size_t blen;
  doc->root = NULL; doc->mem = NULL;
  int st = http_body("POST", path, data, len, "application/octet-stream", &body, &blen);
  if (st < 0) return st;
  if (json_parse((const char *)body, blen, doc) != 0) { doc->root = NULL; doc->mem = NULL; }
  free(body);
  return st;
}

int api_call(const char *path, const char *form, jdoc *doc) {
  uint8_t *body;
  size_t len;
  doc->root = NULL; doc->mem = NULL;
  int st = http(form ? "POST" : "GET", path, form ? form : NULL, &body, &len);
  if (st < 0) return st;
  if (json_parse((const char *)body, len, doc) != 0) { doc->root = NULL; doc->mem = NULL; }
  free(body);
  return st;
}

int api_get_raw(const char *path, uint8_t **data, size_t *len) {
  int st = http("GET", path, NULL, data, len);
  if (st != 200) { free(*data); *data = NULL; *len = 0; return -1; }
  return 0;
}

void form_add(char *buf, size_t cap, const char *key, const char *value) {
  size_t n = strlen(buf);
  if (n && n + 1 < cap) buf[n++] = '&';
  for (const char *k = key; *k && n + 1 < cap; k++) buf[n++] = *k;
  if (n + 1 < cap) buf[n++] = '=';
  static const char hex[] = "0123456789ABCDEF";
  for (const unsigned char *v = (const unsigned char *)value; *v && n + 4 < cap; v++) {
    if ((*v >= 'A' && *v <= 'Z') || (*v >= 'a' && *v <= 'z') || (*v >= '0' && *v <= '9') || *v == '-' || *v == '_' || *v == '.' ||
        *v == '/')
      buf[n++] = (char)*v;
    else { buf[n++] = '%'; buf[n++] = hex[*v >> 4]; buf[n++] = hex[*v & 15]; }
  }
  buf[n] = 0;
}

void form_add_int(char *buf, size_t cap, const char *key, long value) {
  char v[32];
  snprintf(v, sizeof v, "%ld", value);
  form_add(buf, cap, key, v);
}

static bool engine_version(char *out, size_t cap) {
  jdoc d;
  int st = api_call("/api/state", NULL, &d);
  bool ok = st == 200 && d.root;
  if (ok) snprintf(out, cap, "%s", json_str(d.root, "version", "?"));
  json_free(&d);
  return ok;
}

static bool engine_idle(void) {
  jdoc d;
  bool idle = false;
  if (api_call("/api/state", NULL, &d) == 200 && d.root) {
    const char *state = json_str(d.root, "state", "");
    const jnode *q = json_get(d.root, "queue");
    idle = strcmp(state, "running") != 0 && (!q || q->count == 0);
  }
  json_free(&d);
  return idle;
}

static engine_result send_elf(void) {
  uint8_t *elf;
  size_t len;
  char path[256];
  snprintf(path, sizeof path, "%s/exfat2ampr.elf", plat_asset_dir());
  if (plat_read_file(path, &elf, &len) != 0 || len < 64) return ENGINE_NO_ELF;
  int s = plat_tcp_connect(LOADER_PORT, 2000);
  if (s < 0) { free(elf); return ENGINE_NO_LOADER; }
  int rc = send_all(s, elf, len);
  plat_tcp_close(s);
  free(elf);
  return rc == 0 ? ENGINE_STARTED : ENGINE_NO_LOADER;
}

static bool wait_engine(char *running, size_t cap, unsigned ms) {
  for (uint64_t end = plat_ms() + ms; plat_ms() < end; plat_sleep_ms(250))
    if (engine_version(running, cap)) return true;
  return false;
}

engine_result engine_ensure(const char *want, char *running, size_t cap) {
  running[0] = 0;
  if (engine_version(running, cap)) {
    if (!want || !want[0] || !strcmp(running, want)) return ENGINE_OK;
    /* an older (or newer) engine answers: replace it only when it has nothing to do */
    if (!engine_idle()) return ENGINE_BUSY_OLD;
    jdoc d;
    int st = api_call("/api/quit", "", &d);
    json_free(&d);
    if (st != 200 && st >= 0) return ENGINE_BUSY_OLD;   /* engines older than /api/quit: keep it */
    for (int i = 0; i < 20 && engine_version(running, cap); i++) plat_sleep_ms(250);
    engine_result r = send_elf();
    if (r != ENGINE_STARTED) return r;
    return wait_engine(running, cap, 15000) ? ENGINE_UPDATED : ENGINE_TIMEOUT;
  }
  engine_result r = send_elf();
  if (r != ENGINE_STARTED) return r;
  return wait_engine(running, cap, 15000) ? ENGINE_STARTED : ENGINE_TIMEOUT;
}
