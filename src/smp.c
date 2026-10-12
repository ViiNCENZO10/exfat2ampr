/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * ShadowMountPlus API client. Contract (from the SMP sources, sm_api_service.c):
 *  - every JSON route is POST, Content-Type: application/json, Content-Length required,
 *    body = one JSON object (max 4096 bytes), one request per connection (Connection: close);
 *  - success: {"status":0,...}; error: {"status":<errno>,"error":"..."[,"error_reason":"..."]};
 *  - /api/v1/games/mount {"title_id","mode":"ro"} is synchronous and stays mounted until
 *    /api/v1/games/unmount (no timer; USB images are released on rest mode);
 *  - only one title can be prepared at a time, and not while a game is running (409, EBUSY; a
 *    title includes the exFAT2AMPR app itself). 1.7beta5 adds a stable "error_reason" code
 *    (game_active, runtime_prepared, ...) and must not be retried in a tight loop;
 *  - /api/v1/images lists backing images with mount_point/mounted (persistent mounts);
 *  - /api/v1/games/fakelib sets a title's fakelib policy (full, emulators, disabled). */
#include "smp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#define SMP_CONFIG "/data/shadowmount/config.ini"

static void smp_endpoint(char *ip, size_t cap, int *port) {
  snprintf(ip, cap, "127.0.0.1");
  *port = 10101;
  char *txt = NULL;
  if (e2a_read_text(SMP_CONFIG, &txt, NULL, 256 * 1024) != 0) return;
  char *line = txt, *next;
  while (line && *line) {
    next = strchr(line, '\n');
    if (next) *next++ = 0;
    while (*line == ' ' || *line == '\t') line++;
    char *eq = strchr(line, '=');
    if (line[0] != '#' && line[0] != ';' && eq) {
      *eq = 0;
      char *k = line, *v = eq + 1;
      size_t kl = strlen(k);
      while (kl && (k[kl - 1] == ' ' || k[kl - 1] == '\t')) k[--kl] = 0;
      while (*v == ' ' || *v == '\t') v++;
      size_t vl = strlen(v);
      while (vl && (v[vl - 1] == '\r' || v[vl - 1] == ' ' || v[vl - 1] == '\t')) v[--vl] = 0;
      if (strcasecmp(k, "api_port") == 0 && atoi(v) > 0 && atoi(v) < 65536) *port = atoi(v);
      if ((strcasecmp(k, "api_bind_address") == 0 || strcasecmp(k, "api_bind_adress") == 0) && v[0] &&
          strcmp(v, "0.0.0.0") != 0)
        snprintf(ip, cap, "%s", v);
    }
    line = next;
  }
  free(txt);
}

/* One HTTP/1.0 request (no chunked replies, server closes the connection); returns the HTTP status (or -1) and a NUL-terminated malloc'd body. */
static int smp_http(const char *method, const char *path, const char *json, char **body, size_t *body_len,
                    int timeout_s) {
  char ip[64];
  int port;
  smp_endpoint(ip, sizeof ip, &port);
  *body = NULL;
  if (body_len) *body_len = 0;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct timeval tv = {timeout_s, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, ip, &a.sin_addr) != 1 || connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }
  char head[512];
  size_t blen = json ? strlen(json) : 0;
  int n = json ? snprintf(head, sizeof head,
                          "%s %s HTTP/1.0\r\nHost: %s:%d\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                          method, path, ip, port, blen)
               : snprintf(head, sizeof head, "%s %s HTTP/1.0\r\nHost: %s:%d\r\nConnection: close\r\n\r\n", method, path, ip, port);
  if (e2a_write_all(fd, head, (size_t)n) != 0 || (blen && e2a_write_all(fd, json, blen) != 0)) { close(fd); return -1; }
  size_t cap = 64 * 1024, len = 0;
  char *buf = malloc(cap + 1);
  if (!buf) { close(fd); return -1; }
  for (;;) {
    if (len == cap) {
      if (cap >= (16u << 20)) break;
      char *nb = realloc(buf, cap * 2 + 1);
      if (!nb) break;
      buf = nb; cap *= 2;
    }
    ssize_t r = recv(fd, buf + len, cap - len, 0);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) break;
    len += (size_t)r;
  }
  close(fd);
  buf[len] = 0;
  int status = -1;
  if (len > 12 && strncmp(buf, "HTTP/1.", 7) == 0) status = atoi(buf + 9);
  char *hdr_end = strstr(buf, "\r\n\r\n");
  if (!hdr_end) { free(buf); return -1; }
  size_t off = (size_t)(hdr_end + 4 - buf);
  memmove(buf, buf + off, len - off + 1);
  *body = buf;
  if (body_len) *body_len = len - off;
  return status;
}

