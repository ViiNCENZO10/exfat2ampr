/* SPDX-License-Identifier: GPL-3.0-or-later
 * Tiny HTTP/1.0 server: serves the embedded web page and a JSON API. */
#include "common.h"
#include "job.h"
#include "profile.h"
#include "smp.h"
#include "trace.h"
#include "pair.h"
#include "activity.h"
#include "storage.h"
#include "bench.h"
#include "selfupdate.h"
#include "scan.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

extern unsigned char index_html[];
extern unsigned int index_html_len;

int httpd_listen(int port);
int httpd_serve(int srv, int port);
int httpd_run(int port);

#define REQ_MAX (2u << 20)

static void send_response(int fd, int code, const char *type, const char *body, size_t len) {
  char head[256];
  const char *txt = code == 200 ? "OK" : code == 404 ? "Not Found" : code == 409 ? "Conflict" : code == 413 ? "Payload Too Large" : code == 500 ? "Internal Server Error" : code == 503 ? "Service Unavailable" : "Bad Request";
  int n = snprintf(head, sizeof head, "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                   code, txt, type, len);
  e2a_write_all(fd, head, (size_t)n);
  if (len) e2a_write_all(fd, body, len);
}

static void send_json(int fd, int code, const char *json) { send_response(fd, code, "application/json", json, strlen(json)); }

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* value of key in an application/x-www-form-urlencoded body (or query string); returns length or -1 */
static int form_get(const char *body, const char *key, char *out, size_t cap) {
  size_t klen = strlen(key);
  const char *p = body;
  while (p && *p) {
    if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
      p += klen + 1;
      size_t o = 0;
      while (*p && *p != '&' && o + 1 < cap) {
        if (*p == '+') { out[o++] = ' '; p++; }
        else if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) { out[o++] = (char)(hexval(p[1]) * 16 + hexval(p[2])); p += 3; }
        else out[o++] = *p++;
      }
      out[o] = 0;
      return (int)o;
    }
    p = strchr(p, '&');
    if (p) p++;
  }
  out[0] = 0;
  return -1;
}

static bool flag(const char *body, const char *key, bool dflt) {
  char v[8];
  if (form_get(body, key, v, sizeof v) < 0) return dflt;
  return v[0] == '1' || v[0] == 't' || v[0] == 'o';
}

static void api_state(int fd, const char *args) {
  /* the home-screen app polls with app=<pid>: it is alive (and is not a game) */
  char appv[16];
  if (form_get(args, "app", appv, sizeof appv) > 0) activity_app_seen(atoi(appv));
  char games[4][16], game[16] = "";
  if (activity_running_titles(games, 4) > 0) { memcpy(game, games[0], sizeof game); game[sizeof game - 1] = 0; }
  job_status s;
  job_get_status(&s);
  static const char *names[] = {"idle", "running", "done", "error", "cancelled", "paused"};
  char msg[600], title[300], dest[1300], prof[700], log[LOG_JSON_CAP], runtime[1024], queue[E2A_QUEUE_MAX * 224 + 4];
  e2a_json_escape(s.message, msg, sizeof msg);
  e2a_json_escape(s.title, title, sizeof title);
  e2a_json_escape(s.dest, dest, sizeof dest);
  e2a_json_escape(s.profile, prof, sizeof prof);
  {
    char tail[64 * 256];   /* each connection runs on its own 512 KiB stack */
    e2a_log_tail_copy(tail, sizeof tail);
    e2a_json_escape(tail, log, sizeof log);
  }
  job_runtime_json(runtime, sizeof runtime);
  /* a failed or cancelled conversion can be started again; a checkpoint left behind can be resumed */
  char state_dir[700];
  snprintf(state_dir, sizeof state_dir, "%s/.e2a-state", s.dest);
  bool resumable = s.dest[0] && (s.state == JOB_PAUSED || s.state == JOB_ERROR) && e2a_is_dir(state_dir);
  char del[2000];
  job_delete_status_json(del, sizeof del);
  size_t qp = 0;
  queue[qp++] = '[';
  for (int i = 0; i < s.queue_len; i++) {
    char q[220];
    e2a_json_escape(s.queue[i], q, sizeof q);
    int w = snprintf(queue + qp, sizeof queue - qp - 2, "%s\"%s\"", i ? "," : "", q);
    if (w < 0 || (size_t)w >= sizeof queue - qp - 2) break;
    qp += (size_t)w;
  }
  queue[qp++] = ']'; queue[qp] = 0;
  const size_t cap = sizeof log + 8192;
  char *json = malloc(cap);
  if (!json) { send_json(fd, 500, "{}"); return; }
  snprintf(json, cap,
           "{\"version\":\"%s\",\"state\":\"%s\",\"phase\":\"%s\",\"message\":\"%s\",\"code\":\"%s\",\"reason\":\"%s\","
           "\"waiting\":%s,\"can_retry\":%s,\"resumable\":%s,%s,\"percent\":%d,"
           "\"bytes_total\":%llu,\"bytes_done\":%llu,\"speed_bps\":%.0f,\"eta_seconds\":%lld,"
           "\"title_id\":\"%s\",\"title\":\"%s\",\"dest\":\"%s\",\"profile\":\"%s\",\"source_bytes\":%llu,\"packed_bytes\":%llu,"
           "\"shared_bytes\":%llu,\"started\":%llu,\"finished\":%llu,\"queue\":%s,\"runtime\":%s,"
           "\"storage_rev\":%llu,\"game_running\":\"%s\",\"game_mode\":%s,\"game_mode_enabled\":%s,\"log\":\"%s\"}",
           E2A_VERSION, names[s.state], s.phase, msg, s.code, s.reason, s.waiting ? "true" : "false",
           job_can_retry() ? "true" : "false", resumable ? "true" : "false", del, s.percent,
           (unsigned long long)s.bytes_total, (unsigned long long)s.bytes_done, s.speed_bps, (long long)s.eta_seconds,
           s.title_id, title, dest, prof,
           (unsigned long long)s.source_bytes, (unsigned long long)s.packed_bytes, (unsigned long long)s.shared_bytes,
           (unsigned long long)s.started, (unsigned long long)s.finished, queue, runtime,
           (unsigned long long)activity_storage_rev(), game, activity_game_mode() ? "true" : "false",
           activity_game_mode_enabled() ? "true" : "false", log);
  send_json(fd, 200, json);
  free(json);
}

