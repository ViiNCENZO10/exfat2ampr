/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * exFAT2AMPR home-screen app: a DualSense interface for the exfat2ampr engine.
 * The engine (privileged payload) does the work; this sandboxed title starts it through the ELF
 * loader when needed and drives its HTTP API on 127.0.0.1:6972. Conversions keep running when the
 * app is closed.
 * The look: the selected game's key art as wallpaper behind dark glass panels (assets.c), motion
 * on every change (anim.h: gliding focus, sliding screens, fading covers) and soft navigation
 * sounds (sound.c). Both can be turned off in the settings. */
#include "anim.h"
#include "api.h"
#include "assets.h"
#include "gfx.h"
#include "i18n.h"
#include "img.h"
#include "json.h"
#include "platform.h"
#include "qr.h"
#include "sound.h"
#include "update.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ theme -- */
#define C_BG_BOT RGB(6, 8, 14)
#define C_PANEL RGBA(10, 14, 26, 158)        /* dark glass over the wallpaper */
#define C_PANEL_EDGE RGBA(255, 255, 255, 22)
#define C_PANEL_HI RGBA(255, 255, 255, 30)
#define C_SEL RGBA(48, 120, 255, 70)
#define C_SEL_EDGE RGB(90, 160, 255)
#define C_TEXT RGB(236, 240, 248)
#define C_DIM RGB(150, 160, 180)
#define C_ACCENT RGB(64, 150, 255)
#define C_OK RGB(80, 210, 140)
#define C_WARN RGB(255, 190, 80)
#define C_ERR RGB(255, 100, 100)

/* ------------------------------------------------------------------- data -- */
#define MAX_GAMES 256
#define MAX_DESTS 16
#define MAX_RESUME 32

typedef struct {
  char path[600], smp[16], title_id[16], title[160], version[32], reason[32], plugins[12];
  bool image, selectable, ampr, partial, marked;
  bool dup;                 /* another copy of the same title is in the list */
  long long size;
} game_t;

typedef struct {
  char path[300], mount[256], device[256], label[64], kind[16], fstype[16], reason[24];
  int index;
  double total, free;
  bool writable;
  int read_mbps, write_mbps;   /* measured on demand, -1 = unknown */
} dest_t;
typedef struct { char dest[600], source[600], name[160], stage[16]; bool available, active; int level; } resum_t;

typedef struct {
  bool ok;
  char state[16], phase[24], message[300], title[160], title_id[16], dest[600], version[48];
  int percent, queue_len;
  double speed, total, done, source_bytes, packed_bytes, shared_bytes;
  long long eta;
  char queue[16][96];
  double storage_rev;
  char game_running[16];
  bool game_mode, game_mode_enabled;
  char code[32], reason[32];          /* the engine's result code, ShadowMountPlus' busy reason */
  bool waiting, can_retry, resumable; /* waiting for ShadowMountPlus; what the Progress tab offers */
  bool deleting, delete_ok;           /* deleting a copy from the list, and how the last one ended */
  double delete_seq;
  char delete_code[32], delete_title[128];
} status_t;

typedef struct {
  int level, cache, dest;
  bool verify, dedup, clean, overwrite, remove;
} options_t;

enum { TAB_GAMES, TAB_PROGRESS, TAB_RESUME, TAB_SETTINGS, TAB_COUNT };
enum { SCR_BOOT, SCR_MAIN, SCR_OPTIONS, SCR_PAIR, SCR_DEST, SCR_CHECK, SCR_OPTIMIZE };
enum { SET_LANG, SET_GAME_MODE, SET_SOUNDS, SET_ANIMS, SET_PAIR, SET_PLUGINS, SET_UPDATE, SET_QUIT, SET_COUNT };
enum { TOAST_INFO, TOAST_OK, TOAST_ERR };

static game_t g_games[MAX_GAMES];
static int g_ngames, g_sel, g_scroll;
static bool g_games_loaded;
static dest_t g_dests[MAX_DESTS];
static int g_ndests;
static char g_dest_path[300];   /* chosen destination, kept across refreshes and launches */
static resum_t g_res[MAX_RESUME];
static int g_nres, g_res_sel;
static status_t g_st;
static options_t g_opt = {9, 0, 0, true, true, true, false, false};
static int g_opt_sel;
static int g_screen = SCR_BOOT, g_tab = TAB_GAMES, g_set_sel;
static char g_engine_version[48], g_boot_msg[300];
static bool g_boot_failed;
static char g_toast[300];
static uint64_t g_toast_at, g_toast_until;
static int g_toast_kind;
static bool g_dirty = true;
static char g_pin[16], g_ip[64];
static int g_pair_devices;
/* the game plugins (pgo_stub, dlc_emu) the engine keeps up to date: /api/plugins */
static struct { char pgo[16], dlc[16]; int games, updated; bool running, ok; } g_plug;
static uint64_t g_app_started_at;   /* last conversion started from this app (remote ones get a toast) */
static bool run_checks_later;        /* storages changed while the check screen is open */

/* motion */
static int g_prev_screen = -1, g_prev_tab = -1, g_nav_dir = 1, g_tr_dir = 1;
static uint64_t g_tr_start, g_sel_changed_at, g_done_at, g_err_at, g_confirm_at, g_lang_at, g_boot_failed_at;
static bool g_tr_tab;
static int g_lang_dir = 1;
static image_t g_logo;
static bool g_logo_tile;   /* the icon's dark tile, cropped from its white margin */

/* start: the engine is checked (and started) on a thread while the splash animates */
#define SPLASH_MS 1100
static char g_boot_want[64];
static int g_boot_state;   /* 0 idle, 1 working, 2 finished (atomic) */
static engine_result g_boot_result;
static uint64_t g_boot_started;

/* confirm dialog */
enum { ACT_NONE, ACT_CANCEL_JOB, ACT_DISCARD, ACT_START_REMOVE, ACT_PAIR_RESET, ACT_DELETE_SOURCE, ACT_UPDATE };
static int g_confirm;
static char g_confirm_text[300], g_confirm_detail[700];
static char g_delete_path[600];     /* the copy the confirm dialog is about */

/* OPTIONS on a game: a small menu, like on the PS5 home screen */
enum { MENU_OPTIMIZE, MENU_PLUGINS, MENU_CONVERT_ALL, MENU_DELETE, MENU_SETTINGS, MENU_COUNT };
static bool g_menu;
static int g_menu_sel;
static uint64_t g_menu_at;

/* bounded copy; truncation is intended (display strings) */
static void scopy(char *dst, size_t cap, const char *src) {
  size_t i = 0;
  if (!cap) return;
  for (; src && src[i] && i + 1 < cap; i++) dst[i] = src[i];
  dst[i] = 0;
}

static void toast_kind(const char *msg, int kind) {
  scopy(g_toast, sizeof g_toast, msg);
  g_toast_at = plat_ms();
  g_toast_until = g_toast_at + 3600;
  g_toast_kind = kind;
  g_dirty = true;
  if (kind == TOAST_ERR) snd_cue_frame(SND_ERROR);
}
static void toast(const char *msg) { toast_kind(msg, TOAST_INFO); }
static void toast_err(const char *msg) { toast_kind(msg, TOAST_ERR); }

/* ---------------------------------------------------------------- formats -- */
static void fmt_bytes(char *out, size_t cap, double b) {
  int l = i18n_get();
  const char *gb = l == LANG_FR ? "Go" : l == LANG_RU ? "ГБ" : "GB", *mb = l == LANG_FR ? "Mo" : l == LANG_RU ? "МБ" : "MB";
  if (b >= 1073741824.0) snprintf(out, cap, "%.1f %s", b / 1073741824.0, gb);
  else snprintf(out, cap, "%.0f %s", b / 1048576.0, mb);
  if (l != LANG_EN && l != LANG_ZH && l != LANG_TH)   /* 199,1 Go: a decimal comma everywhere else */
    for (char *c = out; *c; c++)
      if (*c == '.') *c = ',';
}

static void fmt_dur(char *out, size_t cap, long long s) {
  if (s < 0) { scopy(out, cap, "—"); return; }
  long long h = s / 3600, m = s % 3600 / 60, x = s % 60;
  if (h) snprintf(out, cap, "%lld h %02lld min", h, m);
  else if (m) snprintf(out, cap, "%lld min %02lld s", m, x);
  else snprintf(out, cap, "%lld s", x);
}

static const char *phase_label(const char *p) {
  if (!strcmp(p, "scan")) return T(S_PH_SCAN);
  if (!strcmp(p, "copy")) return T(S_PH_COPY);
  if (!strcmp(p, "pack")) return T(S_PH_PACK);
  if (!strcmp(p, "verify")) return T(S_PH_VERIFY);
  if (!strcmp(p, "index")) return T(S_PH_INDEX);
  if (!strcmp(p, "mount") || !strcmp(p, "unmount")) return T(S_PH_MOUNT);
  if (!strcmp(p, "done")) return T(S_DONE);
  if (!strcmp(p, "error")) return T(S_ERROR);
  return T(S_PH_FINISH);
}

static const char *state_label(const char *s) {
  if (!strcmp(s, "running")) return T(S_RUNNING);
  if (!strcmp(s, "done")) return T(S_DONE);
  if (!strcmp(s, "error")) return T(S_ERROR);
  if (!strcmp(s, "cancelled")) return T(S_CANCELLED);
  if (!strcmp(s, "paused")) return T(S_PAUSED);
  return T(S_IDLE);
}

/* --------------------------------------------------------------- settings -- */
static void settings_save(void) {
  char buf[700];
  int n = snprintf(buf, sizeof buf, "lang=%d\nlevel=%d\ncache=%d\nverify=%d\ndedup=%d\nclean=%d\noverwrite=%d\nsound=%d\nanims=%d\ndest=%.300s\n",
                   i18n_get(), g_opt.level, g_opt.cache, g_opt.verify, g_opt.dedup, g_opt.clean, g_opt.overwrite, snd_enabled(),
                   g_anim_enabled, g_dest_path);
  if (n > 0) plat_write_file(plat_settings_path(), buf, (size_t)n);
}

static void settings_load(void) {
  i18n_set(i18n_from_system(plat_language()));
  uint8_t *d;
  size_t len;
  if (plat_read_file(plat_settings_path(), &d, &len) != 0) return;
  char *s = realloc(d, len + 1);
  if (!s) { free(d); return; }
  s[len] = 0;
  for (char *line = strtok(s, "\n"); line; line = strtok(NULL, "\n")) {
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    int v = atoi(eq + 1);
    if (!strcmp(line, "dest")) scopy(g_dest_path, sizeof g_dest_path, eq + 1);
    else if (!strcmp(line, "lang")) i18n_set(v);
    else if (!strcmp(line, "level") && v >= 1 && v <= 12) g_opt.level = v;
    else if (!strcmp(line, "cache")) g_opt.cache = v ? 1 : 0;
    else if (!strcmp(line, "verify")) g_opt.verify = v != 0;
    else if (!strcmp(line, "dedup")) g_opt.dedup = v != 0;
    else if (!strcmp(line, "clean")) g_opt.clean = v != 0;
    else if (!strcmp(line, "overwrite")) g_opt.overwrite = v != 0;
    else if (!strcmp(line, "sound")) snd_set_enabled(v != 0);
    else if (!strcmp(line, "anims")) g_anim_enabled = v != 0;
  }
  free(s);
}

/* -------------------------------------------------------------- API calls -- */
static void load_games(void) {
  jdoc d;
  char keep[600];
  scopy(keep, sizeof keep, g_ngames ? g_games[g_sel].path : "");
  static char marked_path[MAX_GAMES][600];
  int nmarked = 0;
  for (int i = 0; i < g_ngames && nmarked < MAX_GAMES; i++)
    if (g_games[i].marked) scopy(marked_path[nmarked++], 600, g_games[i].path);
  int st = api_call("/api/sources", NULL, &d);
  g_games_loaded = true;
  if (st != 200 || !d.root || d.root->type != J_ARR) { json_free(&d); return; }
  g_ngames = 0;
  for (const jnode *n = d.root->child; n && g_ngames < MAX_GAMES; n = n->next) {
    game_t *g = &g_games[g_ngames];
    memset(g, 0, sizeof *g);
    scopy(g->path, sizeof g->path, json_str(n, "path", ""));
    scopy(g->smp, sizeof g->smp, json_str(n, "smp_title", ""));
    scopy(g->title_id, sizeof g->title_id, json_str(n, "title_id", ""));
    scopy(g->title, sizeof g->title, json_str(n, "title", ""));
    if (!g->title[0]) scopy(g->title, sizeof g->title, g->title_id[0] ? g->title_id : g->path);
    scopy(g->version, sizeof g->version, json_str(n, "version", ""));
    scopy(g->reason, sizeof g->reason, json_str(n, "reason", ""));
    g->image = !strcmp(json_str(n, "type", ""), "image") || json_get(n, "image") != NULL;   /* or a mounted image's folder */
    g->selectable = json_bool(n, "selectable", true);
    g->ampr = json_bool(n, "ampr", false);
    g->partial = json_bool(n, "partial", false);
    scopy(g->plugins, sizeof g->plugins, json_str(n, "plugins", ""));   /* ok, update, missing, unknown, none */
    g->size = (long long)json_num(n, "size", 0);
    for (int k = 0; k < nmarked; k++)
      if (!strcmp(marked_path[k], g->path)) g->marked = g->selectable;
    g_ngames++;
  }
  json_free(&d);
  /* the same title more than once: duplicates the user may want to delete */
  for (int i = 0; i < g_ngames; i++)
    for (int k = i + 1; k < g_ngames; k++)
      if (g_games[i].title_id[0] && !strcmp(g_games[i].title_id, g_games[k].title_id)) g_games[i].dup = g_games[k].dup = true;
  g_sel = 0;
  for (int i = 0; i < g_ngames; i++)
    if (!strcmp(g_games[i].path, keep)) g_sel = i;
  if (g_scroll > g_sel) g_scroll = g_sel;
  g_dirty = true;
}

static void dest_name(const dest_t *d, char *out, size_t cap);
static const char *code_text(const char *c);