/* ---------------------------------------------------- tiny JSON helpers -- */
/* value of "key": "..." (or a bool/number as text) within [obj, end) at any depth: keys are unique enough */
static bool jstr(const char *obj, const char *end, const char *key, char *out, size_t cap) {
  char pat[64];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  size_t pl = strlen(pat);
  for (const char *p = obj; p && p + pl < end; p++) {
    p = strstr(p, pat);
    if (!p || p >= end) break;
    const char *q = p + pl;
    while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++;
    if (q >= end || *q != ':') continue;
    q++;
    while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++;
    size_t o = 0;
    if (*q == '"') {
      q++;
      while (q < end && *q != '"' && o + 1 < cap) {
        if (*q == '\\' && q + 1 < end) {
          q++;
          if (*q == 'u') { q++; for (int k = 0; k < 4 && q < end; k++) q++; out[o++] = '?'; continue; }
          out[o++] = *q == 'n' ? '\n' : *q == 't' ? '\t' : *q;
          q++;
          continue;
        }
        out[o++] = *q++;
      }
    } else {
      while (q < end && *q != ',' && *q != '}' && *q != ']' && o + 1 < cap) out[o++] = *q++;
      while (o && (out[o - 1] == ' ' || out[o - 1] == '\r' || out[o - 1] == '\n')) o--;
    }
    out[o] = 0;
    return true;
  }
  if (cap) out[0] = 0;
  return false;
}

static bool jbool(const char *obj, const char *end, const char *key) {
  char v[16];
  return jstr(obj, end, key, v, sizeof v) && strcmp(v, "true") == 0;
}

static int jstatus(const char *body) {
  char v[32];
  if (!body || !jstr(body, body + strlen(body), "status", v, sizeof v)) return -1;
  return atoi(v);
}

/* end of the JSON value starting at p ('{' or '['), string-aware */
static const char *jskip(const char *p) {
  int depth = 0;
  bool in = false;
  for (; *p; p++) {
    if (in) { if (*p == '\\' && p[1]) p++; else if (*p == '"') in = false; continue; }
    if (*p == '"') in = true;
    else if (*p == '{' || *p == '[') depth++;
    else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
  }
  return p;
}

static void parse_game(const char *o, const char *e, smp_game *g) {
  memset(g, 0, sizeof *g);
  jstr(o, e, "title_id", g->title_id, sizeof g->title_id);
  jstr(o, e, "title_name", g->title, sizeof g->title);
  jstr(o, e, "version", g->version, sizeof g->version);
  jstr(o, e, "path", g->path, sizeof g->path);
  jstr(o, e, "runtime_path", g->runtime_path, sizeof g->runtime_path);
  jstr(o, e, "source_type", g->source_type, sizeof g->source_type);
  jstr(o, e, "image_type", g->image_type, sizeof g->image_type);
  jstr(o, e, "fakelib_mode", g->fakelib_mode, sizeof g->fakelib_mode);
  /* identifiers go into JSON and HTTP request lines: keep [A-Za-z0-9._-] only */
  char *ids[] = {g->title_id, g->version, g->source_type, g->image_type, g->fakelib_mode};
  for (int f = 0; f < 5; f++) {
    char *w = ids[f];
    for (const char *r = ids[f]; *r; r++)
      if ((*r >= 'A' && *r <= 'Z') || (*r >= 'a' && *r <= 'z') || (*r >= '0' && *r <= '9') || *r == '.' || *r == '-' || *r == '_')
        *w++ = *r;
    *w = 0;
  }
  g->mounted = jbool(o, e, "mounted");
  g->managed = jbool(o, e, "managed");
  g->source_available = jbool(o, e, "source_available");
  char v[16];
  g->fakelib_enabled = !jstr(o, e, "fakelib_effective_enabled", v, sizeof v) || strcmp(v, "false") != 0;
}

/* English text for the log (the app and the page show their own words from the result and reason) */
static void err_from(const char *body, int http, char *errmsg, size_t errcap, char *reason_out, size_t rcap) {
  char e[200] = "", reason[64] = "";
  if (body) {
    const char *end = body + strlen(body);
    jstr(body, end, "error", e, sizeof e);
    jstr(body, end, "error_reason", reason, sizeof reason);
  }
  if (reason_out && rcap) {   /* a code: [a-z_] only */
    size_t o = 0;
    for (const char *r = reason; *r && o + 1 < rcap; r++)
      if ((*r >= 'a' && *r <= 'z') || *r == '_') reason_out[o++] = *r;
    reason_out[o] = 0;
  }
  if (http < 0) snprintf(errmsg, errcap, "ShadowMountPlus API unreachable (is ShadowMountPlus 1.7+ running?)");
  else if (http == 409) snprintf(errmsg, errcap, "ShadowMountPlus is busy (%s): close the running game or unmount the other game first", e[0] ? e : "EBUSY");
  else if (http == 404) snprintf(errmsg, errcap, "ShadowMountPlus does not manage this game yet (let it scan the image first)");
  else snprintf(errmsg, errcap, "ShadowMountPlus error %d: %s%s%s", http, e, reason[0] ? " / " : "", reason);
}