static void api_list(int fd, int (*fn)(char *, size_t)) {
  const size_t cap = 256 * 1024;
  char *json = malloc(cap);
  if (!json) { send_json(fd, 500, "[]"); return; }
  if (fn(json, cap) != 0) strcpy(json, "[]");
  send_json(fd, 200, json);
  free(json);
}

#ifdef E2A_HOST
static bool safe_path_host(const char *p) { return p[0] && !strstr(p, ".."); }
#define SAFE_PATH(p) safe_path_host(p)
#else
static bool safe_path(const char *p) { return p[0] == '/' && !strstr(p, ".."); }
#define SAFE_PATH(p) safe_path(p)
#endif

static void api_start(int fd, const char *body, bool queue_only) {
  job_request r;
  char v[64];
  memset(&r, 0, sizeof r);
  form_get(body, "path", r.source, sizeof r.source);
  form_get(body, "smp", r.smp_title, sizeof r.smp_title);
  if (r.smp_title[0]) {  /* PPSA12345 / CUSA12345 / LAPY12345 / FAKE12345 */
    bool ok = strlen(r.smp_title) == 9;
    for (int i = 0; ok && i < 9; i++)
      ok = i < 4 ? (r.smp_title[i] >= 'A' && r.smp_title[i] <= 'Z') : (r.smp_title[i] >= '0' && r.smp_title[i] <= '9');
    if (!ok) { send_json(fd, 400, "{\"error\":\"bad title id\",\"code\":\"bad_path\"}"); return; }
  }
  form_get(body, "dest", r.dest_base, sizeof r.dest_base);
  form_get(body, "name", r.name, sizeof r.name);
  r.level = form_get(body, "level", v, sizeof v) >= 0 ? atoi(v) : 9;
  r.workers = form_get(body, "workers", v, sizeof v) >= 0 ? atoi(v) : 6;
  r.clean_leftovers = flag(body, "clean", true);
  r.overwrite = flag(body, "overwrite", false);
  r.remove_source = flag(body, "remove", false);
  r.verify = flag(body, "verify", true);
  r.dedup = flag(body, "dedup", true);
  r.cache_mode = form_get(body, "cache", v, sizeof v) >= 0 && v[0] == '1' ? 1 : 0;
  r.force = flag(body, "force", false);
  if (!r.source[0] || !SAFE_PATH(r.source)) { send_json(fd, 400, "{\"error\":\"bad path\",\"code\":\"bad_path\"}"); return; }
  if (r.dest_base[0] && !SAFE_PATH(r.dest_base)) { send_json(fd, 400, "{\"error\":\"bad destination\",\"code\":\"dest_unavailable\"}"); return; }
#ifndef E2A_HOST
  /* refuse at once anything that is not a connected, writable storage (the job checks again) */
  if (!storage_dest_allowed(r.dest_base[0] ? r.dest_base : E2A_HOMEBREW_DIR)) {
    send_json(fd, 400, "{\"error\":\"destination storage not available\",\"code\":\"dest_unavailable\"}");
    return;
  }
#endif
  if (strchr(r.name, '/') || strstr(r.name, "..")) { send_json(fd, 400, "{\"error\":\"bad name\",\"code\":\"bad_name\"}"); return; }
  (void)queue_only;
  int pos = job_enqueue(&r);
  if (pos < 0) { send_json(fd, 409, "{\"error\":\"queue full\",\"code\":\"queue_full\"}"); return; }
  char out[64];
  snprintf(out, sizeof out, "{\"ok\":true,\"position\":%d}", pos);
  send_json(fd, 200, out);
}

