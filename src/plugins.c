/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE. */
#include "plugins.h"
#include "ampr_pack.h"

#include "activity.h"
#include "job.h"
#include "scan.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* embedded at build time: xxd -i of third_party/plugins (Makefile, tools/plugins.sha256) */
extern unsigned char libScePlayGo_sprx[];
extern unsigned int libScePlayGo_sprx_len;
extern unsigned char libSceAppContent_sprx[];
extern unsigned int libSceAppContent_sprx_len;
extern unsigned char libSceGameUpdate_sprx[];
extern unsigned int libSceGameUpdate_sprx_len;
extern unsigned char libSceNpEntitlementAccess_sprx[];
extern unsigned int libSceNpEntitlementAccess_sprx_len;

typedef struct { const char *version, *variant, *sha256; } release;   /* "log" builds write a log file under /app0 */

/* every build drakmor published: the embedded one first; a file with another digest is left alone */
static const release PLAYGO[] = {
  {"0.6", "nolog", "f5fdb0ed6cc7413e6dcac79a62e0efd8cfbae04cca0bf932e7a741aed850d213"},
  {"0.5", "nolog", "645caa464a7d28fab7019ba547b204ecc007f87ccec072d330eb2b72a3e9ddac"},
  {"0.5", "log", "305e6e117ca3a16cb6062007e3ddee7eb184ca19c3e353d5b12a6161bc92a3bd"},
  {"0.4", "nolog", "a0f20f40107713523ba1a3a0281fb7c4e238b2b5d3fda9354119352d9393d2f0"},
  {"0.4", "log", "39c508a84208a81042dfe637619ece44a6b1d7f737076523aa8096e5eba3457f"},
};
static const release APPCONTENT[] = {
  {"0.3", "nolog", "53e485e22970c693ab70c05bd78b8f627442ed41ca9a49692b38615046494a6b"},
  {"0.3", "log", "6ca6d0e10399422eeb6a4c1085d0ef6c166b8e0dacc1a458c17aac5408140c86"},
  {"0.2", "nolog", "79f25d1ef1206e368e43f7e3316018ce14a114f8088dcb563c471293f88a2fe2"},
  {"0.2", "log", "064ee21a3c0110f7a5ab7f682ee3fc70efb831f2cccf041263e19ea9aca50c18"},
  {"0.1", "nolog", "80af9b73508d2cdf318ca18d4d869ef276ad1fb96c550db0c58bcd68eac76cc3"},
  {"0.1", "log", "db7ce6d3c5766f1c8d7d405b1c5cd51a8a654ee26d079f0701bad25fab4a8513"},
};
static const release GAMEUPDATE[] = {
  {"0.3", "nolog", "1e31ddbe4809b7c5ec117e9b7e1227befe757c31d1f5f73e6c7d5a3d2d238973"},
  {"0.3", "log", "6a3e65faf26356393ff4d7e68b95df46c18521fe8d92c3b7920c8509227fdf15"},
  {"0.2", "nolog", "2d66f588a19c461af572bc7b3138b4100120091fec42c4ce11370080b51dc127"},
  {"0.2", "log", "5f38340ffc0748a1277c14db59b213ef035ec5ed33c64be9a3e7542d815bf104"},
};
static const release ENTITLEMENT[] = {
  {"0.3", "nolog", "3f1826f20e174ff0f7aaa8d7f3f7e386dfcca1136c9e4c0e72a49e041c0b1488"},
  {"0.3", "log", "51871d03aef46163cf5e4165f8641284c509cf57d9baeeba222b168f83806eb3"},
  {"0.2", "nolog", "5333b83a263fe5e8e2ae5ee53d43f0b11c5c519151e1aa367c22621e6020a0bd"},
  {"0.2", "log", "1fc44ef546aa621fef73a13eb1b2c74ac60db0d9f0cf2e680e2b733cb062d707"},
  {"0.1", "nolog", "9315332b065a7039708f09bc489602d5ddedc0802690ec6192bbb3ba916d4b98"},
  {"0.1", "log", "98f8e33dd8d5276756a4c3f8b6f674eefec131cc5c2949b65acf9edad215ea74"},
};