/* -------------------------------------------------------------- public -- */
bool smp_available(char *version, size_t cap) {
  char *body = NULL;
  int st = smp_http("POST", "/api/v1/version", "{}", &body, NULL, 3);
  bool ok = st == 200 && jstatus(body) == 0;
  if (version && cap) {
    version[0] = 0;
    if (ok) jstr(body, body + strlen(body), "shadowmount_version", version, cap);
  }
  free(body);
  return ok;
}

int smp_list_games(smp_game *out, int max) {
  char *body = NULL;
  int st = smp_http("POST", "/api/v1/games", "{}", &body, NULL, 10);
  if (st != 200 || jstatus(body) != 0) { free(body); return -1; }
  const char *arr = strstr(body, "\"games\"");
  int n = 0;
  if (arr) arr = strchr(arr, '[');
  if (arr) {
    const char *p = arr + 1;
    while (*p && n < max) {
      while (*p && *p != '{' && *p != ']') p++;
      if (*p != '{') break;
      const char *e = jskip(p);
      parse_game(p, e, &out[n]);
      if (out[n].title_id[0]) n++;
      p = e;
    }
  }
  free(body);
  return n;
}

int smp_game_info(const char *title_id, smp_game *g) {
  char req[96], *body = NULL;
  snprintf(req, sizeof req, "{\"title_id\":\"%.12s\"}", title_id);
  int st = smp_http("POST", "/api/v1/games/info", req, &body, NULL, 60);
  int rc = -1;
  if (st == 200 && jstatus(body) == 0) { parse_game(body, body + strlen(body), g); rc = 0; }
  free(body);
  return rc;
}

smp_result smp_mount(const char *title_id, char *runtime_path, size_t cap, char *reason, size_t rcap, char *errmsg, size_t errcap) {
  char req[96], *body = NULL;
  if (reason && rcap) reason[0] = 0;
  snprintf(req, sizeof req, "{\"title_id\":\"%.12s\",\"mode\":\"ro\"}", title_id);
  int st = smp_http("POST", "/api/v1/games/mount", req, &body, NULL, 180);
  if (st == 501) {  /* explicit mode refused (folder source): retry with the configured default */
    free(body);
    snprintf(req, sizeof req, "{\"title_id\":\"%.12s\"}", title_id);
    st = smp_http("POST", "/api/v1/games/mount", req, &body, NULL, 180);
  }
  if (st != 200 || jstatus(body) != 0) {
    err_from(body, st, errmsg, errcap, reason, rcap);
    free(body);
    return st < 0 ? SMP_UNREACHABLE : st == 409 ? SMP_BUSY : st == 404 ? SMP_NOT_MANAGED : SMP_FAILED;
  }
  free(body);
  smp_game g;
  if (smp_game_info(title_id, &g) != 0 || !g.runtime_path[0]) {
    char e2[256];   /* the caller only unmounts on success: give the image back to SMP here */
    smp_unmount(title_id, e2, sizeof e2);
    snprintf(errmsg, errcap, "mounted, but ShadowMountPlus did not report the game path");
    return SMP_NO_PATH;
  }
  snprintf(runtime_path, cap, "%s", g.runtime_path);
  e2a_log("SMP mounted %s at %s (%s)", title_id, g.runtime_path, g.image_type);
  return SMP_OK;
}

bool smp_image_mount_point(const char *image_path, char *mount_point, size_t cap) {
  char *body = NULL;
  int st = smp_http("POST", "/api/v1/images", "{}", &body, NULL, 10);
  bool found = false;
  if (st == 200 && jstatus(body) == 0) {
    const char *arr = strstr(body, "\"images\"");
    if (arr) arr = strchr(arr, '[');
    for (const char *p = arr ? arr + 1 : NULL; p && *p && !found;) {
      while (*p && *p != '{' && *p != ']') p++;
      if (*p != '{') break;
      const char *e = jskip(p);
      char path[600], mp[600];
      if (jstr(p, e, "path", path, sizeof path) && strcmp(path, image_path) == 0 && jbool(p, e, "mounted") &&
          jstr(p, e, "mount_point", mp, sizeof mp) && mp[0] == '/') {
        snprintf(mount_point, cap, "%s", mp);
        found = true;
      }
      p = e;
    }
  }
  free(body);
  return found;
}