/* GET /api/icon?path=<game folder or image>&title_id=<id>[&kind=bg]: the game's icon0.png (kind=bg:
 * its key art, pic0.png or pic1.png), from the first place that has it: the folder, the image while
 * ShadowMountPlus keeps it mounted, ShadowMountPlus, then the system's app metadata of the title.
 * Duplicates and images ShadowMountPlus does not manage get their pictures too. */
static void send_png(int fd, const void *img, size_t len) {
  char head[256];
  int n = snprintf(head, sizeof head, "HTTP/1.0 200 OK\r\nContent-Type: image/png\r\nContent-Length: %zu\r\nCache-Control: max-age=3600\r\nConnection: close\r\n\r\n", len);
  e2a_write_all(fd, head, (size_t)n);
  e2a_write_all(fd, img, len);
}

/* a PNG file sent as it is; false when it is missing, too big or not a PNG */
static bool send_png_file(int fd, const char *path, int64_t max) {
  int64_t size = e2a_file_size(path);
  if (size <= 8 || size > max) return false;
  char *img = NULL;
  size_t len = 0;
  if (e2a_read_text(path, &img, &len, (size_t)size) != 0) return false;
  bool png = len > 8 && (unsigned char)img[0] == 0x89 && img[1] == 'P' && img[2] == 'N' && img[3] == 'G';
  if (png) send_png(fd, img, len);
  free(img);
  return png;
}

/* a title ID that can go into the SMP request line and a path: 4 capitals and 5 digits (PPSA12345) */
static bool title_id_ok(const char *t) {
  bool ok = strlen(t) == 9;
  for (int i = 0; ok && i < 9; i++) ok = i < 4 ? (t[i] >= 'A' && t[i] <= 'Z') : (t[i] >= '0' && t[i] <= '9');
  return ok;
}

/* where the console keeps the pictures of every title it registered (games, apps, both storages) */
#ifdef E2A_HOST
static const char *const APPMETA_DIRS[] = {E2A_APPMETA_DIR, NULL};
#else
static const char *const APPMETA_DIRS[] = {E2A_APPMETA_DIR, E2A_APPMETA_DIR "/external", "/system_data/priv/appmeta",
                                           "/system_data/priv/appmeta/external", NULL};
#endif

/* the first of names found in dir as a PNG */
static bool send_from(int fd, const char *dir, const char *const *names, int64_t max) {
  char path[800];
  for (int i = 0; names[i]; i++) {
    snprintf(path, sizeof path, "%s/%s", dir, names[i]);
    if (send_png_file(fd, path, max)) return true;
  }
  return false;
}