static void load_dests(bool announce) {
  jdoc d;
  if (api_call("/api/destinations", NULL, &d) != 200 || !d.root || d.root->type != J_ARR) { json_free(&d); return; }
  static dest_t old[MAX_DESTS];
  int nold = g_ndests;
  memcpy(old, g_dests, sizeof old);
  g_ndests = 0;
  for (const jnode *n = d.root->child; n && g_ndests < MAX_DESTS; n = n->next) {
    dest_t *t = &g_dests[g_ndests++];
    memset(t, 0, sizeof *t);
    scopy(t->path, sizeof t->path, json_str(n, "path", ""));
    scopy(t->mount, sizeof t->mount, json_str(n, "mount", ""));
    scopy(t->device, sizeof t->device, json_str(n, "device", ""));
    scopy(t->label, sizeof t->label, json_str(n, "label", ""));
    scopy(t->kind, sizeof t->kind, json_str(n, "kind", "internal"));
    scopy(t->fstype, sizeof t->fstype, json_str(n, "fstype", ""));
    scopy(t->reason, sizeof t->reason, json_str(n, "reason", ""));
    t->index = (int)json_num(n, "index", -1);
    t->total = json_num(n, "total", 0);
    t->free = json_num(n, "free", 0);
    t->writable = json_bool(n, "writable", false);
    t->read_mbps = (int)json_num(n, "read_mbps", -1);
    t->write_mbps = (int)json_num(n, "write_mbps", -1);
  }
  json_free(&d);
  /* keep the chosen destination; fall back to the first writable one (the internal storage) */
  int chosen = -1;
  for (int i = 0; i < g_ndests; i++)
    if (g_dests[i].writable && !strcmp(g_dests[i].path, g_dest_path)) chosen = i;
  if (chosen < 0) {
    bool was_listed = false;
    for (int i = 0; i < nold; i++) was_listed |= !strcmp(old[i].path, g_dest_path);
    if (announce && g_dest_path[0] && was_listed) { toast(T(S_DEST_GONE)); snd_cue_frame(SND_NOTIFY); }
    /* the fastest storage measured by the speed test (the recommended one), else the first writable */
    int best_speed = 0;
    for (int i = 0; i < g_ndests; i++) {
      int s = g_dests[i].read_mbps >= 0 ? g_dests[i].read_mbps : g_dests[i].write_mbps;
      if (g_dests[i].writable && s > best_speed) { chosen = i; best_speed = s; }
    }
    for (int i = 0; i < g_ndests && chosen < 0; i++)
      if (g_dests[i].writable) chosen = i;
  }
  g_opt.dest = chosen < 0 ? 0 : chosen;
  if (chosen >= 0 && (announce || !g_dest_path[0])) scopy(g_dest_path, sizeof g_dest_path, g_dests[chosen].path);
  if (announce)
    for (int i = 0; i < g_ndests; i++) {
      bool known = false;
      for (int k = 0; k < nold; k++) known |= !strcmp(old[k].mount, g_dests[i].mount);
      if (!known) {
        char name[96], m2[200];
        dest_name(&g_dests[i], name, sizeof name);
        snprintf(m2, sizeof m2, "%s : %s", T(S_DEST_NEW), name);
        toast(m2);
        snd_cue_frame(SND_NOTIFY);
      }
    }
  g_dirty = true;
}

static void load_resumable(void) {
  jdoc d;
  if (api_call("/api/resumable", NULL, &d) != 200 || !d.root || d.root->type != J_ARR) { json_free(&d); return; }
  g_nres = 0;
  for (const jnode *n = d.root->child; n && g_nres < MAX_RESUME; n = n->next) {
    resum_t *r = &g_res[g_nres++];
    scopy(r->dest, sizeof r->dest, json_str(n, "dest", ""));
    scopy(r->source, sizeof r->source, json_str(n, "source", ""));
    scopy(r->name, sizeof r->name, json_str(n, "name", ""));
    scopy(r->stage, sizeof r->stage, json_str(n, "stage", ""));
    r->available = json_bool(n, "source_available", false);
    r->active = json_bool(n, "active", false);
    r->level = (int)json_num(n, "level", 9);
  }
  if (g_res_sel >= g_nres) g_res_sel = g_nres ? g_nres - 1 : 0;
  json_free(&d);
  g_dirty = true;
}

static void poll_status(void) {
  jdoc d;
  status_t s;
  memset(&s, 0, sizeof s);
  char url[64];
  snprintf(url, sizeof url, "/api/state?app=%d", plat_pid());   /* heartbeat: this app is not a game */
  if (api_call(url, NULL, &d) == 200 && d.root) {
    const jnode *r = d.root;
    s.ok = true;
    scopy(s.state, sizeof s.state, json_str(r, "state", "idle"));
    scopy(s.phase, sizeof s.phase, json_str(r, "phase", ""));
    scopy(s.message, sizeof s.message, json_str(r, "message", ""));
    scopy(s.title, sizeof s.title, json_str(r, "title", ""));
    scopy(s.title_id, sizeof s.title_id, json_str(r, "title_id", ""));
    scopy(s.dest, sizeof s.dest, json_str(r, "dest", ""));
    scopy(s.version, sizeof s.version, json_str(r, "version", ""));
    s.percent = (int)json_num(r, "percent", 0);
    s.speed = json_num(r, "speed_bps", 0);
    s.total = json_num(r, "bytes_total", 0);
    s.done = json_num(r, "bytes_done", 0);
    s.eta = (long long)json_num(r, "eta_seconds", -1);
    s.source_bytes = json_num(r, "source_bytes", 0);
    s.packed_bytes = json_num(r, "packed_bytes", 0);
    s.shared_bytes = json_num(r, "shared_bytes", 0);
    const jnode *q = json_get(r, "queue");
    for (const jnode *n = q ? q->child : NULL; n && s.queue_len < 16; n = n->next)
      if (n->type == J_STR) scopy(s.queue[s.queue_len++], 96, n->str);
    s.storage_rev = json_num(r, "storage_rev", 0);
    scopy(s.game_running, sizeof s.game_running, json_str(r, "game_running", ""));
    s.game_mode = json_bool(r, "game_mode", false);
    s.game_mode_enabled = json_bool(r, "game_mode_enabled", true);
    scopy(s.code, sizeof s.code, json_str(r, "code", ""));
    scopy(s.reason, sizeof s.reason, json_str(r, "reason", ""));
    s.waiting = json_bool(r, "waiting", false);
    s.can_retry = json_bool(r, "can_retry", false);
    s.resumable = json_bool(r, "resumable", false);
    s.deleting = json_bool(r, "deleting", false);
    s.delete_ok = json_bool(r, "delete_ok", false);
    s.delete_seq = json_num(r, "delete_seq", 0);
    scopy(s.delete_code, sizeof s.delete_code, json_str(r, "delete_code", ""));
    scopy(s.delete_title, sizeof s.delete_title, json_str(r, "delete_title", ""));
  }
  json_free(&d);
  /* a deletion finished (from here or from the web page): say so, and the list follows */
  static double last_del = -1;
  if (s.ok) {
    if (last_del >= 0 && s.delete_seq != last_del) {
      char m[300];
      if (s.delete_ok) { snprintf(m, sizeof m, T(S_DELETED), s.delete_title); toast_kind(m, TOAST_OK); snd_cue_frame(SND_NOTIFY); }
      else toast_err(code_text(s.delete_code));
      g_games_loaded = false;
    }
    last_del = s.delete_seq;
  }
  /* a drive was plugged in or removed: refresh what depends on it */
  static double last_rev = -1;
  if (s.ok) {
    if (last_rev >= 0 && s.storage_rev != last_rev) {
      load_dests(true);
      g_games_loaded = false;
      if (g_tab == TAB_RESUME) load_resumable();
      if (g_screen == SCR_CHECK) run_checks_later = true;
    }
    last_rev = s.storage_rev;
  }
  if (memcmp(&s, &g_st, sizeof s) != 0) {
    bool finished = g_st.ok && !strcmp(g_st.state, "running") && strcmp(s.state, "running") != 0;
    bool started = g_st.ok && !strcmp(s.state, "running") &&
                   (strcmp(g_st.state, "running") != 0 || (g_st.title_id[0] && strcmp(g_st.title_id, s.title_id) != 0));
    if (started && plat_ms() - g_app_started_at > 8000) {
      char m[300];
      snprintf(m, sizeof m, "%s : %s", T(S_REMOTE_STARTED), s.title[0] ? s.title : s.title_id);
      toast(m);
      snd_cue_frame(SND_NOTIFY);
      g_games_loaded = false;
    }
    g_st = s;
    g_dirty = true;
    if (finished) {
      char m[300];
      snprintf(m, sizeof m, "%s : %s", s.title[0] ? s.title : s.title_id, state_label(s.state));
      plat_notify(m);
      g_games_loaded = false;   /* the list changes after a conversion */
      if (!strcmp(s.state, "done")) { g_done_at = plat_ms(); snd_cue_frame(SND_DONE); }
      else if (!strcmp(s.state, "error")) { g_err_at = plat_ms(); snd_cue_frame(SND_ERROR); }
    }
  }
}

static void load_pair(void) {
  jdoc d;
  if (api_call("/api/pair/info", NULL, &d) == 200 && d.root) {
    scopy(g_pin, sizeof g_pin, json_str(d.root, "pin", "------"));
    scopy(g_ip, sizeof g_ip, json_str(d.root, "ip", "?"));
    int n = (int)json_num(d.root, "devices", 0);
    if (n != g_pair_devices || g_screen == SCR_PAIR) g_dirty = true;
    g_pair_devices = n;
  }
  json_free(&d);
}

/* the embedded plugin releases and the engine's last pass over the games (Settings) */
static void load_plugins_info(void) {
  jdoc d;
  if (api_call("/api/plugins", NULL, &d) == 200 && d.root) {
    const jnode *emb = json_get(d.root, "embedded");
    for (const jnode *n = emb ? emb->child : NULL; n; n = n->next) {
      const char *p = json_str(n, "project", "");
      if (!strcmp(p, "pgo_stub")) scopy(g_plug.pgo, sizeof g_plug.pgo, json_str(n, "version", "?"));
      else if (!strcmp(p, "dlc_emu")) scopy(g_plug.dlc, sizeof g_plug.dlc, json_str(n, "version", "?"));
    }
    const jnode *last = json_get(d.root, "last");
    g_plug.games = last ? (int)json_num(last, "games", 0) : 0;
    g_plug.updated = last ? (int)json_num(last, "updated", 0) : 0;
    g_plug.running = last && json_bool(last, "running", false);
    g_plug.ok = true;
    g_dirty = true;
  }
  json_free(&d);
}

/* one game's plugins brought up to date now: the engine adds what the game needs, replaces older releases */
static void plugins_refresh_game(const game_t *g) {
  if (g->image) { toast(T(S_PLUG_SMP)); return; }
  char form[700] = "", msg[300];
  form_add(form, sizeof form, "path", g->path);
  jdoc d;
  int st = api_call("/api/plugins/refresh", form, &d);
  if (st == 200 && d.root) {
    int n = (int)json_num(d.root, "written", 0);
    if (n > 0) snprintf(msg, sizeof msg, T(S_PLUG_DONE), n);
    else scopy(msg, sizeof msg, T(S_PLUG_NOTHING));
    toast(msg);
    snd_cue_frame(SND_SELECT);
  } else {
    snprintf(msg, sizeof msg, T(S_PLUG_FAILED), st == 409 ? T(S_E_GAME_RUNNING) : d.root ? json_str(d.root, "error", json_str(d.root, "report", "?")) : "?");
    toast_err(msg);
  }
  json_free(&d);
  load_games();
}

/* ------------------------------------------------------- covers, wallpaper -- */
/* what identifies a game's pictures, and the engine queries for its cover and key art */
static void game_queries(const game_t *g, char *key, size_t kcap, char *art, size_t acap, char *cov, size_t ccap) {
  bool smp = g->image && g->smp[0];
  scopy(key, kcap, smp ? g->smp : g->path);
  /* the path and the title: the engine finds the pictures in the folder, the mounted image,
   * ShadowMountPlus or the console's own copy of the title's pictures (duplicates included) */
  art[0] = cov[0] = 0;
  form_add(cov, ccap, "path", g->path);
  if (g->title_id[0]) form_add(cov, ccap, "title_id", g->title_id);
  form_add(art, acap, "path", g->path);
  if (g->title_id[0]) form_add(art, acap, "title_id", g->title_id);
  form_add(art, acap, "kind", "bg");
}

static const image_t *cover_for(const game_t *g, uint64_t *at) {
  char key[620], art[1300], cov[1300];
  game_queries(g, key, sizeof key, art, sizeof art, cov, sizeof cov);
  return assets_cover(key, cov, at);
}

static const game_t *game_by_title(const char *tid) {
  for (int i = 0; tid[0] && i < g_ngames; i++)
    if (!strcmp(g_games[i].title_id, tid)) return &g_games[i];
  return NULL;
}

static int marked_count(void) {
  int n = 0;
  for (int i = 0; i < g_ngames; i++) n += g_games[i].marked;
  return n;
}

/* the game a screen is about: the first marked one, else the highlighted one */
static const game_t *focus_game(void) {
  if (!g_ngames) return NULL;
  for (int i = 0; i < g_ngames; i++)
    if (g_games[i].marked) return &g_games[i];
  return &g_games[g_sel];
}

/* the wallpaper follows the game in focus, once the selection rests a moment (fast scrolling
 * does not load every picture on the way) */
static void choose_wallpaper(uint64_t now) {
  static char want[620];
  static uint64_t since;
  const game_t *g = NULL;
  if (g_screen == SCR_MAIN && g_tab == TAB_GAMES && g_games_loaded && g_ngames) g = &g_games[g_sel];
  else if (g_screen == SCR_OPTIONS || g_screen == SCR_DEST || g_screen == SCR_CHECK) g = focus_game();
  else if (g_screen == SCR_MAIN && g_tab == TAB_PROGRESS && g_st.title_id[0]) g = game_by_title(g_st.title_id);
  char key[620] = "", art[1300] = "", cov[1300] = "";
  if (g) game_queries(g, key, sizeof key, art, sizeof art, cov, sizeof cov);
  if (strcmp(key, want) != 0) {
    scopy(want, sizeof want, key);
    since = now;
  }
  if (now - since >= 160) assets_wallpaper(want, art, cov);
}

/* ------------------------------------------------------------- actions -- */
static int start_one(const game_t *g) {
  char form[2048] = "";
  form_add(form, sizeof form, "path", g->path);
  if (g->image && g->smp[0]) form_add(form, sizeof form, "smp", g->smp);
  form_add(form, sizeof form, "dest", g_ndests ? g_dests[g_opt.dest].path : "");
  form_add_int(form, sizeof form, "level", g_opt.level);
  form_add_int(form, sizeof form, "workers", 6);
  form_add_int(form, sizeof form, "clean", g_opt.clean);
  form_add_int(form, sizeof form, "overwrite", g_opt.overwrite);
  form_add_int(form, sizeof form, "remove", g_opt.remove);
  form_add_int(form, sizeof form, "verify", g_opt.verify);
  form_add_int(form, sizeof form, "dedup", g_opt.dedup);
  form_add_int(form, sizeof form, "cache", g_opt.cache);
  jdoc d;
  int st = api_call("/api/start", form, &d);
  if (st != 200) {
    char m[300];
    const char *code = d.root ? json_str(d.root, "code", "") : "", *why = code[0] ? code_text(code) : NULL;
    snprintf(m, sizeof m, "%s : %s", g->title, why && why != code ? why : d.root ? json_str(d.root, "error", T(S_ERROR)) : T(S_ERROR));
    toast_err(m);
  }
  json_free(&d);
  return st == 200 ? 0 : -1;
}