typedef struct {
  const char *name, *project;
  const unsigned char *data;
  const unsigned int *len;
  const release *known;
  int nknown;
} plugin;

enum { P_PLAYGO, P_APPCONTENT, P_GAMEUPDATE, P_ENTITLEMENT, P_N };
static const plugin PLUGINS[P_N] = {
  {"libScePlayGo.sprx", "pgo_stub", libScePlayGo_sprx, &libScePlayGo_sprx_len, PLAYGO, 5},
  {"libSceAppContent.sprx", "dlc_emu", libSceAppContent_sprx, &libSceAppContent_sprx_len, APPCONTENT, 6},
  {"libSceGameUpdate.sprx", "dlc_emu", libSceGameUpdate_sprx, &libSceGameUpdate_sprx_len, GAMEUPDATE, 4},
  {"libSceNpEntitlementAccess.sprx", "dlc_emu", libSceNpEntitlementAccess_sprx, &libSceNpEntitlementAccess_sprx_len, ENTITLEMENT, 6},
};

/* one file on disk against the known releases */
typedef enum { F_ABSENT, F_CURRENT, F_CURRENT_LOG, F_OLD, F_UNKNOWN, F_UNREADABLE } fstate;
static const char *FSTATE[] = {"absent", "current", "current_log", "old", "unknown", "unreadable"};

static fstate classify(const plugin *p, const char *path, const char **version) {
  *version = "";
  if (!e2a_is_file(path)) return F_ABSENT;
  char hex[65];
  if (e2a_sha256_file(path, hex) != 0) return F_UNREADABLE;
  for (int i = 0; i < p->nknown; i++)
    if (strcmp(hex, p->known[i].sha256) == 0) {
      *version = p->known[i].version;
      if (i == 0) return F_CURRENT;
      if (strcmp(p->known[i].version, p->known[0].version) == 0) return F_CURRENT_LOG;
      return F_OLD;
    }
  return F_UNKNOWN;
}

/* --------------------------------------------------------------- a game -- */
typedef struct {
  fstate st[P_N];
  const char *version[P_N];
  bool need[P_N];
  bool playgo_checked;   /* the eboot was scanned for the libScePlayGo import (slow: done in the background pass) */
} game_eval;

static int import_cached(const char *game_dir);

static void eval_game(const char *game_dir, bool scan_eboot, game_eval *ev) {
  char path[1200];
  memset(ev, 0, sizeof *ev);
  for (int i = 0; i < P_N; i++) {
    snprintf(path, sizeof path, "%s/fakelib/%s", game_dir, PLUGINS[i].name);
    ev->st[i] = classify(&PLUGINS[i], path, &ev->version[i]);
  }
  /* the DLC modules are wanted by a game set up for them (its dlc_emu.ini lists the DLC); without the file they
   * would hide the real DLC, so they are never added on their own, only kept up to date */
  snprintf(path, sizeof path, "%s/dlc_emu.ini", game_dir);
  bool dlc = e2a_is_file(path);
  ev->need[P_APPCONTENT] = ev->need[P_GAMEUPDATE] = ev->need[P_ENTITLEMENT] = dlc;
  /* PlayGo: a game that imports libScePlayGo wants the stub (a full dump: every chunk is there) */
  if (ev->st[P_PLAYGO] != F_ABSENT) ev->need[P_PLAYGO] = true;
  else if (scan_eboot) {
    ev->need[P_PLAYGO] = import_cached(game_dir) == 1;
    ev->playgo_checked = true;
  }
}