static void api_icon(int fd, const char *args) {
  char root[600] = "", path[800], tid[16] = "", kind[8] = "", mp[600] = "";
  form_get(args, "kind", kind, sizeof kind);
  form_get(args, "path", root, sizeof root);
  bool bg = strcmp(kind, "bg") == 0;
  if (root[0] && !SAFE_PATH(root)) root[0] = 0;   /* not a path to read: the title ID still finds the pictures */
  if (form_get(args, "title_id", tid, sizeof tid) >= 0 && !title_id_ok(tid)) tid[0] = 0;
  if (!root[0] && !tid[0]) { send_json(fd, 400, "{\"error\":\"bad path\"}"); return; }
  const char *base = root[0] ? strrchr(root, '/') : NULL;
  base = base ? base + 1 : root;
  bool folder = root[0] && e2a_is_dir(root), image = !folder && root[0] && e2a_is_file(root);
  e2a_game g;   /* the folder's own title ID, for the console's copy of its pictures */
  if (folder && !tid[0] && e2a_read_game(root, &g) == 0) snprintf(tid, sizeof tid, "%s", g.title_id);
  if (!tid[0] && root[0]) e2a_title_from_name(base, tid);   /* "Game [PPSA12345].exfat", "PPSA12345-app0" */
  if (!title_id_ok(tid)) tid[0] = 0;
  static const char *const icons[] = {"sce_sys/icon0.png", "sce_sys/icon0_00.png", "icon0.png", NULL};
  static const char *const art[] = {"sce_sys/pic0.png", "sce_sys/pic1.png", NULL};
  static const char *const meta_icon[] = {"icon0.png", "icon0_00.png", NULL};
  static const char *const meta_art[] = {"pic0.png", "pic1.png", NULL};
  static const char *const wide[] = {"sce_sys/pic0.png", "sce_sys/pic1.png", NULL};
  const char *const *mine = bg ? art : icons, *const *meta = bg ? meta_art : meta_icon;
  const int64_t max = bg ? (24 << 20) : (8 << 20);   /* a 4K key art can weigh a dozen MB */
  /* fastest first, every game and app gets its picture: the folder itself; the copy the console keeps of
   * every title it registered (local files, instant); an image ShadowMountPlus keeps mounted (its list,
   * kept 15 s); ShadowMountPlus' icon; at last a folder's key art used as an icon */
  if (folder && send_from(fd, root, mine, max)) return;
  for (int m = 0; tid[0] && APPMETA_DIRS[m]; m++) {
    snprintf(path, sizeof path, "%s/%s", APPMETA_DIRS[m], tid);
    if (send_from(fd, path, meta, max)) return;
  }
  if (image && smp_image_mount_point_cached(root, mp, sizeof mp) && send_from(fd, mp, mine, max)) return;
  if (!bg && tid[0]) {
    uint8_t *png = NULL;
    size_t plen = 0;
    if (smp_icon(tid, &png, &plen) == 0) { send_png(fd, png, plen); free(png); return; }
  }
  if (!bg && (folder || mp[0]) && send_from(fd, folder ? root : mp, wide, max)) return;
  send_json(fd, 404, "{\"error\":\"no icon\"}");
}

/* trace mode: enable / disable / collect on a game folder, generate a profile for a title */
static pthread_mutex_t g_trace_mx = PTHREAD_MUTEX_INITIALIZER;

/* POST /api/update/{begin,chunk,install}: the app's own update, from the console only (see selfupdate.h) */
static void api_update(int fd, const char *op, const char *query, const char *body, size_t blen) {
  char err[300] = "", out[900], esc[700], v[96] = "";
  int rc = -1;
  if (!strcmp(op, "begin")) {
    char sha[80] = "";
    form_get(body, "size", v, sizeof v);
    form_get(body, "sha256", sha, sizeof sha);
    rc = selfupdate_begin(strtoull(v, NULL, 10), sha, err, sizeof err);
  } else if (!strcmp(op, "chunk")) {
    if (query) form_get(query, "off", v, sizeof v);
    if (!v[0]) snprintf(err, sizeof err, "missing offset");
    else rc = selfupdate_chunk(strtoull(v, NULL, 10), body, blen, err, sizeof err);
  } else if (!strcmp(op, "install")) {
    char where[600];
    rc = selfupdate_install(where, sizeof where, err, sizeof err);
    if (rc == 0) {
      e2a_json_escape(where, esc, sizeof esc);
      snprintf(out, sizeof out, "{\"ok\":true,\"path\":\"%s\"}", esc);
      send_json(fd, 200, out);
      return;
    }
  } else snprintf(err, sizeof err, "unknown step");
  if (rc == 0) { send_json(fd, 200, "{\"ok\":true}"); return; }
  e2a_json_escape(err, esc, sizeof esc);
  snprintf(out, sizeof out, "{\"error\":\"%s\"}", esc);
  send_json(fd, 400, out);
}