static void start_conversions(void) {
  int ok = 0, n = marked_count();
  g_app_started_at = plat_ms();
  if (n == 0) ok += start_one(&g_games[g_sel]) == 0;
  else
    for (int i = 0; i < g_ngames; i++)
      if (g_games[i].marked && start_one(&g_games[i]) == 0) { ok++; g_games[i].marked = false; }
  settings_save();
  if (ok) {
    char m[200];
    snprintf(m, sizeof m, "%d × %s", ok, T(S_QUEUED));
    toast_kind(m, TOAST_OK);
    snd_cue_frame(SND_START);
    g_screen = SCR_MAIN;
    g_tab = TAB_PROGRESS;
  }
  poll_status();
}

static void confirm(int action, const char *text) {
  if (action != ACT_DELETE_SOURCE) g_confirm_detail[0] = 0;
  g_confirm = action;
  g_confirm_at = plat_ms();
  scopy(g_confirm_text, sizeof g_confirm_text, text);
  g_dirty = true;
}

static void run_confirmed(int action) {
  jdoc d;
  if (action == ACT_CANCEL_JOB) { api_call("/api/cancel", "", &d); json_free(&d); poll_status(); }
  else if (action == ACT_DISCARD && g_nres) {
    char form[2048] = "";
    form_add(form, sizeof form, "dest", g_res[g_res_sel].dest);
    api_call("/api/discard", form, &d);
    json_free(&d);
    load_resumable();
    g_games_loaded = false;
  } else if (action == ACT_START_REMOVE) start_conversions();
  else if (action == ACT_PAIR_RESET) { api_call("/api/pair/reset", "", &d); json_free(&d); load_pair(); }
  else if (action == ACT_UPDATE) update_install();
  else if (action == ACT_DELETE_SOURCE) {
    char form[1300] = "";
    form_add(form, sizeof form, "path", g_delete_path);
    int st = api_call("/api/delete-source", form, &d);
    if (st == 200) {
      char m[300];
      snprintf(m, sizeof m, T(S_DELETING), g_confirm_text);
      toast(m);
    } else toast_err(code_text(d.root ? json_str(d.root, "code", "unknown") : "unknown"));
    json_free(&d);
    poll_status();
  }
}

/* -------------------------------------------------------------- drawing -- */
static uint32_t mix_col(uint32_t a, uint32_t b, float t) {
  t = clampf(t, 0.0f, 1.0f);
  uint32_t out = 0;
  for (int sh = 0; sh < 32; sh += 8) {
    float ca = (float)((a >> sh) & 0xff), cb = (float)((b >> sh) & 0xff);
    out |= (uint32_t)(ca + (cb - ca) * t + 0.5f) << sh;
  }
  return out;
}

/* dark glass with a thin light edge */
static void panel(int x, int y, int w, int h) {
  gfx_round_rect(x, y, w, h, 28, C_PANEL);
  gfx_round_frame(x, y, w, h, 28, 1, C_PANEL_EDGE);
}

/* the highlighted row: blue glass, a glow that breathes slowly */
static void draw_focus(int x, int y, int w, int h, int r) {
  float p = anim_pulse(2400);
  gfx_glow(x, y, w, h, r, RGBA(64, 150, 255, (int)(40 + 36 * p)), 12);
  gfx_round_rect(x, y, w, h, r, C_SEL);
  gfx_round_frame(x, y, w, h, r, 3, RGBA(110, 180, 255, (int)(165 + 90 * p)));
}

/* rows arrive one after the other when a screen or a tab opens */
static void row_push(int i) {
  float e = ease_out(anim_t(g_tr_start + (uint64_t)(i > 0 ? i : 0) * 28u, 280));
  gfx_push((int)((1.0f - e) * 40.0f * (float)g_tr_dir), 0, (int)(255.0f * e));
}

static void draw_toggle(int x, int y, bool on, uint32_t color, anim_val *a) {
  float t = anim_track(a, on ? 1.0f : 0.0f, 16.0f, 0.01f);
  gfx_round_rect(x, y, 92, 46, 23, mix_col(C_PANEL_HI, color, t));
  gfx_circle(x + 23 + (int)(46.0f * t), y + 23, 18, RGB(255, 255, 255));
}

/* cover with a soft shadow; it fades in when it arrives, a placeholder until then */
/* No picture anywhere (never installed on this console, image not mounted): a tile drawn from the title,
 * its colour fixed by the title ID, its initials, so that every game and app always has a picture. Also
 * what shows while the real picture loads, under it. */