/* the eboot scan reads the whole executable: its answer is kept per game, keyed by the eboot's size and date */
static int import_cached(const char *game_dir) {
  static struct { char path[600]; uint64_t key; int8_t result; } cache[256];
  static int n, next;
  static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
  char eb[1200];
  struct stat st;
  snprintf(eb, sizeof eb, "%s/eboot.bin", game_dir);
  uint64_t key = stat(eb, &st) == 0 ? ((uint64_t)st.st_size << 20) ^ (uint64_t)st.st_mtime ^ 1 : 0;
  pthread_mutex_lock(&mx);
  for (int i = 0; i < n; i++)
    if (cache[i].key == key && strcmp(cache[i].path, game_dir) == 0) { int r = cache[i].result; pthread_mutex_unlock(&mx); return r; }
  pthread_mutex_unlock(&mx);
  int r = e2a_game_imports(game_dir, "libScePlayGo");
  pthread_mutex_lock(&mx);
  int slot = n < 256 ? n++ : next;
  if (n == 256 && slot == next) next = (next + 1) % 256;
  snprintf(cache[slot].path, sizeof cache[slot].path, "%.599s", game_dir);
  cache[slot].key = key;
  cache[slot].result = (int8_t)r;
  pthread_mutex_unlock(&mx);
  return r;
}

static const char *summary(const game_eval *ev) {
  bool any = false, old = false, missing = false, unknown = false;
  for (int i = 0; i < P_N; i++) {
    if (ev->st[i] == F_ABSENT && !ev->need[i]) continue;
    any = true;
    if (ev->st[i] == F_OLD) old = true;
    else if (ev->st[i] == F_ABSENT) missing = true;
    else if (ev->st[i] == F_UNKNOWN || ev->st[i] == F_UNREADABLE) unknown = true;
  }
  if (!any) return "none";
  if (old) return "update";
  if (missing) return "missing";
  if (unknown) return "unknown";
  return "ok";
}

/* a cheap fingerprint of what the evaluation depends on: sizes and times of the files, dlc_emu.ini */
static uint64_t fingerprint(const char *game_dir) {
  char path[1200];
  struct stat st;
  uint64_t h = 1469598103934665603ull;
  for (int i = 0; i <= P_N; i++) {
    if (i < P_N) snprintf(path, sizeof path, "%s/fakelib/%s", game_dir, PLUGINS[i].name);
    else snprintf(path, sizeof path, "%s/dlc_emu.ini", game_dir);
    uint64_t v = stat(path, &st) == 0 ? ((uint64_t)st.st_size << 20) ^ (uint64_t)st.st_mtime ^ 1 : 0;
    h = (h ^ v) * 1099511628211ull;
    h = (h ^ (uint64_t)i) * 1099511628211ull;
  }
  return h;
}

/* ---------------------------------------------------------------- cache -- */
#define CACHE_MAX 512
typedef struct { char path[600]; uint64_t fp; char state[12]; bool full; } centry;
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static centry g_cache[CACHE_MAX];
static int g_ncache, g_next;

static void cache_put(const char *path, uint64_t fp, const char *state, bool full) {
  pthread_mutex_lock(&g_mx);
  centry *e = NULL;
  for (int i = 0; i < g_ncache; i++) if (strcmp(g_cache[i].path, path) == 0) { e = &g_cache[i]; break; }
  if (!e) {
    if (g_ncache < CACHE_MAX) e = &g_cache[g_ncache++];
    else { e = &g_cache[g_next]; g_next = (g_next + 1) % CACHE_MAX; }
    snprintf(e->path, sizeof e->path, "%s", path);
  }
  e->fp = fp;
  e->full = full;
  snprintf(e->state, sizeof e->state, "%s", state);
  pthread_mutex_unlock(&g_mx);
}