/* POST /api/speedtest path=<destination>: read and write speed (a few seconds), kept with the destinations */
static pthread_mutex_t g_bench_mx = PTHREAD_MUTEX_INITIALIZER;
static void api_speedtest(int fd, const char *body) {
  char base[300] = "", err[300] = "", out[800], esc[700];
  form_get(body, "path", base, sizeof base);
  if (!base[0] || !storage_dest_allowed(base)) { send_json(fd, 400, "{\"error\":\"bad destination\"}"); return; }
  if (job_running()) { send_json(fd, 409, "{\"error\":\"a conversion is running\"}"); return; }
  if (pthread_mutex_trylock(&g_bench_mx) != 0) { send_json(fd, 409, "{\"error\":\"busy\"}"); return; }
  e2a_speed s;
  int rc = bench_run(base, &s, err, sizeof err);
  pthread_mutex_unlock(&g_bench_mx);
  if (rc != 0) {
    e2a_json_escape(err, esc, sizeof esc);
    snprintf(out, sizeof out, "{\"error\":\"%s\"}", esc);
    send_json(fd, 400, out);
    return;
  }
  snprintf(out, sizeof out, "{\"ok\":true,\"read_mbps\":%d,\"write_mbps\":%d}", s.read_mbps, s.write_mbps);
  send_json(fd, 200, out);
}

static void api_trace(int fd, const char *op, const char *body) {
  char path[600] = "", err[300] = "", out[1400];
  if (strcmp(op, "generate") != 0) {
    form_get(body, "path", path, sizeof path);
    if (!path[0] || !trace_game_allowed(path)) { send_json(fd, 400, "{\"error\":\"bad game folder\"}"); return; }
  }
  if (pthread_mutex_trylock(&g_trace_mx) != 0) { send_json(fd, 409, "{\"error\":\"busy\"}"); return; }
  int rc = -1;
  if (strcmp(op, "enable") == 0) {
    if (job_running()) snprintf(err, sizeof err, "a conversion is running");
    else rc = trace_enable(path, err, sizeof err);
  } else if (strcmp(op, "disable") == 0) {
    rc = trace_disable(path);
    if (rc != 0) snprintf(err, sizeof err, "fakelib2 was not created by exfat2ampr: left untouched");
  } else if (strcmp(op, "collect") == 0 || strcmp(op, "generate") == 0) {
    char tid[16] = "";
    if (path[0]) {
      e2a_game g;
      if (e2a_read_game(path, &g) == 0) snprintf(tid, sizeof tid, "%s", g.title_id);
    } else form_get(body, "title_id", tid, sizeof tid);
    bool ok = strlen(tid) == 9;
    for (int i = 0; ok && tid[i]; i++) ok = (tid[i] >= 'A' && tid[i] <= 'Z') || (tid[i] >= '0' && tid[i] <= '9');
    if (!ok) snprintf(err, sizeof err, "bad title id");
    else if (strcmp(op, "collect") == 0) {
      rc = trace_collect(path, tid, err, sizeof err);
      if (rc == 1) { snprintf(err, sizeof err, "%d", trace_count(tid)); rc = 0; }
      else if (rc == 0) rc = -1;   /* nothing new: err explains why */
    } else {
      rc = trace_generate(tid, err, sizeof err);
    }
  }
  pthread_mutex_unlock(&g_trace_mx);
  char esc[700];
  e2a_json_escape(err, esc, sizeof esc);
  if (rc == 0) snprintf(out, sizeof out, "{\"ok\":true,\"info\":\"%s\"}", esc);
  else snprintf(out, sizeof out, "{\"error\":\"%s\"}", esc[0] ? esc : "failed");
  send_json(fd, rc == 0 ? 200 : 400, out);
}

static void api_profile_save(int fd, const char *body) {
  char title_id[32];
  form_get(body, "title_id", title_id, sizeof title_id);
  char *toml = malloc(REQ_MAX);
  if (!toml) { send_json(fd, 500, "{}"); return; }
  int n = form_get(body, "toml", toml, REQ_MAX);
  if (n <= 0 || !title_id[0]) { free(toml); send_json(fd, 400, "{\"error\":\"title_id and toml required\"}"); return; }
  int rc = e2a_profile_save(title_id, toml, (size_t)n);
  free(toml);
  send_json(fd, rc == 0 ? 200 : 400, rc == 0 ? "{\"ok\":true}" : "{\"error\":\"profile rejected\"}");
}