static void draw_tile(const game_t *g, int x, int y, int s, int r, bool label) {
  static const uint32_t pal[][2] = {
    {RGB(70, 110, 210), RGB(26, 40, 96)}, {RGB(160, 80, 200), RGB(60, 26, 100)}, {RGB(30, 165, 150), RGB(10, 64, 72)},
    {RGB(225, 120, 55), RGB(104, 42, 22)}, {RGB(205, 65, 100), RGB(90, 22, 48)}, {RGB(80, 160, 80), RGB(26, 66, 32)},
    {RGB(110, 120, 145), RGB(36, 42, 58)}, {RGB(215, 170, 55), RGB(100, 72, 18)}};
  const char *key = g->title_id[0] ? g->title_id : g->title;
  uint32_t h = 2166136261u;
  for (const char *p = key; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
  const uint32_t *c = pal[h % 8];
  gfx_round_gradient(x, y, s, s, r, c[0], c[1]);
  gfx_round_frame(x, y, s, s, r, s >= 200 ? 2 : 1, RGBA(255, 255, 255, 46));
  /* initials: the first character of the first two words (one is enough for Chinese, Japanese, Korean) */
  char ini[16] = "";
  size_t n = 0;
  int words = 0;
  bool start = true;
  for (const char *p = g->title[0] ? g->title : key; *p && words < 2;) {
    unsigned char ch = (unsigned char)*p;
    size_t len = ch < 0x80 ? 1 : ch >= 0xF0 ? 4 : ch >= 0xE0 ? 3 : ch >= 0xC0 ? 2 : 1;
    bool word = ch >= 0x80 || (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
    if (start && word) {
      if (n + len < sizeof ini) {
        memcpy(ini + n, p, len);
        if (len == 1 && ini[n] >= 'a' && ini[n] <= 'z') ini[n] = (char)(ini[n] - 32);
        n += len;
        ini[n] = 0;
      }
      words++;
      start = false;
      if (ch >= 0xE0) break;
    } else if (!word) start = true;
    for (size_t k = 0; k < len && *p; k++) p++;
  }
  const font_t *f = s >= 200 ? &FONT_TITLE : s >= 100 ? &FONT_HEAD : &FONT_BODY;
  bool sub = label && s >= 160 && g->title_id[0];
  int cy = y + s / 2 - f->line / 2 - (sub ? FONT_BODY.line / 2 + 4 : 0);
  gfx_text_center(f, x + s / 2, cy, ini[0] ? ini : "?", RGBA(255, 255, 255, 235));
  if (sub) gfx_text_center(&FONT_BODY, x + s / 2, cy + f->line + 8, g->title_id, RGBA(255, 255, 255, 150));
}

static void draw_cover(const game_t *g, int x, int y, int s, int r, bool label) {
  uint64_t at = 0;
  const image_t *img = cover_for(g, &at);
  float t = img ? ease_out(anim_t(at, 280)) : 0.0f;
  gfx_glow(x, y + 8, s, s, r, RGBA(0, 0, 0, 120), s / 16 + 6);
  if (t < 1.0f) draw_tile(g, x, y, s, r, label);
  if (img) gfx_image_a(img, x, y, s, s, r, (int)(255.0f * t));
}

/* Where a game is kept, from its path: the console's own storage, a USB drive (/mnt/usbN, numbered like
 * the destinations) or the M.2 SSD (/mnt/extN). full: the long name, else a short one for the list. */
static bool game_location(const char *path, bool full, char *out, size_t cap) {
  out[0] = 0;
  if (!strncmp(path, "/data/", 6) || !strncmp(path, "/user/", 6)) {
    scopy(out, cap, full ? T(S_DEST_INTERNAL) : T(S_LOC_INTERNAL));
    return true;
  }
  bool usb = !strncmp(path, "/mnt/usb", 8), ext = !strncmp(path, "/mnt/ext", 8);
  if ((!usb && !ext) || path[8] < '0' || path[8] > '9') return false;
  int n = path[8] - '0';
  if (usb && full) snprintf(out, cap, T(S_DEST_USB), n);
  else if (usb) snprintf(out, cap, "USB %d", n);
  else if (full) snprintf(out, cap, T(S_DEST_EXT), n);
  else if (n == 0) scopy(out, cap, "SSD M.2");
  else snprintf(out, cap, "M.2 (ext%d)", n);
  return true;
}

static void draw_hints(const int *icons, const str_id *labels, int n) {
  int x = 80, y = 1012;
  for (int i = 0; i < n; i++) {
    x += gfx_button(icons[i], x, y, 40) + 12;
    x += gfx_text(&FONT_BODY, x, y + 20 - FONT_BODY.line / 2, T(labels[i]), C_TEXT, 0) + 44;
  }
}

#include "storage_ui.inc"

/* what the engine reports, in the user's language: its result code first, then (older engines)
 * its English message recognized by a few words, else the message as it is */
static const char *engine_text(void) {
  const char *c = g_st.code;
  if (!strcmp(c, "smp_busy")) {
    const char *r = g_st.reason;
    if (!r[0] || !strcmp(r, "game_active")) return T(S_X_SMP_GAME_ACTIVE);   /* before 1.7beta5: no reason, the app is the usual cause */
    if (!strcmp(r, "runtime_prepared")) return T(S_X_SMP_PREPARED);
    if (!strcmp(r, "rest_mode")) return T(S_X_SMP_REST);
    if (!strcmp(r, "storage_unavailable")) return T(S_X_SMP_STORAGE);
    return T(S_X_SMP_BUSY);
  }
  if (c[0]) {
    const char *t = code_text(c);
    if (t != c) return t;
  }
  const char *msg = g_st.message;
  if (!strncmp(msg, "game launched", 13)) return T(S_PAUSE_GAME);
  if (strstr(msg, "destination storage disconnected") || strstr(msg, "storage disconnected or read-only")) return T(S_PAUSE_DEST);
  if (strstr(msg, "source disconnected")) return T(S_PAUSE_SRC);
  if (strstr(msg, "destination full")) return T(S_PAUSE_FULL);
  if (strstr(msg, "this game is running")) return T(S_E_GAME_RUNNING);
  if (strstr(msg, "does not use AMPR")) return T(S_COMPAT_NO);
  if (strstr(msg, "destination storage not")) return T(S_E_DEST_MISSING);
  if (strstr(msg, "ShadowMountPlus is busy")) return T(S_X_SMP_GAME_ACTIVE);
  if (strstr(msg, "ShadowMountPlus API unreachable")) return T(S_X_SMP_UNREACHABLE);
  if (strstr(msg, "does not manage this game")) return T(S_X_SMP_NOT_MANAGED);
  if (!strcmp(g_st.state, "cancelled")) return T(S_X_CANCELLED);
  return msg;
}

/* "v1.137" for a numbered version, other ones ("dev") as they are */
static const char *ver_label(const char *v, char *buf, size_t cap) {
  if (v[0] >= '0' && v[0] <= '9') snprintf(buf, cap, "v%s", v);
  else snprintf(buf, cap, "%s", v);
  return buf;
}

/* the version of this app: the engine it carries is built from the same commit */
static const char *app_version(void) { return g_boot_want[0] ? g_boot_want : "dev"; }

static void draw_header(void) {
  char ver[80];
  int tw = gfx_text(&FONT_TITLE, 80, 34, "exFAT2AMPR", C_TEXT, 0);
  gfx_text(&FONT_BODY, 80 + tw + 18, 34 + FONT_TITLE.ascent - FONT_BODY.ascent, ver_label(app_version(), ver, sizeof ver), C_DIM, 0);
  gfx_text(&FONT_BODY, 82, 98, T(S_APP_SUB), C_DIM, 0);
  /* engine / job pill on the right */
  char pill[200];
  uint32_t col = C_DIM;
  bool running = !strcmp(g_st.state, "running");
  if (!g_st.ok) snprintf(pill, sizeof pill, "%s : —", T(S_ENGINE));
  else if (running && g_st.game_mode) { snprintf(pill, sizeof pill, "%s · %d %%", T(S_GAME_MODE_ON), g_st.percent); col = C_WARN; }
  else if (running && g_st.waiting) { snprintf(pill, sizeof pill, "%s", T(S_WAITING_SHORT)); col = C_WARN; }
  else if (running) { snprintf(pill, sizeof pill, "%s %d %%", T(S_RUNNING), g_st.percent); col = C_ACCENT; }
  else if (!strcmp(g_st.state, "error")) { snprintf(pill, sizeof pill, "%s", T(S_ERROR)); col = C_ERR; }
  else if (!strcmp(g_st.state, "paused")) { snprintf(pill, sizeof pill, "%s", T(S_PAUSED)); col = C_WARN; }
  else snprintf(pill, sizeof pill, "%s %s", T(S_ENGINE), ver_label(g_st.version, ver, sizeof ver));
  int w = gfx_text_width(&FONT_BODY, pill) + 68;
  gfx_round_rect(1840 - w, 50, w, 56, 28, C_PANEL);
  gfx_round_frame(1840 - w, 50, w, 56, 28, 1, C_PANEL_EDGE);
  if (running) {   /* a ripple around the light while it works */
    float p = 1.0f - anim_pulse(1500);
    gfx_circle(1840 - w + 24, 78, 7 + (int)(9.0f * p), WITH_ALPHA(col, (int)(110.0f * (1.0f - p))));
  }
  gfx_circle(1840 - w + 24, 78, 7, col);
  gfx_text(&FONT_BODY, 1840 - w + 40, 78 - FONT_BODY.line / 2, pill, C_TEXT, 0);
}

static void draw_tabs(void) {
  static const str_id names[TAB_COUNT] = {S_GAMES, S_PROGRESS, S_INTERRUPTED, S_SETTINGS};
  static anim_val pill_x, pill_w;
  int x = 80, y = 150, tx[TAB_COUNT], tw[TAB_COUNT];
  gfx_button(ICON_L1, x, y + 4, 40);
  x += 90;
  for (int i = 0; i < TAB_COUNT; i++) {
    tw[i] = gfx_text_width(&FONT_HEAD, T(names[i])) + 56;
    tx[i] = x;
    x += tw[i] + 16;
  }
  /* the blue pill slides to the new tab; the labels light up as it passes */
  float px = anim_track(&pill_x, (float)tx[g_tab], 18.0f, 0.5f), pw = anim_track(&pill_w, (float)tw[g_tab], 18.0f, 0.5f);
  gfx_glow((int)px, y, (int)pw, 52, 26, RGBA(64, 150, 255, 64), 10);
  gfx_round_rect((int)px, y, (int)pw, 52, 26, C_ACCENT);
  for (int i = 0; i < TAB_COUNT; i++) {
    float d = (px - (float)tx[i]) / (float)tw[i];
    float lit = 1.0f - clampf(d < 0 ? -d : d, 0.0f, 1.0f);
    gfx_text(&FONT_HEAD, tx[i] + 28, y + 26 - FONT_HEAD.line / 2, T(names[i]), mix_col(C_DIM, RGB(255, 255, 255), lit), 0);
  }
  gfx_button(ICON_R1, x + 8, y + 4, 40);
}

static void draw_checkbox(int x, int y, float on, bool enabled) {
  gfx_round_frame(x, y, 34, 34, 8, 3, enabled ? C_TEXT : RGBA(255, 255, 255, 60));
  int s = (int)(20.0f * ease_out_back(on));
  if (s > 0 && on > 0.01f) gfx_round_rect(x + 17 - s / 2, y + 17 - s / 2, s, s, s / 4, C_ACCENT);
}

static void draw_games(void) {
  const int lx = 80, ly = 230, lw = 1060, row = 88, rows = 8;
  panel(lx, ly, lw, row * rows + 24);
  if (!g_games_loaded) { gfx_text(&FONT_BODY, lx + 40, ly + 40, T(S_LOADING), C_DIM, 0); return; }
  if (!g_ngames) { gfx_text_wrap(&FONT_BODY, lx + 40, ly + 40, T(S_NO_GAMES), C_DIM, lw - 80, 3); return; }
  if (g_sel >= g_ngames) g_sel = g_ngames - 1;
  if (g_sel < g_scroll) g_scroll = g_sel;
  if (g_sel >= g_scroll + rows) g_scroll = g_sel - rows + 1;
  static anim_val scroll_v, sel_v, focus_v[MAX_GAMES], mark_v[MAX_GAMES];
  float sc = anim_track(&scroll_v, (float)g_scroll, 16.0f, 0.002f);
  float sv = anim_track(&sel_v, (float)g_sel, 22.0f, 0.002f);
  gfx_push(0, 0, 255);
  gfx_clip(lx, ly + 6, lw, row * rows + 12);
  row_push(g_sel - g_scroll);
  draw_focus(lx + 12, ly + 12 + (int)((sv - sc) * (float)row), lw - 24, row - 8, 20);
  gfx_pop();
  for (int k = (int)sc - 1; k <= (int)sc + rows; k++) {
    if (k < 0 || k >= g_ngames) continue;
    const game_t *g = &g_games[k];
    int y = ly + 12 + (int)(((float)k - sc) * (float)row);
    int fx = (int)(10.0f * anim_track(&focus_v[k], k == g_sel ? 1.0f : 0.0f, 14.0f, 0.01f));
    float mk = anim_track(&mark_v[k], g->marked ? 1.0f : 0.0f, 16.0f, 0.01f);
    row_push(k - g_scroll);
    draw_checkbox(lx + 36 + fx, y + 22, mk, g->selectable);
    draw_cover(g, lx + 86 + fx, y + 10, 64, 12, false);   /* every game and app shows its picture */
    uint32_t tc = g->selectable ? C_TEXT : C_DIM;
    gfx_text(&FONT_HEAD, lx + 166 + fx, y + 8, g->title, tc, lw - 210 - fx);
    char sub[300], sz[32] = "", where[64];
    if (g->size > 0) fmt_bytes(sz, sizeof sz, (double)g->size);
    bool known = game_location(g->path, false, where, sizeof where);
    snprintf(sub, sizeof sub, "%s · %s%s%s%s%s%s%s", g->title_id[0] ? g->title_id : "—", g->image ? T(S_IMAGE) : T(S_FOLDER),
             known ? " · " : "", where, g->version[0] ? " · v" : "", g->version, sz[0] ? " · " : "", sz);
    int w = gfx_text(&FONT_SMALL, lx + 168 + fx, y + 50, sub, C_DIM, 0);
    bool playing = g->title_id[0] && !strcmp(g->title_id, g_st.game_running);
    const char *tag = playing ? T(S_RUNNING_NOW)
                      : !g->selectable ? (!strcmp(g->reason, "smp_offline") ? T(S_SMP_OFFLINE) : T(S_NOT_MANAGED))
                      : g->partial ? T(S_PARTIAL) : g->ampr ? T(S_ALREADY_AMPR) : g->dup ? T(S_DUPLICATE) : NULL;
    if (tag) gfx_text(&FONT_SMALL, lx + 168 + fx + w + 18, y + 50, tag, g->selectable && !playing ? C_WARN : C_ERR, lw - 230 - w - fx);
    gfx_pop();
  }
  gfx_pop();
  /* scroll bar */
  if (g_ngames > rows) {
    int th = (row * rows) * rows / g_ngames, ty = ly + 12 + (int)((float)(row * rows - th) * sc / (float)(g_ngames - rows));
    gfx_round_rect(lx + lw - 10, ty, 5, th, 3, C_PANEL_HI);
  }
  /* details: the new game's cover rises and fades in */
  const game_t *g = &g_games[g_sel];
  static int last_sel = -1;
  if (g_sel != last_sel) { last_sel = g_sel; g_sel_changed_at = g_now; }
  float e = ease_out(anim_t(g_sel_changed_at, 220));
  const int dx = 1180, dw = 660;
  panel(dx, ly, dw, row * rows + 24);
  gfx_push(0, (int)((1.0f - e) * 16.0f), (int)(255.0f * (0.3f + 0.7f * e)));
  draw_cover(g, dx + (dw - 360) / 2, ly + 36, 360, 28, true);
  int y = ly + 420;
  gfx_text(&FONT_HEAD, dx + 36, y, g->title, C_TEXT, dw - 72);
  y += 52;
  char line[700];
  snprintf(line, sizeof line, "%s%s%s", g->title_id, g->version[0] ? "  ·  v" : "", g->version);
  gfx_text(&FONT_BODY, dx + 36, y, line, C_DIM, dw - 72);
  y += 44;
  char where[96];
  if (game_location(g->path, true, where, sizeof where)) {
    snprintf(line, sizeof line, T(S_LOCATION), where);
    gfx_text(&FONT_BODY, dx + 36, y, line, C_ACCENT, dw - 72);
    y += 44;
  }
  gfx_text(&FONT_SMALL, dx + 36, y, g->path, C_DIM, dw - 72);
  y += 44;
  int marked = marked_count();
  if (marked) {
    snprintf(line, sizeof line, "%d %s", marked, T(S_GAMES_SELECTED));
    gfx_text(&FONT_BODY, dx + 36, y, line, C_ACCENT, dw - 72);
  }
  gfx_pop();
}

static void stat_card(int x, int y, int w, const char *label, const char *value, uint32_t vc) {
  gfx_round_rect(x, y, w, 112, 22, RGBA(255, 255, 255, 14));
  gfx_round_frame(x, y, w, 112, 22, 1, C_PANEL_EDGE);
  gfx_text(&FONT_SMALL, x + 28, y + 20, label, C_DIM, w - 56);
  gfx_text(&FONT_HEAD, x + 28, y + 56, value, vc, w - 56);
}

/* green disc that pops, then a tick that draws itself */
static void draw_check_badge(int cx, int cy, int r, float t) {
  int rr = (int)((float)r * ease_out_back(clampf(t / 0.6f, 0.0f, 1.0f)));
  if (rr <= 2) return;
  gfx_glow(cx - rr, cy - rr, 2 * rr, 2 * rr, rr, RGBA(80, 210, 140, 80), 16);
  gfx_circle(cx, cy, rr, C_OK);
  float d = clampf((t - 0.4f) / 0.5f, 0.0f, 1.0f);
  int ax = cx - rr * 45 / 100, ay = cy + rr * 2 / 100, bx = cx - rr * 12 / 100, by = cy + rr * 34 / 100;
  int ex = cx + rr * 48 / 100, ey = cy - rr * 32 / 100, th = rr / 5 > 3 ? rr / 5 : 3;
  const float split = 0.38f;
  if (d > 0) {
    float k = clampf(d / split, 0.0f, 1.0f);
    gfx_line(ax, ay, ax + (int)((float)(bx - ax) * k), ay + (int)((float)(by - ay) * k), th, RGB(255, 255, 255));
  }
  if (d > split) {
    float k = (d - split) / (1.0f - split);
    gfx_line(bx, by, bx + (int)((float)(ex - bx) * k), by + (int)((float)(ey - by) * k), th, RGB(255, 255, 255));
  }
}

static void draw_progress(void) {
  const int x = 80, y = 230, w = 1760, h = 730;
  panel(x, y, w, h);
  if (!g_st.ok) { gfx_text(&FONT_BODY, x + 40, y + 40, T(S_TIMEOUT), C_ERR, w - 80); return; }
  bool running = !strcmp(g_st.state, "running"), paused = !strcmp(g_st.state, "paused"), done = !strcmp(g_st.state, "done");
  bool error = !strcmp(g_st.state, "error"), cancelled = !strcmp(g_st.state, "cancelled"), waiting = running && g_st.waiting;
  if (!running && !paused && !strcmp(g_st.state, "idle") && !g_st.title[0]) {
    row_push(0);
    gfx_text(&FONT_HEAD, x + 40, y + 40, T(S_NOTHING_RUNNING), C_DIM, 0);
    gfx_text_wrap(&FONT_BODY, x + 40, y + 100, T(S_CONTINUES), C_DIM, w - 80, 2);
    gfx_pop();
    return;
  }
  row_push(0);
  const game_t *cg = game_by_title(g_st.title_id);
  if (cg) draw_cover(cg, x + 40, y + 40, 220, 22, false);
  else gfx_round_rect(x + 40, y + 40, 220, 220, 22, C_PANEL_HI);
  int tx = x + 300, tw = w - 340 - 330;
  /* an image waits for ShadowMountPlus before its title is known: name it from the queue entry */
  const char *name = g_st.title[0] ? g_st.title : g_st.title_id[0] ? g_st.title_id : waiting ? T(S_WAITING) : "";
  gfx_text(&FONT_TITLE, tx, y + 40, name, C_TEXT, tw);
  uint32_t sc = error ? C_ERR : done ? C_OK : paused || waiting ? C_WARN : cancelled ? C_DIM : C_ACCENT;
  char line[600];
  if (waiting) scopy(line, sizeof line, T(S_WAITING));
  else snprintf(line, sizeof line, "%s — %s", state_label(g_st.state), phase_label(g_st.phase));
  gfx_text(&FONT_HEAD, tx, y + 116, line, sc, tw);
  /* why, and what to do: in the chosen language, never the engine's English */
  const char *msg = engine_text();
  if (error || cancelled || paused || waiting || done)
    gfx_text_wrap(&FONT_BODY, tx, y + 166, msg, error ? RGB(255, 178, 178) : paused || waiting ? RGB(255, 214, 150) : done ? C_OK : C_DIM, tw, 3);
  else gfx_text_wrap(&FONT_SMALL, tx, y + 170, msg, C_DIM, w - 340, 2);   /* the file being packed */
  gfx_pop();

  /* percentage: counts smoothly between the engine's updates; left alone for a while, the bar
   * jumps instead, so an open app does not redraw all the time during a long conversion */
  static anim_val bar_v;
  int pct = g_st.percent < 0 ? 0 : g_st.percent > 100 ? 100 : g_st.percent;
  float pv = anim_track(&bar_v, (float)pct, anim_ambient_allowed() ? 2.6f : 1000.0f, 0.05f);
  row_push(1);
  if (done && g_done_at) {
    draw_check_badge(x + w - 170, y + 104, 62, anim_t(g_done_at, 1000));
    gfx_text_center(&FONT_SMALL, x + w - 170, y + 186, T(S_DONE_TITLE), C_OK);
  } else if (waiting) {   /* turns while it waits (for the first seconds after a button press) */
    float ph = anim_ambient() ? (float)(g_now % 1200) / 1200.0f : 0.0f;
    gfx_spinner(x + w - 170, y + 100, 40, 7, ph, C_WARN);
    gfx_text_center(&FONT_SMALL, x + w - 170, y + 186, T(S_WAITING_SHORT), C_WARN);
  } else if (error || cancelled) {
    float e = ease_out_back(clampf(anim_t(g_err_at, 600) / 0.7f, 0.0f, 1.0f));
    int r = (int)(62.0f * e), cx = x + w - 170, cy = y + 104;
    if (r > 2) {
      uint32_t bc = error ? C_ERR : RGB(120, 128, 146);
      gfx_glow(cx - r, cy - r, 2 * r, 2 * r, r, WITH_ALPHA(bc, 80), 16);
      gfx_circle(cx, cy, r, bc);
      if (error) {   /* "!" */
        gfx_line(cx, cy - r * 45 / 100, cx, cy + r * 12 / 100, r / 5, RGB(255, 255, 255));
        gfx_circle(cx, cy + r * 38 / 100, r / 9 + 1, RGB(255, 255, 255));
      } else {       /* "x" */
        gfx_line(cx - r / 3, cy - r / 3, cx + r / 3, cy + r / 3, r / 6, RGB(255, 255, 255));
        gfx_line(cx - r / 3, cy + r / 3, cx + r / 3, cy - r / 3, r / 6, RGB(255, 255, 255));
      }
    }
    gfx_text_center(&FONT_SMALL, cx, y + 186, state_label(g_st.state), error ? C_ERR : C_DIM);
  } else {
    snprintf(line, sizeof line, "%d %%", (int)(pv + 0.5f));
    gfx_text_right(&FONT_HUGE, x + w - 40, y + 34, line, sc);
  }
  /* the bar: glow, gloss, and a light sweeping across while it works */
  int bx = x + 40, by = y + 296, bw = w - 80, bh = 30;
  gfx_round_rect(bx, by, bw, bh, bh / 2, C_PANEL_HI);
  int fw = (int)((float)bw * pv / 100.0f);
  if (pv > 0.05f && fw < bh) fw = bh;
  if (fw > 0) {
    gfx_glow(bx, by, fw, bh, bh / 2, WITH_ALPHA(sc, 56), 12);
    gfx_round_rect(bx, by, fw, bh, bh / 2, sc);
    if (fw > 24) gfx_round_rect(bx + 8, by + 4, fw - 16, bh / 3, bh / 6, RGBA(255, 255, 255, 46));
    if (running && anim_ambient()) {
      int span = fw + 260, pos = (int)(g_now % 1700 * (uint64_t)span / 1700) - 130;
      gfx_push(0, 0, 255);
      gfx_clip(bx, by, fw, bh);
      for (int i = -60; i <= 60; i++) {
        int a = 64 - (i < 0 ? -i : i) * 64 / 60;
        if (a > 0) gfx_rect(bx + pos + i, by, 1, bh, RGBA(255, 255, 255, a));
      }
      gfx_pop();
    }
  }
  gfx_pop();

  char a[32], b[32], c[48], d[48], v1[80];
  fmt_bytes(a, sizeof a, g_st.done);
  fmt_bytes(b, sizeof b, g_st.total);
  fmt_bytes(c, sizeof c, g_st.speed);
  fmt_dur(d, sizeof d, g_st.eta);
  snprintf(v1, sizeof v1, "%s / %s", a, b);
  snprintf(line, sizeof line, "%s/s", c);
  int cw = (bw - 48) / 3, cy = by + 64;
  row_push(2);
  stat_card(bx, cy, cw, T(S_PROGRESS), v1, C_TEXT);
  stat_card(bx + cw + 24, cy, cw, T(S_SPEED), running && !waiting ? line : "—", C_TEXT);   /* no speed or time left while paused */
  stat_card(bx + 2 * (cw + 24), cy, cw, T(S_ETA), running && !waiting ? d : "—", C_TEXT);
  if (done && g_st.source_bytes > 0) {
    char s1[32], s2[32];
    fmt_bytes(s1, sizeof s1, g_st.source_bytes);
    fmt_bytes(s2, sizeof s2, g_st.packed_bytes);
    snprintf(line, sizeof line, "%s → %s (%s)", s1, s2, T(S_PACKED));
    gfx_text(&FONT_BODY, bx, cy + 128, line, C_OK, bw);
  }
  gfx_pop();
  /* queue */
  row_push(3);
  int qy = cy + 172;
  snprintf(line, sizeof line, "%s (%d)", T(S_QUEUE), g_st.queue_len);
  gfx_text(&FONT_HEAD, x + 40, qy, line, C_TEXT, 0);
  for (int i = 0; i < g_st.queue_len && i < 2; i++) gfx_text(&FONT_BODY, x + 60, qy + 52 + i * 40, g_st.queue[i], C_DIM, w - 120);
  if (g_st.queue_len > 2) {
    snprintf(line, sizeof line, "+ %d", g_st.queue_len - 2);
    gfx_text(&FONT_BODY, x + 60, qy + 132, line, C_DIM, 0);
  }
  if (running || paused) gfx_text(&FONT_SMALL, x + 40, y + h - 46, T(S_CONTINUES), C_DIM, w - 80);   /* only while there is something to continue */
  gfx_pop();
}

static void draw_resume(void) {
  const int x = 80, y = 230, w = 1760, row = 104, rows = 6;
  panel(x, y, w, 730);
  if (!g_nres) { row_push(0); gfx_text(&FONT_BODY, x + 40, y + 40, T(S_RESUME_NONE), C_DIM, 0); gfx_pop(); return; }
  int scroll = g_res_sel >= rows ? g_res_sel - rows + 1 : 0;
  static anim_val sel_v;
  float sv = anim_track(&sel_v, (float)(g_res_sel - scroll), 22.0f, 0.002f);
  row_push(g_res_sel - scroll);
  draw_focus(x + 12, y + 16 + (int)(sv * (float)row), w - 24, row - 10, 20);
  gfx_pop();
  for (int i = 0; i < rows && scroll + i < g_nres; i++) {
    const resum_t *r = &g_res[scroll + i];
    int ry = y + 16 + i * row;
    row_push(i);
    gfx_text(&FONT_HEAD, x + 40, ry + 10, r->name[0] ? r->name : r->dest, C_TEXT, w - 80);
    char line[1400];
    snprintf(line, sizeof line, "%s : %s · LZ4 %d · %s%s%s", T(S_STAGE), r->stage, r->level, r->source, r->available ? "" : " · ",
             r->available ? "" : T(S_SOURCE_MISSING));
    gfx_text(&FONT_SMALL, x + 42, ry + 58, line, r->available ? C_DIM : C_WARN, w - 90);
    gfx_pop();
  }
}

static void draw_settings(void) {
  const int x = 80, y = 230, w = 1060, h = 730, row = 88;
  panel(x, y, w, h);
  const char *labels[SET_COUNT] = {T(S_LANGUAGE), T(S_GAME_MODE), T(S_SOUNDS), T(S_ANIMS), T(S_PAIR), T(S_PLUGINS), T(S_UPDATES), T(S_QUIT_APP)};
  static anim_val sel_v, tog[3];
  float sv = anim_track(&sel_v, (float)g_set_sel, 22.0f, 0.002f);
  row_push(g_set_sel);
  draw_focus(x + 12, y + 24 + (int)(sv * (float)row), w - 24, row - 14, 20);
  gfx_pop();
  for (int i = 0; i < SET_COUNT; i++) {
    int ry = y + 24 + i * row;
    row_push(i);
    gfx_text(&FONT_HEAD, x + 40, ry + 25, labels[i], C_TEXT, i == SET_LANG ? 520 : w - 220);
    if (i == SET_LANG) {   /* the language name slides in from the side it came from */
      float e = ease_out(anim_t(g_lang_at, 240));
      char v[64];
      snprintf(v, sizeof v, "‹  %s  ›", lang_name(i18n_get()));
      gfx_push((int)((1.0f - e) * 28.0f * (float)g_lang_dir), 0, (int)(255.0f * (0.2f + 0.8f * e)));
      gfx_text_right(&FONT_HEAD, x + w - 40, ry + 25, v, C_ACCENT);
      gfx_pop();
    } else if (i == SET_GAME_MODE) draw_toggle(x + w - 140, ry + 20, g_st.game_mode_enabled, C_ACCENT, &tog[0]);
    else if (i == SET_SOUNDS) draw_toggle(x + w - 140, ry + 20, snd_enabled(), C_ACCENT, &tog[1]);
    else if (i == SET_ANIMS) draw_toggle(x + w - 140, ry + 20, g_anim_enabled, C_ACCENT, &tog[2]);
    else if (i == SET_PAIR) gfx_text_right(&FONT_HEAD, x + w - 44, ry + 25, "›", C_DIM);
    else if (i == SET_PLUGINS) {   /* the embedded releases and the last pass over the games */
      char v[160];
      if (!g_plug.ok) scopy(v, sizeof v, "…");
      else if (g_plug.running) scopy(v, sizeof v, T(S_PLUG_RUNNING));
      else snprintf(v, sizeof v, T(S_PLUG_STATUS), g_plug.pgo, g_plug.dlc, g_plug.games, g_plug.updated);
      gfx_text_right(&FONT_BODY, x + w - 40, ry + 30, v, g_plug.running ? C_ACCENT : C_DIM);
    } else if (i == SET_UPDATE) {   /* where the app's own update stands */
      int pct = 0;
      char ver[32], v[96];
      upd_state us = update_state(&pct, ver, sizeof ver, NULL, NULL, 0);
      uint32_t c = C_DIM;
      switch (us) {
        case UPD_LATEST: scopy(v, sizeof v, T(S_UPD_LATEST)); c = C_OK; break;
        case UPD_AVAILABLE: snprintf(v, sizeof v, T(S_UPD_AVAILABLE), ver); c = C_ACCENT; break;
        case UPD_DOWNLOADING: snprintf(v, sizeof v, T(S_UPD_DOWNLOADING), pct); c = C_ACCENT; break;
        case UPD_INSTALLED: scopy(v, sizeof v, T(S_UPD_RESTART)); c = C_OK; break;
        case UPD_UNREACHABLE: scopy(v, sizeof v, T(S_UPD_UNREACHABLE)); break;
        case UPD_FAILED: {   /* the reason, cut for the row; the retry dialog shows it in full */
          char why[200];
          update_state(NULL, NULL, 0, NULL, why, sizeof why);
          if (strlen(why) > 44) scopy(why + 41, 4, "…");
          snprintf(v, sizeof v, T(S_UPD_FAILED), why[0] ? why : "?");
          c = C_ERR;
          break;
        }
        default: scopy(v, sizeof v, T(S_UPD_CHECKING)); break;
      }
      gfx_text_right(&FONT_BODY, x + w - 40, ry + 30, v, c);
    }
    gfx_pop();
  }
  /* right: the web page and its pairing code, the engine, the licence */
  const int ix = 1180, iw = 660;
  panel(ix, y, iw, h);
  row_push(1);
  char line[300];
  gfx_text(&FONT_SMALL, ix + 36, y + 36, T(S_WEB), C_DIM, iw - 72);
  snprintf(line, sizeof line, "http://%s:%d", g_ip[0] ? g_ip : "<IP PS5>", ENGINE_PORT);
  gfx_text(&FONT_HEAD, ix + 36, y + 72, line, C_ACCENT, iw - 72);
  gfx_text(&FONT_SMALL, ix + 36, y + 150, T(S_PAIR_CODE), C_DIM, iw - 72);
  for (int i = 0; i < 6 && g_pin[i]; i++) {
    int bx = ix + 36 + i * 96;
    gfx_round_rect(bx, y + 188, 82, 104, 18, C_PANEL_HI);
    char dch[2] = {g_pin[i], 0};
    gfx_text_center(&FONT_TITLE, bx + 41, y + 240 - FONT_TITLE.line / 2, dch, C_TEXT);
  }
  snprintf(line, sizeof line, "%d %s", g_pair_devices, T(S_DEVICES));
  gfx_text(&FONT_BODY, ix + 36, y + 318, line, g_pair_devices ? C_OK : C_DIM, iw - 72);
  /* this app's version; the engine's too when another one answers (started by hand, or still converting) */
  char ver[80], ever[80];
  bool other = g_st.ok && strcmp(g_st.version, app_version()) != 0;
  if (other)
    snprintf(line, sizeof line, "%s %s  ·  %s %s", T(S_VERSION), ver_label(app_version(), ver, sizeof ver), T(S_ENGINE),
             ver_label(g_st.version, ever, sizeof ever));
  else snprintf(line, sizeof line, "%s %s", T(S_VERSION), ver_label(app_version(), ver, sizeof ver));
  gfx_text(&FONT_BODY, ix + 36, y + 400, line, other ? C_WARN : C_DIM, iw - 72);
  gfx_text(&FONT_SMALL, ix + 36, y + 446, T(S_CREDITS), C_TEXT, iw - 72);
  gfx_text_wrap(&FONT_SMALL, ix + 36, y + 492, T(S_ABOUT), C_DIM, iw - 72, 4);
  gfx_pop();
}

enum { OPT_DEST, OPT_LEVEL, OPT_CACHE, OPT_VERIFY, OPT_DEDUP, OPT_CLEAN, OPT_OVERWRITE, OPT_REMOVE, OPT_START, OPT_COUNT };

/* the author's advice on each option, shown next to its value with the reason below the list; the defaults
 * are the recommended values, the user keeps the last word */
enum { RECO_NONE, RECO_YES, RECO_LESS };
static int opt_reco(int i, str_id *why) {
  switch (i) {
    case OPT_LEVEL:
      if (g_opt.level == 9) { *why = S_R_LEVEL_9; return RECO_YES; }
      if (g_opt.level >= 10) { *why = S_R_LEVEL_HI; return RECO_LESS; }
      if (g_opt.level <= 4) { *why = S_R_LEVEL_LOW; return RECO_LESS; }
      *why = S_R_LEVEL_MID;
      return RECO_NONE;
    case OPT_CACHE: *why = g_opt.cache ? S_R_CACHE_LARGE : S_R_CACHE_STD; return g_opt.cache ? RECO_NONE : RECO_YES;
    case OPT_VERIFY: *why = g_opt.verify ? S_R_VERIFY_ON : S_R_VERIFY_OFF; return g_opt.verify ? RECO_YES : RECO_LESS;
    case OPT_DEDUP: *why = g_opt.dedup ? S_R_DEDUP_ON : S_R_DEDUP_OFF; return g_opt.dedup ? RECO_YES : RECO_LESS;
    case OPT_CLEAN: *why = g_opt.clean ? S_R_CLEAN_ON : S_R_CLEAN_OFF; return g_opt.clean ? RECO_YES : RECO_LESS;
    case OPT_OVERWRITE: *why = g_opt.overwrite ? S_R_OVERWRITE_ON : S_R_OVERWRITE_OFF; return g_opt.overwrite ? RECO_LESS : RECO_YES;
    case OPT_REMOVE: *why = g_opt.remove ? S_R_REMOVE_ON : S_R_REMOVE_OFF; return g_opt.remove ? RECO_LESS : RECO_YES;
    default: *why = S_R_DEST; return RECO_NONE;
  }
}

static void draw_options(void) {
  static const str_id labels[OPT_COUNT] = {S_DESTINATION, S_LEVEL, S_CACHE, S_VERIFY, S_DEDUP, S_CLEAN, S_OVERWRITE, S_REMOVE, S_START};
  const int x = 80, y = 150, w = 1180, row = 84;
  static anim_val sel_v, tog_v[OPT_COUNT], start_v;
  gfx_text(&FONT_TITLE, x, 60, T(S_OPTIONS_TITLE), C_TEXT, 0);
  panel(x, y + 20, w, row * OPT_COUNT + 30);
  /* the focus glides; on the start button the button itself lights up instead */
  float sv = anim_track(&sel_v, (float)g_opt_sel, 22.0f, 0.002f), onto_start = clampf(sv - (float)(OPT_START - 1), 0.0f, 1.0f);
  if (onto_start < 1.0f) {
    gfx_push(0, 0, (int)(255.0f * (1.0f - onto_start)));
    draw_focus(x + 12, y + 34 + (int)(sv * (float)row), w - 24, row - 8, 20);
    gfx_pop();
  }
  for (int i = 0; i < OPT_COUNT; i++) {
    int ry = y + 34 + i * row;
    bool sel = i == g_opt_sel;
    row_push(i);
    if (i == OPT_START) {
      float f = anim_track(&start_v, sel ? 1.0f : 0.0f, 14.0f, 0.01f);
      if (f > 0.01f) gfx_glow(x + 24, ry + 4, w - 48, row - 14, 24, RGBA(64, 150, 255, (int)(110.0f * f * (0.6f + 0.4f * anim_pulse(1600)))), 16);
      gfx_round_rect(x + 24, ry + 4, w - 48, row - 14, 24, mix_col(C_PANEL_HI, C_ACCENT, f));
      gfx_text_center(&FONT_HEAD, x + w / 2, ry + 4 + (row - 14) / 2 - FONT_HEAD.line / 2, T(S_START), RGB(255, 255, 255));
      gfx_pop();
      continue;
    }
    gfx_text(&FONT_HEAD, x + 40, ry + 18, T(labels[i]), C_TEXT, i >= OPT_VERIFY ? w - 240 : 470);
    char v[200] = "";
    switch (i) {
      case OPT_DEST:
        if (!g_ndests) scopy(v, sizeof v, T(S_NO_DEST));
        else {
          char f[32];
          fmt_bytes(f, sizeof f, g_dests[g_opt.dest].free);
          char name[96];
          dest_name(&g_dests[g_opt.dest], name, sizeof name);
          snprintf(v, sizeof v, "%s · %s %s  ›", name, f, T(S_FREE));
        }
        break;
      case OPT_LEVEL: snprintf(v, sizeof v, "‹ %d ›", g_opt.level); break;
      case OPT_CACHE: snprintf(v, sizeof v, "‹ %s ›", g_opt.cache ? T(S_CACHE_LARGE) : T(S_CACHE_STD)); break;
      default: break;
    }
    if (i >= OPT_VERIFY && i <= OPT_REMOVE) {
      bool on = i == OPT_VERIFY ? g_opt.verify : i == OPT_DEDUP ? g_opt.dedup : i == OPT_CLEAN ? g_opt.clean : i == OPT_OVERWRITE ? g_opt.overwrite : g_opt.remove;
      draw_toggle(x + w - 140, ry + 16, on, i == OPT_REMOVE ? C_WARN : C_ACCENT, &tog_v[i]);
    } else gfx_text_right(&FONT_BODY, x + w - 40, ry + 22, v, C_ACCENT);
    {   /* the advice: a small green or amber word next to the value */
      str_id why;
      int r = opt_reco(i, &why);
      if (r != RECO_NONE)
        gfx_text_right(&FONT_SMALL, x + w - (i >= OPT_VERIFY ? 164 : 40), ry + (i >= OPT_VERIFY ? 24 : 50),
                       T(r == RECO_YES ? S_RECO : S_LESS_RECO), r == RECO_YES ? C_OK : C_WARN);
    }
    gfx_pop();
  }
  if (g_opt_sel < OPT_START) {   /* why, for the focused option */
    str_id why;
    opt_reco(g_opt_sel, &why);
    gfx_text(&FONT_SMALL, x + 40, y + 34 + OPT_COUNT * row + 24, T(why), C_DIM, w - 80);
  }
  /* right: what will be converted */
  const int dx = 1300, dw = 540;
  panel(dx, y + 20, dw, row * OPT_COUNT + 30);
  row_push(1);
  int n = marked_count();
  if (n > 1) {
    char line[100];
    snprintf(line, sizeof line, "%d %s", n, T(S_GAMES_SELECTED));
    gfx_text(&FONT_HEAD, dx + 32, y + 52, line, C_TEXT, dw - 64);
    int k = 0;
    for (int i = 0; i < g_ngames && k < 12; i++)
      if (g_games[i].marked) gfx_text(&FONT_BODY, dx + 32, y + 110 + 46 * k++, g_games[i].title, C_DIM, dw - 64);
  } else if (g_ngames) {
    const game_t *g = focus_game();
    draw_cover(g, dx + (dw - 320) / 2, y + 60, 320, 24, true);
    gfx_text(&FONT_HEAD, dx + 32, y + 410, g->title, C_TEXT, dw - 64);
    gfx_text(&FONT_BODY, dx + 32, y + 462, g->title_id, C_DIM, dw - 64);
  }
  gfx_pop();
}

static void draw_pair(void) {
  gfx_text(&FONT_TITLE, 80, 60, T(S_PAIR), C_TEXT, 0);
  panel(80, 170, 1760, 790);
  char url[160];
  snprintf(url, sizeof url, "http://%s:%d/?pin=%s", g_ip, ENGINE_PORT, g_pin);
  row_push(0);
  qr_draw(url, 140, 230, 560);
  gfx_pop();
  int x = 780;
  row_push(1);
  gfx_text_wrap(&FONT_BODY, x, 236, T(S_PAIR_HELP), C_DIM, 1000, 2);
  snprintf(url, sizeof url, "http://%s:%d", g_ip, ENGINE_PORT);
  gfx_text(&FONT_TITLE, x, 330, url, C_TEXT, 1020);
  gfx_text(&FONT_HEAD, x, 470, T(S_PAIR_CODE), C_DIM, 0);
  gfx_pop();
  /* the code, one big box per digit, popping in one after the other */
  for (int i = 0; i < 6 && g_pin[i]; i++) {
    int bx = x + i * 150;
    float e = ease_out_back(anim_t(g_tr_start + 160 + (uint64_t)i * 60, 380));
    gfx_push(0, (int)((1.0f - e) * 40.0f), (int)(255.0f * clampf(e, 0.0f, 1.0f)));
    gfx_round_rect(bx, 530, 130, 170, 24, C_PANEL_HI);
    char dch[2] = {g_pin[i], 0};
    gfx_text_center(&FONT_TITLE, bx + 65, 530 + 85 - FONT_TITLE.line / 2, dch, C_ACCENT);
    gfx_pop();
  }
  row_push(3);
  char dev[120];
  snprintf(dev, sizeof dev, "%d %s", g_pair_devices, T(S_DEVICES));
  gfx_text(&FONT_BODY, x, 760, dev, g_pair_devices ? C_OK : C_DIM, 0);
  gfx_text(&FONT_SMALL, x, 820, T(S_CONTINUES), C_DIM, 1000);
  gfx_pop();
}

/* start screen: the app icon in a breathing halo, a spinner while the engine gets ready */
static void draw_boot(void) {
  const int cx = SCREEN_W / 2, s = 220;
  float e = ease_out(anim_t(g_boot_started, 800)), p = anim_pulse(2000);
  int ly = 230 + (int)((1.0f - e) * 40.0f);
  gfx_push(0, 0, (int)(255.0f * e));
  int lh = g_logo_tile ? s * g_logo.h / g_logo.w : s, lr = g_logo_tile ? s * 48 / g_logo.w : 0, lt = ly + (s - lh) / 2;
  gfx_glow(cx - s / 2 + 24, ly + 24, s - 48, s - 48, 40, RGBA(70, 150, 255, (int)(46 + 56 * p)), 36);
  gfx_glow(cx - s / 2, lt + 10, s, lh, lr, RGBA(0, 0, 0, 120), 22);
  if (g_logo.px) gfx_image(&g_logo, cx - s / 2, lt, s, lh, lr);
  else gfx_round_rect(cx - s / 2, ly, s, s, 48, C_ACCENT);
  gfx_text_center(&FONT_TITLE, cx, ly + s + 44, "exFAT2AMPR", C_TEXT);
  gfx_text_center(&FONT_BODY, cx, ly + s + 112, T(S_APP_SUB), C_DIM);
  char ver[80], vline[120];
  snprintf(vline, sizeof vline, "%s %s", T(S_VERSION), ver_label(app_version(), ver, sizeof ver));
  gfx_text_center(&FONT_SMALL, cx, SCREEN_H - 118, T(S_CREDITS), C_DIM);
  gfx_text_center(&FONT_SMALL, cx, SCREEN_H - 80, vline, C_DIM);
  if (!g_boot_failed) {
    g_anim_busy = true;   /* the spinner turns until the engine answers */
    gfx_spinner(cx, ly + s + 228, 30, 5, (float)(g_now % 1100) / 1100.0f, C_ACCENT);
    gfx_text_center(&FONT_BODY, cx, ly + s + 286, T(S_STARTING), C_DIM);
  } else {
    float f = ease_out(anim_t(g_boot_failed_at, 320));
    gfx_push(0, (int)((1.0f - f) * 24.0f), (int)(255.0f * f));
    panel(360, ly + s + 180, 1200, 160);
    gfx_text_wrap(&FONT_BODY, 400, ly + s + 210, g_boot_msg, C_ERR, 1120, 3);
    int bx = 820;
    bx += gfx_button(ICON_CROSS, bx, ly + s + 372, 44) + 14;
    gfx_text(&FONT_HEAD, bx, ly + s + 376, T(S_RETRY), C_TEXT, 0);
    gfx_pop();
  }
  gfx_pop();
}

static void draw_confirm(void) {
  float e = ease_out(anim_t(g_confirm_at, 220));
  bool detail = g_confirm_detail[0] != 0;   /* a deletion shows what is erased; a retry shows why it failed */
  bool del = g_confirm == ACT_DELETE_SOURCE;
  int by = detail ? 330 : 380, bh = detail ? 420 : 300, ty = by + 50, hy = by + bh - 90;
  uint32_t edge = g_confirm == ACT_DELETE_SOURCE ? RGBA(255, 120, 120, 190) : RGBA(110, 180, 255, 170);
  gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(0, 0, 0, (int)(160.0f * e)));
  gfx_push(0, (int)((1.0f - e) * 28.0f), (int)(255.0f * e));
  gfx_glow(460, by, 1000, bh, 32, RGBA(0, 0, 0, 140), 26);
  gfx_round_rect(460, by, 1000, bh, 32, RGB(24, 30, 46));
  gfx_round_frame(460, by, 1000, bh, 32, 2, edge);
  if (detail && del) {
    char q[400];
    snprintf(q, sizeof q, T(S_DELETE_Q), g_confirm_text);
    gfx_text_wrap(&FONT_HEAD, 520, ty, q, C_TEXT, 880, 2);
    gfx_text_wrap(&FONT_BODY, 520, ty + 100, T(S_DELETE_NOTE), C_WARN, 880, 2);
    gfx_text_wrap(&FONT_SMALL, 520, ty + 186, g_confirm_detail, C_DIM, 880, 3);
  } else if (detail) {
    gfx_text_wrap(&FONT_HEAD, 520, ty, g_confirm_text, C_TEXT, 880, 2);
    gfx_text_wrap(&FONT_SMALL, 520, ty + 120, g_confirm_detail, C_WARN, 880, 4);
  } else gfx_text_wrap(&FONT_HEAD, 520, ty, g_confirm_text, C_TEXT, 880, 3);
  int x = 520;
  x += gfx_button(ICON_CROSS, x, hy, 44) + 12;
  x += gfx_text(&FONT_HEAD, x, hy + 4, T(S_YES), C_TEXT, 0) + 60;
  x += gfx_button(ICON_CIRCLE, x, hy, 44) + 12;
  gfx_text(&FONT_HEAD, x, hy + 4, T(S_NO), C_TEXT, 0);
  gfx_pop();
}

/* the game's plugins line of the menu: what the engine found in its fakelib (an image: ShadowMountPlus
 * applies the engine's emulator folder to it at launch) */
static str_id plugins_label(const game_t *g) {
  if (g->image) return S_PLUG_SMP;
  if (!strcmp(g->plugins, "update")) return S_PLUG_UPDATE;
  if (!strcmp(g->plugins, "missing")) return S_PLUG_MISSING;
  if (!strcmp(g->plugins, "unknown")) return S_PLUG_UNKNOWN;
  if (!g->plugins[0] || !strcmp(g->plugins, "none")) return S_PLUG_NONE;
  return S_PLUG_OK;
}

/* the menu OPTIONS opens on a game: rises from the bottom right corner */
static void draw_game_menu(void) {
  static const str_id items[MENU_COUNT] = {S_OPTIMIZE, S_PLUG_OK, S_CONVERT_ALL, S_DELETE_COPY, S_SETTINGS};
  static anim_val sel_v;
  float e = ease_out(anim_t(g_menu_at, 200));
  const int w = 560, row = 76, h = row * MENU_COUNT + 92, x = 1840 - w, y = 980 - h;
  gfx_push(0, (int)((1.0f - e) * 30.0f), (int)(255.0f * e));
  gfx_glow(x, y, w, h, 28, RGBA(0, 0, 0, 150), 24);
  gfx_round_rect(x, y, w, h, 28, RGBA(20, 26, 42, 245));
  gfx_round_frame(x, y, w, h, 28, 1, C_PANEL_EDGE);
  const game_t *g = &g_games[g_sel];
  gfx_text(&FONT_SMALL, x + 32, y + 24, g->title, C_DIM, w - 64);
  float sv = anim_track(&sel_v, (float)g_menu_sel, 22.0f, 0.002f);
  draw_focus(x + 14, y + 66 + (int)(sv * (float)row), w - 28, row - 10, 18);
  for (int i = 0; i < MENU_COUNT; i++) {
    str_id id = i == MENU_PLUGINS ? plugins_label(g) : items[i];
    uint32_t c = i == MENU_DELETE ? RGB(255, 170, 170) : C_TEXT;
    if (i == MENU_PLUGINS && (id == S_PLUG_SMP || id == S_PLUG_NONE || id == S_PLUG_OK)) c = C_DIM;   /* nothing to do */
    gfx_text(&FONT_HEAD, x + 40, y + 66 + i * row + (row - 10) / 2 - FONT_HEAD.line / 2, T(id), c, w - 80);
  }
  gfx_pop();
}

/* toast: rises in, fades out */
#define TOAST_OUT_MS 300
static void draw_toast(void) {
  if (!g_toast[0] || g_now >= g_toast_until) return;
  float in = ease_out(anim_t(g_toast_at, 260));
  float out = g_toast_until - g_now < TOAST_OUT_MS ? (float)(g_toast_until - g_now) / TOAST_OUT_MS : 1.0f;
  if (out < 1.0f) g_anim_busy = true;
  int w = gfx_text_width(&FONT_BODY, g_toast) + 104;
  if (w > 1100) w = 1100;
  int y = 894 + (int)((1.0f - in) * 44.0f);   /* above the button hints, over the panels' lower edge */
  uint32_t bg = g_toast_kind == TOAST_ERR ? RGBA(150, 42, 50, 238) : g_toast_kind == TOAST_OK ? RGBA(24, 118, 80, 238) : RGBA(30, 74, 172, 238);
  gfx_push(0, 0, (int)(255.0f * (in < out ? in : out)));
  gfx_glow(1840 - w, y, w, 64, 32, RGBA(0, 0, 0, 110), 18);
  gfx_round_rect(1840 - w, y, w, 64, 32, bg);
  gfx_round_frame(1840 - w, y, w, 64, 32, 1, RGBA(255, 255, 255, 40));
  gfx_circle(1840 - w + 34, y + 32, 9, RGBA(255, 255, 255, 230));
  gfx_text(&FONT_BODY, 1840 - w + 58, y + 32 - FONT_BODY.line / 2, g_toast, RGB(255, 255, 255), w - 90);
  gfx_pop();
}

static void draw_optimize(void);

static void draw(void) {
  /* a new screen or tab: its content slides in from the side we are going to */
  if (g_screen != g_prev_screen || (g_screen == SCR_MAIN && g_tab != g_prev_tab)) {
    g_tr_tab = g_screen == g_prev_screen;
    g_tr_dir = g_nav_dir;
    g_tr_start = g_now;
    g_prev_screen = g_screen;
    g_prev_tab = g_tab;
    g_nav_dir = 1;
  }
  assets_draw_wallpaper();
  float e = ease_out(anim_t(g_tr_start, 320));
  int dx = (int)((1.0f - e) * 70.0f * (float)g_tr_dir), op = (int)(255.0f * e);
  if (g_screen == SCR_BOOT) draw_boot();
  else if (g_screen == SCR_MAIN) {
    gfx_push(g_tr_tab ? 0 : dx, 0, g_tr_tab ? 255 : op);   /* header and tabs stay put when only the tab changes */
    draw_header();
    draw_tabs();
    gfx_push(g_tr_tab ? dx : 0, 0, g_tr_tab ? op : 255);
    if (g_tab == TAB_GAMES) {
      draw_games();
      int icons[] = {ICON_CROSS, ICON_SQUARE, ICON_TRIANGLE, ICON_OPTIONS};
      str_id labels[] = {S_CONVERT, S_MARK, S_REFRESH, S_MORE};
      draw_hints(icons, labels, 4);
      if (g_menu) draw_game_menu();
    } else if (g_tab == TAB_PROGRESS) {
      draw_progress();
      bool running = !strcmp(g_st.state, "running"), paused = !strcmp(g_st.state, "paused");
      bool error = !strcmp(g_st.state, "error"), cancelled = !strcmp(g_st.state, "cancelled"), done = !strcmp(g_st.state, "done");
      if (running || paused) {
        int icons[] = {ICON_TRIANGLE, ICON_SQUARE};
        str_id labels[] = {g_st.waiting ? S_CLOSE_APP : paused ? S_RESUME : S_PAUSE, S_CANCEL};
        draw_hints(icons, labels, 2);
      } else if (error || cancelled) {   /* never stuck on an error: start it again, or clear it */
        int icons[] = {ICON_CROSS, ICON_SQUARE};
        str_id labels[] = {g_st.resumable ? S_RESUME : error ? S_RETRY : S_RESTART, S_CLEAR};
        if (g_st.resumable || g_st.can_retry) draw_hints(icons, labels, 2);
        else draw_hints(icons + 1, labels + 1, 1);
      } else if (done) {
        int icons[] = {ICON_SQUARE};
        str_id labels[] = {S_CLEAR};
        draw_hints(icons, labels, 1);
      }
    } else if (g_tab == TAB_RESUME) {
      draw_resume();
      int icons[] = {ICON_CROSS, ICON_SQUARE, ICON_TRIANGLE};
      str_id labels[] = {S_RESUME, S_DISCARD, S_REFRESH};
      if (g_nres) draw_hints(icons, labels, 3);
    } else {
      draw_settings();
      int icons[] = {ICON_CROSS, ICON_DPAD};
      str_id labels[] = {S_SELECT, S_LANGUAGE};
      draw_hints(icons, labels, 2);
    }
    gfx_pop();
    gfx_pop();
  } else {
    gfx_push(dx, 0, op);
    if (g_screen == SCR_PAIR) {
      draw_pair();
      int icons[] = {ICON_SQUARE, ICON_CIRCLE};
      str_id labels[] = {S_PAIR_RESET, S_BACK};
      draw_hints(icons, labels, 2);
    } else if (g_screen == SCR_DEST) {
      draw_dest();
      int icons[] = {ICON_CROSS, ICON_CIRCLE, ICON_SQUARE, ICON_TRIANGLE, ICON_OPTIONS};
      str_id labels[] = {S_SELECT, S_BACK, S_SPEED_TEST, S_REFRESH, S_DEST_INFO};
      draw_hints(icons, labels, 5);
    } else if (g_screen == SCR_OPTIMIZE) {
      draw_optimize();
      int icons[] = {ICON_CROSS, ICON_CIRCLE, ICON_TRIANGLE};
      str_id labels[] = {S_SELECT, S_BACK, S_REFRESH};
      draw_hints(icons, labels, 3);
    } else if (g_screen == SCR_CHECK) {
      draw_checks();
      int icons[] = {ICON_CROSS, ICON_CIRCLE, ICON_DPAD};
      str_id labels[] = {S_START, S_BACK, S_LIST};
      /* without anything to convert, no "start" hint */
      if (checks_ok()) draw_hints(icons, labels, 3);
      else draw_hints(icons + 1, labels + 1, 2);
    } else {
      draw_options();
      int icons[] = {ICON_CROSS, ICON_CIRCLE};
      str_id labels[] = {S_SELECT, S_BACK};
      draw_hints(icons, labels, 2);
    }
    gfx_pop();
  }
  draw_toast();
  if (g_confirm) draw_confirm();
}

/* --------------------------------------------------------------- input -- */
static uint32_t g_prev, g_repeat_btn;
static uint64_t g_repeat_at;

static uint32_t read_pressed(void) {
  int lx, ly;
  uint32_t held = plat_buttons(&lx, &ly);
  if (ly < -80) held |= BTN_UP;
  if (ly > 80) held |= BTN_DOWN;
  if (lx < -80) held |= BTN_LEFT;
  if (lx > 80) held |= BTN_RIGHT;
  uint32_t pressed = held & ~g_prev;
  const uint32_t dirs = BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT;
  uint64_t now = plat_ms();
  if (pressed & dirs) { g_repeat_btn = pressed & dirs; g_repeat_at = now + 380; }
  else if ((held & g_repeat_btn) && now >= g_repeat_at) { pressed |= held & g_repeat_btn; g_repeat_at = now + 80; }
  else if (!(held & g_repeat_btn)) g_repeat_btn = 0;
  g_prev = held;
  return pressed;
}

static void enter_tab(int tab) {
  g_nav_dir = tab >= g_tab ? 1 : -1;
  g_tab = (tab + TAB_COUNT) % TAB_COUNT;
  if (g_tab == TAB_RESUME) load_resumable();
  if (g_tab == TAB_PROGRESS) poll_status();
  if (g_tab == TAB_SETTINGS) load_plugins_info();
  g_dirty = true;
}

static void *boot_thread(void *arg) {
  (void)arg;
  g_boot_result = engine_ensure(g_boot_want, g_engine_version, sizeof g_engine_version);
  __atomic_store_n(&g_boot_state, 2, __ATOMIC_RELEASE);
  return NULL;
}

static void boot(void) {
  g_boot_failed = false;
  g_screen = SCR_BOOT;
  g_boot_started = plat_ms();
  g_boot_want[0] = 0;
  char path[256];
  uint8_t *v;
  size_t len;
  snprintf(path, sizeof path, "%s/engine_version.txt", plat_asset_dir());
  if (plat_read_file(path, &v, &len) == 0) {
    size_t n = len < sizeof g_boot_want - 1 ? len : sizeof g_boot_want - 1;
    memcpy(g_boot_want, v, n);
    g_boot_want[n] = 0;
    g_boot_want[strcspn(g_boot_want, "\r\n ")] = 0;
    free(v);
  }
  __atomic_store_n(&g_boot_state, 1, __ATOMIC_RELEASE);
  pthread_t th;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&th, &at, boot_thread, NULL) != 0) boot_thread(NULL);
  pthread_attr_destroy(&at);
  g_dirty = true;
}