void plugins_state(const char *game_dir, char out[16]) {
  uint64_t fp = fingerprint(game_dir);
  pthread_mutex_lock(&g_mx);
  for (int i = 0; i < g_ncache; i++)
    if (strcmp(g_cache[i].path, game_dir) == 0 && g_cache[i].fp == fp) {
      snprintf(out, 16, "%s", g_cache[i].state);   /* copied under the lock: the entry may be reused */
      pthread_mutex_unlock(&g_mx);
      return;
    }
  pthread_mutex_unlock(&g_mx);
  /* not seen yet: the quick evaluation (no eboot scan); the background pass completes it */
  game_eval ev;
  eval_game(game_dir, false, &ev);
  const char *s = summary(&ev);
  cache_put(game_dir, fp, s, false);
  snprintf(out, 16, "%s", s);
}

int plugins_status_json(const char *game_dir, char *json, size_t cap) {
  game_eval ev;
  eval_game(game_dir, true, &ev);
  cache_put(game_dir, fingerprint(game_dir), summary(&ev), true);
  size_t pos = 0;
  int n = snprintf(json, cap, "{\"state\":\"%s\",\"files\":[", summary(&ev));
  if (n < 0 || (size_t)n >= cap) return -1;
  pos = (size_t)n;
  for (int i = 0; i < P_N; i++) {
    n = snprintf(json + pos, cap - pos, "%s{\"name\":\"%s\",\"project\":\"%s\",\"state\":\"%s\",\"version\":\"%s\",\"latest\":\"%s\",\"needed\":%s}",
                 i ? "," : "", PLUGINS[i].name, PLUGINS[i].project, FSTATE[ev.st[i]], ev.version[i], PLUGINS[i].known[0].version,
                 ev.need[i] ? "true" : "false");
    if (n < 0 || (size_t)n >= cap - pos) return -1;
    pos += (size_t)n;
  }
  n = snprintf(json + pos, cap - pos, "]}");
  return n < 0 || (size_t)n >= cap - pos ? -1 : 0;
}