/* The mounted images as ShadowMountPlus last listed them, for the pictures: a cover is asked for every game
 * on screen, so one request at most every 15 s (and a short wait), never one per cover. */
typedef struct { char path[600], mp[600]; } smp_mounted;
static pthread_mutex_t g_img_mx = PTHREAD_MUTEX_INITIALIZER;
static smp_mounted *g_imgs;
static int g_nimgs, g_imgs_cap;
static time_t g_imgs_at;

bool smp_image_mount_point_cached(const char *image_path, char *mount_point, size_t cap) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  pthread_mutex_lock(&g_img_mx);
  if (!g_imgs_at || ts.tv_sec - g_imgs_at >= 15) {
    g_imgs_at = ts.tv_sec ? ts.tv_sec : 1;   /* a failed request is not repeated at once either */
    g_nimgs = 0;
    char *body = NULL;
    int st = smp_http("POST", "/api/v1/images", "{}", &body, NULL, 4);
    if (st == 200 && jstatus(body) == 0) {
      const char *arr = strstr(body, "\"images\"");
      if (arr) arr = strchr(arr, '[');
      for (const char *p = arr ? arr + 1 : NULL; p && *p;) {
        while (*p && *p != '{' && *p != ']') p++;
        if (*p != '{') break;
        const char *e = jskip(p);
        char path[600], mp[600];
        if (jstr(p, e, "path", path, sizeof path) && jbool(p, e, "mounted") && jstr(p, e, "mount_point", mp, sizeof mp) && mp[0] == '/') {
          if (g_nimgs == g_imgs_cap) {
            int ncap = g_imgs_cap ? g_imgs_cap * 2 : 32;
            smp_mounted *n = ncap <= 1024 ? realloc(g_imgs, sizeof *n * (size_t)ncap) : NULL;
            if (!n) break;
            g_imgs = n;
            g_imgs_cap = ncap;
          }
          snprintf(g_imgs[g_nimgs].path, sizeof g_imgs[0].path, "%s", path);
          snprintf(g_imgs[g_nimgs].mp, sizeof g_imgs[0].mp, "%s", mp);
          g_nimgs++;
        }
        p = e;
      }
    }
    free(body);
  }
  bool found = false;
  for (int i = 0; i < g_nimgs && !found; i++)
    if (strcmp(g_imgs[i].path, image_path) == 0) {
      snprintf(mount_point, cap, "%s", g_imgs[i].mp);
      found = true;
    }
  pthread_mutex_unlock(&g_img_mx);
  return found;
}

int smp_set_fakelib(const char *title_id, const char *mode) {
  char req[96], *body = NULL;
  snprintf(req, sizeof req, "{\"title_id\":\"%.12s\",\"mode\":\"%.12s\"}", title_id, mode);
  int st = smp_http("POST", "/api/v1/games/fakelib", req, &body, NULL, 15);
  int rc = st == 200 && jstatus(body) == 0 ? 0 : -1;
  e2a_log("SMP fakelib of %s -> %s: %s", title_id, mode, rc == 0 ? "saved" : "refused");
  free(body);
  return rc;
}

int smp_unmount(const char *title_id, char *errmsg, size_t errcap) {
  char req[96], *body = NULL;
  snprintf(req, sizeof req, "{\"title_id\":\"%.12s\"}", title_id);
  int st = smp_http("POST", "/api/v1/games/unmount", req, &body, NULL, 120);
  int rc = 0;
  if (st != 200 || jstatus(body) != 0) {
    err_from(body, st, errmsg, errcap, NULL, 0);
    e2a_log("SMP unmount %s failed: %s", title_id, errmsg);
    rc = -1;
  } else {
    e2a_log("SMP unmounted %s", title_id);
  }
  free(body);
  return rc;
}

int smp_icon(const char *title_id, uint8_t **png, size_t *len) { return smp_icon_wait(title_id, png, len, 4); }   /* a cover: never worth a long wait */

int smp_icon_wait(const char *title_id, uint8_t **png, size_t *len, int timeout_s) {
  char path[96], *body = NULL;
  size_t n = 0;
  snprintf(path, sizeof path, "/api/v1/games/icon?title_id=%.12s", title_id);
  int st = smp_http("GET", path, NULL, &body, &n, timeout_s);
  if (st != 200 || n < 8 || (unsigned char)body[0] != 0x89) { free(body); return -1; }
  *png = (uint8_t *)body;
  *len = n;
  return 0;
}