/* value of a request header (header block only), "" when absent */
static void header_get(const char *req, const char *name, char *out, size_t cap) {
  out[0] = 0;
  const char *end = strstr(req, "\r\n\r\n");
  size_t nl = strlen(name);
  for (const char *p = strstr(req, "\r\n"); p && (!end || p < end); p = strstr(p + 2, "\r\n")) {
    if (strncasecmp(p + 2, name, nl) == 0 && p[2 + nl] == ':') {
      const char *v = p + 3 + nl;
      while (*v == ' ') v++;
      size_t o = 0;
      while (*v && *v != '\r' && o + 1 < cap) out[o++] = *v++;
      out[o] = 0;
      return;
    }
  }
}

static bool peer_is_local(int fd) {
  struct sockaddr_in a;
  socklen_t len = sizeof a;
  if (getpeername(fd, (struct sockaddr *)&a, &len) != 0 || a.sin_family != AF_INET) return false;
  return ntohl(a.sin_addr.s_addr) == 0x7f000001u;
}

static void api_pair_info(int fd) {
  char ip[64], out[256];
  pair_local_ip(ip, sizeof ip);
  snprintf(out, sizeof out, "{\"pin\":\"%s\",\"ip\":\"%s\",\"port\":%d,\"devices\":%d}", pair_pin(), ip, E2A_HTTP_PORT, pair_devices());
  send_json(fd, 200, out);
}