/* ---------------------------------------------------------------- apply -- */
static void add_report(char *report, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void add_report(char *report, size_t cap, const char *fmt, ...) {
  size_t n = strlen(report);
  if (n + 2 >= cap) return;
  if (n) { report[n++] = ';'; report[n++] = ' '; report[n] = 0; }
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(report + n, cap - n, fmt, ap);
  va_end(ap);
}

/* write one embedded file, the previous copy kept as <name>.old (nothing is deleted) */
static int put_file(const char *dir, const plugin *p, bool keep_old, char *report, size_t cap) {
  char path[1200], old[1300];
  snprintf(path, sizeof path, "%s/%s", dir, p->name);
  if (keep_old) {
    snprintf(old, sizeof old, "%s.old", path);
    if (rename(path, old) != 0) {
      add_report(report, cap, "%s: cannot set the old copy aside (%s)", p->name, strerror(errno));
      return -1;
    }
  }
  if (e2a_write_file_atomic(path, p->data, *p->len) != 0) {
    add_report(report, cap, "%s: cannot write (%s)", p->name, strerror(errno));
    if (keep_old) rename(old, path);   /* back as it was */
    return -1;
  }
  return 0;
}

/* the game read in eval_dir, the files written to write_dir/fakelib (the same folder, or the destination of a
 * conversion); keep_old: a replaced copy stays next to the new one as <name>.old */
static pthread_mutex_t g_apply_mx = PTHREAD_MUTEX_INITIALIZER;   /* in-place refreshes one at a time: the background pass and a request never race */

static int apply_core(const char *eval_dir, const char *write_dir, bool keep_old, unsigned *written, char *report, size_t cap) {
  bool in_place = strcmp(eval_dir, write_dir) == 0;
  report[0] = 0;
  *written = 0;
  game_eval ev;
  eval_game(eval_dir, true, &ev);   /* the slow part (eboot scan, digests) runs without the lock */
  int count = 0, rc = 0;
  char dir[1100], path[1300];
  snprintf(dir, sizeof dir, "%s/fakelib", write_dir);
  if (in_place) pthread_mutex_lock(&g_apply_mx);
  e2a_game g;
  if (in_place && e2a_read_game(eval_dir, &g) == 0 && activity_title_running(g.title_id)) {   /* launched meanwhile */
    add_report(report, cap, "the game is running: left alone");
    pthread_mutex_unlock(&g_apply_mx);
    return -1;
  }
  for (int i = 0; i < P_N; i++) {
    const plugin *p = &PLUGINS[i];
    if (ev.st[i] != F_ABSENT && ev.st[i] != F_OLD) {
      if (ev.st[i] == F_UNKNOWN) add_report(report, cap, "%s: a build we do not know, left alone", p->name);
      else if (ev.st[i] == F_UNREADABLE) { add_report(report, cap, "%s: unreadable", p->name); rc = -1; }
      continue;   /* current */
    }
    if (ev.st[i] == F_ABSENT && !ev.need[i]) continue;
    if (!in_place) {   /* a resumed conversion: the destination may already hold the file; never rewrite it (its
                        * date is part of the checkpoint's hash), just list it again */
      const char *v;
      snprintf(path, sizeof path, "%s/%s", dir, p->name);
      if (classify(p, path, &v) == F_CURRENT) { *written |= 1u << i; continue; }
    }
    if (e2a_mkdir_p(dir) != 0) { add_report(report, cap, "cannot create fakelib (%s)", strerror(errno)); rc = -1; continue; }
    if (put_file(dir, p, keep_old && ev.st[i] == F_OLD, report, cap) != 0) { rc = -1; continue; }
    count++;
    *written |= 1u << i;
    if (ev.st[i] == F_ABSENT) add_report(report, cap, "%s %s added", p->name, p->known[0].version);
    else add_report(report, cap, "%s %s -> %s", p->name, ev.version[i], p->known[0].version);
  }
  game_eval now;   /* the listing shows the new state right away (what the game needs is kept) */
  eval_game(write_dir, false, &now);
  for (int i = 0; i < P_N; i++) now.need[i] |= ev.need[i];
  cache_put(write_dir, fingerprint(write_dir), summary(&now), true);
  if (count) e2a_log("plugins of %s: %s", write_dir, report);
  if (in_place) pthread_mutex_unlock(&g_apply_mx);
  return rc < 0 ? -1 : count;
}

int plugins_apply(const char *game_dir, char *report, size_t cap) {
  unsigned written;
  return apply_core(game_dir, game_dir, true, &written, report, cap);
}

int plugins_apply_to(const char *game_src, const char *dest, unsigned *written, char *report, size_t cap) {
  return apply_core(game_src, dest, false, written, report, cap);
}

const char *plugins_name(int i) { return i >= 0 && i < P_N ? PLUGINS[i].name : NULL; }

/* ----------------------------------------------------------------- emus -- */
extern unsigned char libSceAmpr_sprx[];   /* the embedded AMPR emulator (gen/libSceAmpr_sprx.c) */
extern unsigned int libSceAmpr_sprx_len;

int plugins_sync_emus(char *report, size_t cap) {
  report[0] = 0;
  char parent[300];   /* ShadowMountPlus' own folder: without it, there is nobody to feed */
  snprintf(parent, sizeof parent, "%s", E2A_EMUS_DIR);
  char *slash = strrchr(parent, '/');
  if (slash) *slash = 0;
  if (!e2a_is_dir(parent)) { add_report(report, cap, "%s not present: ShadowMountPlus not installed", parent); return 0; }
  if (e2a_mkdir_p(E2A_EMUS_DIR) != 0) { add_report(report, cap, "cannot create %s (%s)", E2A_EMUS_DIR, strerror(errno)); return -1; }
  int written = 0, rc = 0;
  for (int i = 0; i < P_N; i++) {
    const plugin *p = &PLUGINS[i];
    char path[1200];
    const char *version;
    snprintf(path, sizeof path, "%s/%s", E2A_EMUS_DIR, p->name);
    fstate st = classify(p, path, &version);
    if (st == F_ABSENT) {
      if (put_file(E2A_EMUS_DIR, p, false, report, cap) == 0) { written++; add_report(report, cap, "%s %s added", p->name, p->known[0].version); }
      else rc = -1;
    } else if (st == F_OLD) {
      if (put_file(E2A_EMUS_DIR, p, true, report, cap) == 0) { written++; add_report(report, cap, "%s %s -> %s", p->name, version, p->known[0].version); }
      else rc = -1;
    } else if (st == F_UNKNOWN) add_report(report, cap, "%s: a build we do not know, left alone", p->name);
  }
  /* the AMPR emulator too: with update_emulators=1, ShadowMountPlus replaces the libSceAmpr.sprx of every game's
   * fakelib with the one in this folder at launch. Missing, it would download one at launch time (nothing
   * without internet); an older one (before 0.4.2.1) cannot read LZ4 packs and the converted game would not start */
  {
    char path[1200], hex[65], mine[65];
    snprintf(path, sizeof path, "%s/libSceAmpr.sprx", E2A_EMUS_DIR);
    e2a_sha256 c;
    uint8_t d[32];
    e2a_sha256_init(&c);
    e2a_sha256_update(&c, libSceAmpr_sprx, libSceAmpr_sprx_len);
    e2a_sha256_final(&c, d);
    for (int i = 0; i < 32; i++) snprintf(mine + i * 2, 3, "%02x", d[i]);
    if (!e2a_is_file(path)) {
      if (libSceAmpr_sprx_len > 4096 && e2a_write_file_atomic(path, libSceAmpr_sprx, libSceAmpr_sprx_len) == 0) {
        written++;
        add_report(report, cap, "libSceAmpr.sprx %s added", E2A_AMPR_RUNTIME_VERSION);
      }
    } else if (e2a_sha256_file(path, hex) == 0 && strcmp(hex, mine) != 0) {
      e2a_log("ShadowMountPlus' AMPR emulator %s (%lld bytes, sha256 %.16s) is not the one this version packs for (%s, %u "
              "bytes): converted games receive it at launch; if it is older than 0.4.2.1 they cannot start",
              path, (long long)e2a_file_size(path), hex, E2A_AMPR_RUNTIME_VERSION, libSceAmpr_sprx_len);
    }
  }
  if (written) e2a_log("ShadowMountPlus emulators (%s): %s", E2A_EMUS_DIR, report);
  return rc < 0 ? -1 : written;
}

/* ------------------------------------------------------------ every game -- */
/* the folders ShadowMountPlus scans: a game is a folder with sce_sys/param.json and eboot.bin, one or two
 * levels under these roots (images are its business: update_emulators applies the emus folder to them) */
static const char *ROOTS[] = {
#ifdef E2A_HOST
  "homebrew",
#else
  "/data/homebrew", "/data/etaHEN/games",
  "/mnt/usb0/homebrew", "/mnt/usb1/homebrew", "/mnt/usb2/homebrew", "/mnt/usb3/homebrew",
  "/mnt/usb4/homebrew", "/mnt/usb5/homebrew", "/mnt/usb6/homebrew", "/mnt/usb7/homebrew",
  "/mnt/ext0/homebrew", "/mnt/ext1/homebrew",
  "/mnt/usb0/etaHEN/games", "/mnt/usb1/etaHEN/games", "/mnt/usb2/etaHEN/games", "/mnt/usb3/etaHEN/games",
  "/mnt/usb4/etaHEN/games", "/mnt/usb5/etaHEN/games", "/mnt/usb6/etaHEN/games", "/mnt/usb7/etaHEN/games",
  "/mnt/ext0/etaHEN/games", "/mnt/ext1/etaHEN/games",
  "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3", "/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7",
  "/mnt/ext0", "/mnt/ext1",
#endif
  NULL};

/* a game folder the engine may touch on request: a real game folder one or two levels under a known root
 * (a drive's top level allows a sub-folder), nothing else on the console */
bool plugins_path_allowed(const char *path) {
  char real[PATH_MAX];
  if (!path || !path[0] || strstr(path, "/../") || !realpath(path, real)) return false;
  e2a_game g;
  if (e2a_read_game(real, &g) != 0) return false;
  for (int r = 0; ROOTS[r]; r++) {
    char root[PATH_MAX];
    if (!realpath(ROOTS[r], root)) continue;
    size_t n = strlen(root);
    if (strncmp(real, root, n) != 0 || real[n] != '/') continue;
    int depth = 1;
    for (const char *p = real + n + 1; *p; p++) if (*p == '/') depth++;
    if (depth <= 2) return true;   /* the listing scans one sub-folder deep under every root */
  }
  return false;
}

typedef struct { int games, updated, failed; char seen[256][600]; int nseen; } pass;

static bool seen(pass *ps, const char *path) {
  for (int i = 0; i < ps->nseen; i++) if (strcasecmp(ps->seen[i], path) == 0) return true;
  if (ps->nseen < 256) snprintf(ps->seen[ps->nseen++], 600, "%.599s", path);
  return false;
}

static void visit_game(pass *ps, const char *path) {
  e2a_game g;
  if (e2a_read_game(path, &g) != 0 || seen(ps, path)) return;
  ps->games++;
  if (activity_title_running(g.title_id)) return;   /* its fakelib is mounted: next time */
  if (!job_claim_path(path, true)) return;   /* the source or destination of a conversion (running, queued or paused) */
  char report[400];
  /* folders converted by earlier versions: index with the wrong hash, executables without an execute bit */
  int fixed = e2a_repair_converted(path, report, sizeof report);
  if (fixed != 0) e2a_log("repair of %s: %s", path, report);
  int n = fixed < 0 ? -1 : plugins_apply(path, report, sizeof report);
  job_release_path();
  if (n > 0) ps->updated++;
  else if (n < 0) { ps->failed++; e2a_log("plugins of %s: %s", path, report); }
}

static void visit_dir(pass *ps, const char *root, int depth) {
  DIR *d = opendir(root);
  if (!d) return;
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    if (de->d_name[0] == '.' || strcasecmp(de->d_name, "backports") == 0) continue;
    char path[1100];
    snprintf(path, sizeof path, "%s/%s", root, de->d_name);
    if (!e2a_is_dir(path)) continue;
    char probe[1200];
    snprintf(probe, sizeof probe, "%s/sce_sys/param.json", path);
    if (e2a_is_file(probe)) visit_game(ps, path);
    else if (depth > 0) visit_dir(ps, path, depth - 1);
  }
  closedir(d);
}