/* the engine answered (or not): leave the splash once it has played */
static void boot_finish(void) {
  if (plat_ms() - g_boot_started < SPLASH_MS) return;
  __atomic_store_n(&g_boot_state, 0, __ATOMIC_RELEASE);
  switch (g_boot_result) {
    case ENGINE_OK: break;
    case ENGINE_STARTED: toast_kind(T(S_ENGINE_STARTED), TOAST_OK); snd_cue_frame(SND_NOTIFY); break;
    case ENGINE_UPDATED: toast_kind(T(S_ENGINE_UPDATED), TOAST_OK); snd_cue_frame(SND_NOTIFY); break;
    case ENGINE_BUSY_OLD: toast(T(S_OLD_ENGINE)); break;
    case ENGINE_NO_LOADER: g_boot_failed = true; scopy(g_boot_msg, sizeof g_boot_msg, T(S_NO_LOADER)); break;
    case ENGINE_NO_ELF: g_boot_failed = true; scopy(g_boot_msg, sizeof g_boot_msg, T(S_NO_ELF)); break;
    case ENGINE_TIMEOUT: g_boot_failed = true; scopy(g_boot_msg, sizeof g_boot_msg, T(S_TIMEOUT)); break;
  }
  g_dirty = true;
  if (g_boot_failed) {
    g_boot_failed_at = plat_ms();
    snd_cue_frame(SND_ERROR);
    return;
  }
  g_screen = SCR_MAIN;
  poll_status();
  /* a conversion already running: show it first (the game list gives its cover) */
  bool busy = !strcmp(g_st.state, "running") || !strcmp(g_st.state, "paused");
  if (busy) load_games();
  enter_tab(busy ? TAB_PROGRESS : TAB_GAMES);
  load_dests(false);
  update_check(app_version());   /* a newer release? said once, in a toast */
}