static void handle(int fd) {
  size_t cap = 16 * 1024, n = 0;
  char *req = malloc(cap + 1);
  if (!req) return;
  req[0] = 0;
  bool complete = false;
  for (;;) {
    if (n == cap) {
      if (cap >= REQ_MAX) { send_json(fd, 413, "{\"error\":\"too large\"}"); free(req); return; }
      size_t nc = cap * 4 > REQ_MAX ? REQ_MAX : cap * 4;
      char *nr = realloc(req, nc + 1);
      if (!nr) break;
      req = nr; cap = nc;
    }
    ssize_t r = recv(fd, req + n, cap - n, 0);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) break;
    n += (size_t)r;
    req[n] = 0;
    char *hdr_end = strstr(req, "\r\n\r\n");
    if (hdr_end) {
      *hdr_end = 0;   /* look for the header in the header block only, never in the body */
      const char *cl = strcasestr(req, "\r\nContent-Length:");
      *hdr_end = '\r';
      size_t want = cl ? (size_t)atol(cl + 17) : 0;
      size_t have = n - (size_t)(hdr_end + 4 - req);
      if (want > REQ_MAX) { send_json(fd, 413, "{\"error\":\"too large\"}"); free(req); return; }
      if (have >= want) { complete = true; break; }
    }
  }
  if (n == 0) { free(req); return; }  /* idle/preconnect socket closed or timed out */
  /* never act on a truncated request (timeout / reset mid-body): a cut "dest" or "path" would be used as is */
  if (!complete) { send_json(fd, 400, "{\"error\":\"incomplete request\"}"); free(req); return; }
  req[n] = 0;
  char method[8] = {0}, path[2048] = {0};
  sscanf(req, "%7s %2047s", method, path);
  const char *body = strstr(req, "\r\n\r\n");
  size_t body_len = body ? n - (size_t)(body + 4 - req) : 0;   /* binary bodies (an update piece) hold zeros */
  body = body ? body + 4 : "";
  char *q = strchr(path, '?');
  if (q) *q++ = 0;
  const char *args = body[0] ? body : (q ? q : "");
  bool post = strcmp(method, "POST") == 0;
  char v[700];

  /* pairing: the console itself (the home-screen app) is trusted; other devices need their token,
   * sent as a header, or as t= for <img> URLs */
  bool local = peer_is_local(fd);
  char token[80];
  header_get(req, "X-E2A-Token", token, sizeof token);
  if (!token[0] && q) form_get(q, "t", token, sizeof token);
  bool authorized = local || pair_token_ok(token);
  bool open_route = strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0 || strcmp(path, "/api/hello") == 0 ||
                    strcmp(path, "/api/pair") == 0;
  if (!authorized && !open_route) { send_json(fd, 401, "{\"error\":\"pair\"}"); free(req); return; }

  if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)
    send_response(fd, 200, "text/html; charset=utf-8", (const char *)index_html, index_html_len);
  else if (strcmp(path, "/api/hello") == 0) {
    char out[160];
    snprintf(out, sizeof out, "{\"paired\":%s,\"local\":%s,\"version\":\"%s\"}", authorized ? "true" : "false", local ? "true" : "false",
             E2A_VERSION);
    send_json(fd, 200, out);
  } else if (strcmp(path, "/api/pair") == 0 && post) {
    char pin[16], tok[65];
    form_get(args, "pin", pin, sizeof pin);
    int rc = pair_register(pin, tok);
    char out[128];
    if (rc == 0) {
      snprintf(out, sizeof out, "{\"token\":\"%s\"}", tok);
      send_json(fd, 200, out);
      e2a_notify("exfat2ampr: new device paired");
    } else send_json(fd, rc == -2 ? 429 : 403, rc == -2 ? "{\"error\":\"blocked\"}" : "{\"error\":\"bad code\"}");
  } else if (strcmp(path, "/api/pair/info") == 0 && local) api_pair_info(fd);
  else if (strcmp(path, "/api/pair/reset") == 0 && post && local) { pair_reset(); api_pair_info(fd); }
  else if (strcmp(path, "/api/state") == 0) api_state(fd, args);
  else if (strcmp(path, "/api/check") == 0) {
    char p[600], smp[16], dst[300], out[1400];
    form_get(args, "path", p, sizeof p);
    form_get(args, "smp", smp, sizeof smp);
    form_get(args, "dest", dst, sizeof dst);
    if (job_preflight(p, smp, dst, out, sizeof out) == 0) send_json(fd, 200, out);
    else send_json(fd, 500, "{\"error\":\"preflight\"}");
  } else if (strcmp(path, "/api/settings") == 0) {
    if (post && form_get(args, "game_mode", v, sizeof v) > 0) activity_set_game_mode_enabled(v[0] == '1');
    char out[64];
    snprintf(out, sizeof out, "{\"game_mode\":%s}", activity_game_mode_enabled() ? "true" : "false");
    send_json(fd, 200, out);
  }
  else if (strcmp(path, "/api/sources") == 0) api_list(fd, job_list_sources);
  else if (strcmp(path, "/api/destinations") == 0) api_list(fd, job_list_destinations);
  else if (strcmp(path, "/api/resumable") == 0) api_list(fd, job_list_resumable);
  else if (strcmp(path, "/api/profiles") == 0) api_list(fd, job_list_profiles);
  else if (strcmp(path, "/api/icon") == 0) api_icon(fd, args);
  else if (strcmp(path, "/api/traces") == 0) api_list(fd, trace_list_json);
  else if (strncmp(path, "/api/trace/", 11) == 0 && post) api_trace(fd, path + 11, args);
  else if (strcmp(path, "/api/start") == 0 && post) api_start(fd, args, false);
  else if (strcmp(path, "/api/queue") == 0 && post) api_start(fd, args, true);
  else if (strcmp(path, "/api/quit") == 0 && post) {
    /* the home-screen app replaces an older engine this way; never while something is converting */
    if (job_busy()) send_json(fd, 409, "{\"error\":\"busy\"}");
    else {
      send_json(fd, 200, "{\"ok\":true}");
      e2a_log("quit requested (engine replaced)");
      close(fd);
      exit(0);
    }
  }
  else if (strcmp(path, "/api/pause") == 0 && post) { job_pause(); send_json(fd, 200, "{\"ok\":true}"); }
  else if (strcmp(path, "/api/cancel") == 0 && post) { job_cancel(); send_json(fd, 200, "{\"ok\":true}"); }
  else if (strcmp(path, "/api/retry") == 0 && post) {
    if (job_retry() == 0) send_json(fd, 200, "{\"ok\":true}");
    else send_json(fd, 409, "{\"error\":\"nothing to retry\",\"code\":\"nothing_to_retry\"}");
  } else if (strncmp(path, "/api/update/", 12) == 0 && post) {
    if (!local) send_json(fd, 403, "{\"error\":\"from the console only\"}");
    else api_update(fd, path + 12, q, body, body_len);
  } else if (strcmp(path, "/api/speedtest") == 0 && post) {
    api_speedtest(fd, args);
  } else if (strcmp(path, "/api/delete-source") == 0 && post) {
    char p[600], code[32] = "", out[96];
    form_get(args, "path", p, sizeof p);
    if (job_delete_source(p, code, sizeof code) == 0) send_json(fd, 200, "{\"ok\":true}");
    else {
      snprintf(out, sizeof out, "{\"error\":\"refused\",\"code\":\"%s\"}", code);
      send_json(fd, 409, out);
    }
  } else if (strcmp(path, "/api/dismiss") == 0 && post) {
    if (job_dismiss() == 0) send_json(fd, 200, "{\"ok\":true}");
    else send_json(fd, 409, "{\"error\":\"a conversion is running\",\"code\":\"busy\"}");
  }
  else if (strcmp(path, "/api/resume") == 0 && post) {
    form_get(args, "dest", v, sizeof v);
    int rc = SAFE_PATH(v) ? job_resume(v) : -1;
    send_json(fd, rc == 0 ? 200 : 400, rc == 0 ? "{\"ok\":true}" : "{\"error\":\"cannot resume\"}");
  } else if (strcmp(path, "/api/discard") == 0 && post) {
    form_get(args, "dest", v, sizeof v);
    int rc = SAFE_PATH(v) ? job_discard(v) : -1;
    send_json(fd, rc == 0 ? 200 : 400, rc == 0 ? "{\"ok\":true}" : "{\"error\":\"cannot discard\"}");
  } else if (strcmp(path, "/api/profile") == 0 && post) api_profile_save(fd, args);
  else if (strcmp(path, "/api/profiles/import-usb") == 0 && post) {
    char out[64];
    snprintf(out, sizeof out, "{\"ok\":true,\"imported\":%d}", e2a_profile_import_usb());
    send_json(fd, 200, out);
  } else send_json(fd, 404, "{\"error\":\"not found\"}");
  free(req);
}