static struct { time_t at; int games, updated, failed, emus; char emus_report[400]; bool running; } g_last;

int plugins_refresh_all(char *report, size_t cap) {
  pass *ps = calloc(1, sizeof *ps);
  if (!ps) { snprintf(report, cap, "out of memory"); return -1; }
  for (int r = 0; ROOTS[r]; r++) visit_dir(ps, ROOTS[r], 1);   /* one sub-folder deep, like the listing */
  snprintf(report, cap, "%d games, %d updated, %d failed", ps->games, ps->updated, ps->failed);
  pthread_mutex_lock(&g_mx);
  g_last.at = time(NULL);
  g_last.games = ps->games;
  g_last.updated = ps->updated;
  g_last.failed = ps->failed;
  pthread_mutex_unlock(&g_mx);
  int updated = ps->updated;
  free(ps);
  e2a_log("plugins: %s", report);
  return updated;
}

int plugins_info_json(char *json, size_t cap) {
  size_t pos = 0;
  int n = snprintf(json, cap, "{\"embedded\":[");
  if (n < 0 || (size_t)n >= cap) return -1;
  pos = (size_t)n;
  for (int i = 0; i < P_N; i++) {
    n = snprintf(json + pos, cap - pos, "%s{\"name\":\"%s\",\"project\":\"%s\",\"version\":\"%s\",\"size\":%u}",
                 i ? "," : "", PLUGINS[i].name, PLUGINS[i].project, PLUGINS[i].known[0].version, *PLUGINS[i].len);
    if (n < 0 || (size_t)n >= cap - pos) return -1;
    pos += (size_t)n;
  }
  char er[500];
  pthread_mutex_lock(&g_mx);
  e2a_json_escape(g_last.emus_report, er, sizeof er);
  n = snprintf(json + pos, cap - pos, "],\"emus\":{\"dir\":\"%s\",\"written\":%d,\"report\":\"%s\"},"
               "\"last\":{\"at\":%lld,\"running\":%s,\"games\":%d,\"updated\":%d,\"failed\":%d}}",
               E2A_EMUS_DIR, g_last.emus, er, (long long)g_last.at, g_last.running ? "true" : "false",
               g_last.games, g_last.updated, g_last.failed);
  pthread_mutex_unlock(&g_mx);
  return n < 0 || (size_t)n >= cap - pos ? -1 : 0;
}