static bool *opt_flag(int i) {
  switch (i) {
    case OPT_VERIFY: return &g_opt.verify;
    case OPT_DEDUP: return &g_opt.dedup;
    case OPT_CLEAN: return &g_opt.clean;
    case OPT_OVERWRITE: return &g_opt.overwrite;
    case OPT_REMOVE: return &g_opt.remove;
    default: return NULL;
  }
}

/* moves a list selection; a tick when it really moved */
static void move_sel(int *sel, int n, int delta) {
  if (n <= 0) return;
  int old = *sel;
  *sel = ((*sel + delta) % n + n) % n;
  if (*sel != old) snd_cue_frame(SND_MOVE);
}

static void toggle_sound(bool on) { snd_cue_frame(on ? SND_TOGGLE_ON : SND_TOGGLE_OFF); }

#include "optimize_ui.inc"

static void handle_input(uint32_t p) {
  if (g_confirm) {
    if (p & BTN_CROSS) { int a = g_confirm; g_confirm = 0; snd_cue_frame(SND_SELECT); run_confirmed(a); }
    else if (p & BTN_CIRCLE) { g_confirm = 0; snd_cue_frame(SND_BACK); }
    return;
  }
  if (g_screen == SCR_BOOT) {
    if (g_boot_failed && (p & BTN_CROSS)) { snd_cue_frame(SND_SELECT); boot(); }
    return;
  }
  if (g_screen == SCR_OPTIMIZE) { optimize_input(p); return; }
  if (g_screen == SCR_PAIR) {
    if (p & BTN_CIRCLE) { g_screen = SCR_MAIN; snd_cue_frame(SND_BACK); }
    if (p & BTN_SQUARE) { confirm(ACT_PAIR_RESET, T(S_PAIR_RESET_CONFIRM)); snd_cue_frame(SND_SELECT); }
    return;
  }
  if (g_screen == SCR_DEST) {
    if (p & BTN_CIRCLE) { g_screen = SCR_OPTIONS; snd_cue_frame(SND_BACK); return; }
    if (p & BTN_TRIANGLE) { load_dests(true); toast(T(S_REFRESHED)); snd_cue_frame(SND_SELECT); }
    if (p & BTN_OPTIONS) { g_dest_info = !g_dest_info; toggle_sound(g_dest_info); }
    if ((p & BTN_SQUARE) && g_ndests && g_dest_sel < g_ndests) speed_start(&g_dests[g_dest_sel]);
    if (g_ndests) {
      if (p & BTN_UP) move_sel(&g_dest_sel, g_ndests, -1);
      if (p & BTN_DOWN) move_sel(&g_dest_sel, g_ndests, 1);
      if (g_dest_sel >= g_ndests) g_dest_sel = g_ndests - 1;
      if (p & BTN_CROSS) {
        if (!g_dests[g_dest_sel].writable) toast_err(T(S_DEST_CANT));
        else {
          g_opt.dest = g_dest_sel;
          scopy(g_dest_path, sizeof g_dest_path, g_dests[g_dest_sel].path);
          settings_save();
          g_screen = SCR_OPTIONS;
          snd_cue_frame(SND_SELECT);
          if (speed_class(&g_dests[g_dest_sel]) == SPEED_SLOW) toast_err(T(S_SPEED_SLOW_WARN));
        }
      }
    }
    return;
  }
  if (g_screen == SCR_CHECK) {
    if (p & BTN_CIRCLE) { g_screen = SCR_OPTIONS; snd_cue_frame(SND_BACK); return; }
    if ((p & BTN_UP) && g_check_scroll > 0) { g_check_scroll--; snd_cue_frame(SND_MOVE); }
    if ((p & BTN_DOWN) && g_check_scroll + 4 < g_nchecks) { g_check_scroll++; snd_cue_frame(SND_MOVE); }
    if (p & BTN_CROSS) {
      if (checks_ok()) start_checked();
      else toast_err(T(S_NOTHING_TO_START));
    }
    return;
  }
  if (g_screen == SCR_OPTIONS) {
    if (p & BTN_CIRCLE) { g_screen = SCR_MAIN; snd_cue_frame(SND_BACK); return; }
    if (p & BTN_UP) move_sel(&g_opt_sel, OPT_COUNT, -1);
    if (p & BTN_DOWN) move_sel(&g_opt_sel, OPT_COUNT, 1);
    int dir = (p & BTN_RIGHT) ? 1 : (p & BTN_LEFT) ? -1 : 0;
    bool *f = opt_flag(g_opt_sel);
    if (dir) {
      if (g_opt_sel == OPT_DEST && g_ndests) {
        /* shortcut: cycle through the writable storages */
        for (int k = 0; k < g_ndests; k++) {
          g_opt.dest = (g_opt.dest + dir + g_ndests) % g_ndests;
          if (g_dests[g_opt.dest].writable) break;
        }
        scopy(g_dest_path, sizeof g_dest_path, g_dests[g_opt.dest].path);
        snd_cue_frame(SND_MOVE);
      } else if (g_opt_sel == OPT_LEVEL) {
        g_opt.level = g_opt.level + dir < 1 ? 12 : g_opt.level + dir > 12 ? 1 : g_opt.level + dir;
        snd_cue_frame(SND_MOVE);
      } else if (g_opt_sel == OPT_CACHE) { g_opt.cache ^= 1; toggle_sound(g_opt.cache); }
      else if (f && *f != (dir > 0)) { *f = dir > 0; toggle_sound(*f); }
    }
    if (p & BTN_CROSS) {
      if (f) { *f = !*f; toggle_sound(*f); }
      else if (g_opt_sel == OPT_CACHE) { g_opt.cache ^= 1; toggle_sound(g_opt.cache); }
      else if (g_opt_sel == OPT_DEST) { load_dests(true); g_dest_sel = g_opt.dest; g_screen = SCR_DEST; snd_cue_frame(SND_SELECT); }
      else if (g_opt_sel == OPT_START) {
        if (!g_ndests || !g_dests[g_opt.dest].writable) toast_err(T(S_E_DEST_MISSING));
        else {
          /* check everything before anything is written: source, compatibility, space, destination */
          snd_cue_frame(SND_SELECT);
          snd_flush();
          g_nchecks = 0;
          g_screen = SCR_CHECK;
          assets_draw_wallpaper();
          gfx_text_center(&FONT_HEAD, SCREEN_W / 2, SCREEN_H / 2 - 20, T(S_CHECKING), C_ACCENT);
          gfx_present();
          load_dests(true);
          run_checks();
        }
      }
    }
    return;
  }
  if (g_menu) {   /* the game menu has the controller until it closes */
    if (p & (BTN_CIRCLE | BTN_OPTIONS)) { g_menu = false; snd_cue_frame(SND_BACK); return; }
    if (p & BTN_UP) move_sel(&g_menu_sel, MENU_COUNT, -1);
    if (p & BTN_DOWN) move_sel(&g_menu_sel, MENU_COUNT, 1);
    if ((p & BTN_CROSS) && g_ngames) {
      g_menu = false;
      if (g_menu_sel == MENU_SETTINGS) { enter_tab(TAB_SETTINGS); snd_cue_frame(SND_TAB); return; }
      if (g_menu_sel == MENU_CONVERT_ALL) { convert_all(); return; }
      if (g_menu_sel == MENU_OPTIMIZE) { open_optimize(&g_games[g_sel]); return; }
      if (g_menu_sel == MENU_PLUGINS) { plugins_refresh_game(&g_games[g_sel]); return; }
      const game_t *g = &g_games[g_sel];
      scopy(g_delete_path, sizeof g_delete_path, g->path);
      char size[32] = "";
      if (g->size > 0) fmt_bytes(size, sizeof size, (double)g->size);
      snprintf(g_confirm_detail, sizeof g_confirm_detail, "%s%s%s", g->path, size[0] ? "  ·  " : "", size);
      confirm(ACT_DELETE_SOURCE, g->title[0] ? g->title : g->path);
      snd_cue_frame(SND_SELECT);
    }
    return;
  }
  if (p & BTN_L1) { enter_tab(g_tab - 1); snd_cue_frame(SND_TAB); return; }
  if (p & BTN_R1) { enter_tab(g_tab + 1); snd_cue_frame(SND_TAB); return; }
  if ((p & BTN_OPTIONS) && g_tab == TAB_GAMES && g_ngames) {
    g_menu = true;
    g_menu_sel = 0;
    g_menu_at = plat_ms();
    snd_cue_frame(SND_SELECT);
    return;
  }
  if (p & BTN_OPTIONS) { if (g_tab != TAB_SETTINGS) snd_cue_frame(SND_TAB); enter_tab(TAB_SETTINGS); return; }
  switch (g_tab) {
    case TAB_GAMES: {
      if (p & BTN_TRIANGLE) { snd_cue_frame(SND_SELECT); g_games_loaded = false; draw(); gfx_present(); load_games(); }
      if (!g_ngames) break;
      if (p & BTN_UP) move_sel(&g_sel, g_ngames, -1);
      if (p & BTN_DOWN) move_sel(&g_sel, g_ngames, 1);
      if (p & (BTN_LEFT | BTN_RIGHT)) {   /* a page at a time */
        int old = g_sel;
        if (p & BTN_LEFT) g_sel = g_sel >= 8 ? g_sel - 8 : 0;
        if (p & BTN_RIGHT) g_sel = g_sel + 8 < g_ngames ? g_sel + 8 : g_ngames - 1;
        if (g_sel != old) snd_cue_frame(SND_MOVE);
      }
      bool playing = g_games[g_sel].title_id[0] && !strcmp(g_games[g_sel].title_id, g_st.game_running);
      if ((p & (BTN_SQUARE | BTN_CROSS)) && playing && !marked_count()) { toast_err(T(S_E_GAME_RUNNING)); break; }
      if (p & BTN_SQUARE) {
        if (g_games[g_sel].selectable && !playing) {
          g_games[g_sel].marked = !g_games[g_sel].marked;
          toggle_sound(g_games[g_sel].marked);
        } else snd_cue_frame(SND_ERROR);
      }
      if (p & BTN_CROSS) {
        if (!marked_count() && !g_games[g_sel].selectable) { toast_err(T(S_NOT_MANAGED)); break; }
        load_dests(false);
        g_opt.remove = false;   /* never carried over from a previous conversion */
        g_opt_sel = OPT_START;
        g_screen = SCR_OPTIONS;
        snd_cue_frame(SND_SELECT);
      }
      break;
    }
    case TAB_PROGRESS: {
      bool running = !strcmp(g_st.state, "running"), paused = !strcmp(g_st.state, "paused");
      bool failed = !strcmp(g_st.state, "error") || !strcmp(g_st.state, "cancelled"), done = !strcmp(g_st.state, "done");
      jdoc d;
      if (failed && (p & BTN_CROSS)) {
        int st = -1;
        if (g_st.resumable) {
          char form[1300] = "";
          form_add(form, sizeof form, "dest", g_st.dest);
          st = api_call("/api/resume", form, &d);
          json_free(&d);
        } else if (g_st.can_retry) {
          st = api_call("/api/retry", "", &d);
          json_free(&d);
        }
        if (st == 200) { g_app_started_at = plat_ms(); snd_cue_frame(SND_START); }
        else snd_cue_frame(SND_ERROR);
        poll_status();
        break;
      }
      if ((failed || done || paused) && (p & BTN_SQUARE) && !running) {
        if (paused) { confirm(ACT_CANCEL_JOB, T(S_CANCEL_CONFIRM)); snd_cue_frame(SND_SELECT); break; }
        api_call("/api/dismiss", "", &d);   /* the result goes away: back to "nothing running" */
        json_free(&d);
        g_done_at = g_err_at = 0;
        snd_cue_frame(SND_BACK);
        poll_status();
        break;
      }
      if ((p & BTN_TRIANGLE) && running && g_st.waiting) {   /* ShadowMountPlus mounts once no title runs: this app included */
        settings_save();
        plat_exit();
      }
      if ((p & BTN_TRIANGLE) && running) { api_call("/api/pause", "", &d); json_free(&d); poll_status(); snd_cue_frame(SND_TOGGLE_OFF); }
      else if ((p & BTN_TRIANGLE) && paused) {
        char form[1300] = "";
        form_add(form, sizeof form, "dest", g_st.dest);
        api_call("/api/resume", form, &d);
        json_free(&d);
        poll_status();
        snd_cue_frame(SND_TOGGLE_ON);
      }
      if ((p & BTN_SQUARE) && (running || paused)) { confirm(ACT_CANCEL_JOB, T(S_CANCEL_CONFIRM)); snd_cue_frame(SND_SELECT); }
      break;
    }
    case TAB_RESUME:
      if (p & BTN_TRIANGLE) { load_resumable(); snd_cue_frame(SND_SELECT); }
      if (!g_nres) break;
      if (p & BTN_UP) move_sel(&g_res_sel, g_nres, -1);
      if (p & BTN_DOWN) move_sel(&g_res_sel, g_nres, 1);
      if ((p & BTN_CROSS) && g_res[g_res_sel].available && !g_res[g_res_sel].active) {
        char form[1300] = "";
        jdoc d;
        form_add(form, sizeof form, "dest", g_res[g_res_sel].dest);
        int st = api_call("/api/resume", form, &d);
        json_free(&d);
        if (st == 200) { enter_tab(TAB_PROGRESS); snd_cue_frame(SND_START); }
        else toast_err(T(S_ERROR));
      } else if (p & BTN_CROSS) snd_cue_frame(SND_ERROR);
      if ((p & BTN_SQUARE) && !g_res[g_res_sel].active) { confirm(ACT_DISCARD, T(S_DISCARD_CONFIRM)); snd_cue_frame(SND_SELECT); }
      break;
    case TAB_SETTINGS:
      if (p & BTN_UP) move_sel(&g_set_sel, SET_COUNT, -1);
      if (p & BTN_DOWN) move_sel(&g_set_sel, SET_COUNT, 1);
      if (g_set_sel == SET_LANG && (p & (BTN_LEFT | BTN_RIGHT | BTN_CROSS))) {
        int dir = (p & BTN_LEFT) ? -1 : 1;
        i18n_set((i18n_get() + dir + LANG_COUNT) % LANG_COUNT);
        g_lang_at = plat_ms();
        g_lang_dir = dir;
        settings_save();
        snd_cue_frame(SND_MOVE);
      }
      if (g_set_sel == SET_GAME_MODE && (p & (BTN_LEFT | BTN_RIGHT | BTN_CROSS))) {
        jdoc d;
        api_call("/api/settings", g_st.game_mode_enabled ? "game_mode=0" : "game_mode=1", &d);
        json_free(&d);
        toggle_sound(!g_st.game_mode_enabled);
        poll_status();
      }
      if (g_set_sel == SET_SOUNDS && (p & (BTN_LEFT | BTN_RIGHT | BTN_CROSS))) {
        bool on = (p & BTN_LEFT) ? false : (p & BTN_RIGHT) ? true : !snd_enabled();
        snd_set_enabled(on);
        toggle_sound(on);   /* heard only when switching them on */
        settings_save();
      }
      if (g_set_sel == SET_ANIMS && (p & (BTN_LEFT | BTN_RIGHT | BTN_CROSS))) {
        g_anim_enabled = (p & BTN_LEFT) ? false : (p & BTN_RIGHT) ? true : !g_anim_enabled;
        toggle_sound(g_anim_enabled);
        settings_save();
      }
      if (g_set_sel == SET_PAIR && (p & BTN_CROSS)) { load_pair(); g_screen = SCR_PAIR; snd_cue_frame(SND_SELECT); }
      if (g_set_sel == SET_PLUGINS && (p & BTN_CROSS)) {   /* every game and ShadowMountPlus' emulator folder, in the background */
        jdoc d;
        if (api_call("/api/plugins/refresh", "all=1", &d) == 200) { toast(T(S_PLUG_STARTED)); g_plug.running = true; g_plug.ok = true; }
        else { char m[200]; snprintf(m, sizeof m, T(S_PLUG_FAILED), T(S_ENGINE)); toast_err(m); }
        json_free(&d);
        snd_cue_frame(SND_SELECT);
      }
      if (g_set_sel == SET_UPDATE && (p & BTN_CROSS)) {
        char ver[32], sz[32], q[300];
        uint64_t size = 0;
        upd_state us = update_state(NULL, ver, sizeof ver, &size, NULL, 0);
        if (us == UPD_AVAILABLE || us == UPD_FAILED) {
          fmt_bytes(sz, sizeof sz, (double)size);
          snprintf(q, sizeof q, T(S_UPD_INSTALL_Q), ver, sz);
          confirm(ACT_UPDATE, q);
          if (us == UPD_FAILED) update_state(NULL, NULL, 0, NULL, g_confirm_detail, sizeof g_confirm_detail);   /* why it failed */
        } else if (us != UPD_DOWNLOADING && us != UPD_INSTALLED) update_check(app_version());
        snd_cue_frame(SND_SELECT);
      }
      if (g_set_sel == SET_QUIT && (p & BTN_CROSS)) { settings_save(); plat_exit(); }
      break;
    default: break;
  }
}