/* bind + listen; returns the server socket or -1 (port busy: another instance is running) */
int httpd_listen(int port) {
  signal(SIGPIPE, SIG_IGN);   /* a browser closing a connection must not kill the payload */
  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) return -1;
  int one = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons((uint16_t)port);
  if (bind(srv, (struct sockaddr *)&addr, sizeof addr) != 0) { close(srv); return -1; }
  if (listen(srv, 64) != 0) { close(srv); return -1; }
  return srv;
}

/* One thread per connection: a slow request (SMP query, USB scan) or an idle browser
 * preconnect socket must never block the other requests of the page. */
#define MAX_CLIENTS 32
static pthread_mutex_t g_cli_mx = PTHREAD_MUTEX_INITIALIZER;
static int g_clients;

static void *client_thread(void *arg) {
  int fd = (int)(intptr_t)arg;
  handle(fd);
  close(fd);
  pthread_mutex_lock(&g_cli_mx);
  g_clients--;
  pthread_mutex_unlock(&g_cli_mx);
  return NULL;
}

static void serve_one(int fd) {
  int one = 1;
  (void)one;
#ifdef SO_NOSIGPIPE
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  struct timeval rtv = {5, 0}, stv = {20, 0};   /* request must arrive within 5 s */
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof rtv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof stv);
  pthread_mutex_lock(&g_cli_mx);
  bool room = g_clients < MAX_CLIENTS;
  if (room) g_clients++;
  pthread_mutex_unlock(&g_cli_mx);
  if (!room) { send_json(fd, 503, "{\"error\":\"busy\"}"); close(fd); return; }
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&at, 512 * 1024);
  pthread_t th;
  if (pthread_create(&th, &at, client_thread, (void *)(intptr_t)fd) != 0) {
    pthread_mutex_lock(&g_cli_mx);
    g_clients--;
    pthread_mutex_unlock(&g_cli_mx);
    handle(fd);   /* fall back to serving inline */
    close(fd);
  }
  pthread_attr_destroy(&at);
}

/* Never returns: when the listening socket breaks (rest mode, network reset), it is recreated. */
int httpd_serve(int srv, int port) {
  int failures = 0;
  for (;;) {
    int fd = accept(srv, NULL, NULL);
    if (fd < 0) {
      if (errno == EINTR || errno == ECONNABORTED || errno == EAGAIN) continue;
      if (errno == EMFILE || errno == ENFILE) { usleep(200000); continue; }
      e2a_log("accept failed (%s): restarting the web server", strerror(errno));
      close(srv);
      for (;;) {
        sleep(failures < 10 ? 1 : 5);
        failures++;
        srv = httpd_listen(port);
        if (srv >= 0) { e2a_log("web server listening again on port %d", port); break; }
      }
      continue;
    }
    failures = 0;
    serve_one(fd);
  }
  return 0;
}

int httpd_run(int port) {
  int srv = httpd_listen(port);
  if (srv < 0) return -1;
  return httpd_serve(srv, port);
}