/* ----------------------------------------------------------- background -- */
static pthread_mutex_t g_wake_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER;
static bool g_asked;

static void *background(void *arg) {
  (void)arg;
  uint64_t storage_rev = 0;
  for (;;) {
    pthread_mutex_lock(&g_mx);
    g_last.running = true;
    pthread_mutex_unlock(&g_mx);
    char report[500];
    int e = plugins_sync_emus(report, sizeof report);
    pthread_mutex_lock(&g_mx);
    g_last.emus = e;
    snprintf(g_last.emus_report, sizeof g_last.emus_report, "%.399s", report);
    pthread_mutex_unlock(&g_mx);
    plugins_refresh_all(report, sizeof report);
    pthread_mutex_lock(&g_mx);
    g_last.running = false;
    pthread_mutex_unlock(&g_mx);
    storage_rev = activity_storage_rev();
    /* again on request, when a drive comes or goes, and every 6 hours */
    pthread_mutex_lock(&g_wake_mx);
    for (int waited = 0; !g_asked && waited < 6 * 3600 && activity_storage_rev() == storage_rev; waited += 30) {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += 30;
      pthread_cond_timedwait(&g_wake, &g_wake_mx, &ts);
    }
    g_asked = false;
    pthread_mutex_unlock(&g_wake_mx);
    if (activity_storage_rev() != storage_rev) sleep(20);   /* let the new drive settle and be scanned */
  }
  return NULL;
}