static int screen_depth(int s) { return s == SCR_BOOT ? 0 : s == SCR_MAIN ? 1 : s == SCR_OPTIONS || s == SCR_PAIR || s == SCR_OPTIMIZE ? 2 : 3; }

static void handle(uint32_t p) {
  if (!p) return;
  g_dirty = true;
  int old_screen = g_screen;
  handle_input(p);
  if (g_screen != old_screen) g_nav_dir = screen_depth(g_screen) >= screen_depth(old_screen) ? 1 : -1;
}

static void load_logo(void) {
  uint8_t *png;
  size_t len;
  if (plat_read_file(plat_icon_path(), &png, &len) != 0) return;
  image_t full;
  if (img_decode_png(png, len, 512, &full) == 0) {
    /* icon0.png (512 x 512): a dark tile with 48 px corners on white; the start screen shows the
     * tile alone and rounds its corners itself */
    g_logo_tile = full.w == 512 && full.h == 512 && img_crop(&full, 54, 60, 404, 398, &g_logo) == 0;
    if (g_logo_tile) img_free(&full);
    else g_logo = full;
  }
  free(png);
}

/* ---------------------------------------------------------------- main -- */
#ifndef E2A_APP_NO_MAIN
/* the CJK characters of game titles: a few MB to inflate, off the main thread */
static void *fonts_thread(void *arg) {
  (void)arg;
  char path[256];
  snprintf(path, sizeof path, "%s/glyphs_cjk.bin", plat_asset_dir());
  gfx_load_extra(path);
  return NULL;
}