void plugins_start_background(void) {
  static bool started;
  if (started) {   /* a later call = a refresh request */
    pthread_mutex_lock(&g_wake_mx);
    g_asked = true;
    pthread_cond_signal(&g_wake);
    pthread_mutex_unlock(&g_wake_mx);
    return;
  }
  /* the embedded files must be the releases the table expects (a build with other downloads would mislabel) */
  for (int i = 0; i < P_N; i++) {
    e2a_sha256 c;
    uint8_t d[32];
    char hex[65];
    e2a_sha256_init(&c);
    e2a_sha256_update(&c, PLUGINS[i].data, *PLUGINS[i].len);
    e2a_sha256_final(&c, d);
    for (int k = 0; k < 32; k++) snprintf(hex + k * 2, 3, "%02x", d[k]);
    if (strcmp(hex, PLUGINS[i].known[0].sha256) != 0)
      e2a_log("WARNING: embedded %s is not %s %s (sha256 %s)", PLUGINS[i].name, PLUGINS[i].project, PLUGINS[i].known[0].version, hex);
  }
  pthread_t th;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&th, &at, background, NULL) != 0) e2a_log("plugins: cannot start the background pass");
  else started = true;   /* only a running thread can take the later refresh requests */
  pthread_attr_destroy(&at);
}