int main(void) {
  if (plat_init() != 0 || gfx_init() != 0) {
    plat_notify("exFAT2AMPR: display init failed");
    for (;;) plat_sleep_ms(1000);
  }
  settings_load();
  pthread_t fth;
  pthread_attr_t fat;
  pthread_attr_init(&fat);
  pthread_attr_setdetachstate(&fat, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&fth, &fat, fonts_thread, NULL) != 0) fonts_thread(NULL);
  pthread_attr_destroy(&fat);
  assets_init();
  snd_init();
  load_logo();
  anim_input(plat_ms());
  boot();
  uint64_t next_poll = 0, last_draw = 0;
  bool busy = true, ambient = false;
  for (;;) {
    uint64_t start = plat_ms();
    uint32_t pressed = read_pressed();
    if (pressed) anim_input(start);
    handle(pressed);
    if (__atomic_load_n(&g_boot_state, __ATOMIC_ACQUIRE) == 2) boot_finish();
    uint64_t now = plat_ms();
    if (g_screen != SCR_BOOT && now >= next_poll) {
      poll_status();
      if (g_screen == SCR_PAIR || g_tab == TAB_SETTINGS || !g_pin[0]) load_pair();
      if (g_tab == TAB_SETTINGS && g_plug.running) load_plugins_info();   /* until the pass over the games ends */
      next_poll = now + (!strcmp(g_st.state, "running") ? 1000 : 2500);
    }
    if (g_screen == SCR_MAIN && g_tab == TAB_GAMES && !g_games_loaded) { anim_frame(now); draw(); gfx_present(); load_games(); }
    if (run_checks_later) { run_checks_later = false; run_checks(); }
    if (g_toast[0] && now >= g_toast_until) { g_toast[0] = 0; g_dirty = true; }
    else if (g_toast[0] && now + TOAST_OUT_MS >= g_toast_until) g_dirty = true;
    choose_wallpaper(now);
    if (assets_update(now)) g_dirty = true;
    if (gfx_extra_arrived()) g_dirty = true;
    if (speed_poll()) g_dirty = true;
    if (update_changed()) {   /* the update's news, once each */
      static upd_state told = UPD_IDLE;
      char ver[32], err[200], msg[300];
      upd_state us = update_state(NULL, ver, sizeof ver, NULL, err, sizeof err);
      if (us != told) {
        if (us == UPD_AVAILABLE) { snprintf(msg, sizeof msg, T(S_UPD_TOAST), ver); toast_kind(msg, TOAST_OK); snd_cue_frame(SND_NOTIFY); }
        else if (us == UPD_INSTALLED) { toast_kind(T(S_UPD_RESTART), TOAST_OK); snd_cue_frame(SND_DONE); }
        else if (us == UPD_FAILED) { snprintf(msg, sizeof msg, T(S_UPD_FAILED), err); toast_err(msg); snd_cue_frame(SND_ERROR); }
        told = us;
      }
      g_dirty = true;
    }
    snd_flush();
    if (g_dirty || busy || (ambient && now >= last_draw + 33)) {
      g_dirty = false;
      anim_frame(now);
      draw();
      gfx_present();
      busy = g_anim_busy;
      ambient = g_anim_ambient;
      last_draw = now;
      uint64_t spent = plat_ms() - start;
      if (spent < 16) plat_sleep_ms((unsigned)(16 - spent));   /* ~60 frames per second at most (the console also waits for the vblank) */
    } else plat_sleep_ms(10);
  }
}
#endif
