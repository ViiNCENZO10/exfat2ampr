/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * Conversion jobs: queue, scan -> profile -> index -> pack -> loose copy -> verify,
 * pause / resume through checkpoints, storage detection, ShadowMountPlus integration. */
#include "job.h"
#include "ampr_pack.h"
#include "profile.h"
#include "scan.h"
#include "smp.h"
#include "verify.h"
#include "storage.h"
#include "activity.h"
#include "plugins.h"
#include "covers.h"
#include "estimate.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#if defined(__linux__) || defined(__CYGWIN__)
#include <sys/statfs.h>
#else
#include <sys/param.h>
#include <sys/mount.h>
#endif
#include <stddef.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* libSceAmpr.sprx embedded at build time (xxd -i), see Makefile */
extern unsigned char libSceAmpr_sprx[];
extern unsigned int libSceAmpr_sprx_len;

static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_op_mx = PTHREAD_MUTEX_INITIALIZER;
static char g_op_path[1100];

static char *resolve_future(const char *path) {
  char *r = realpath(path, NULL);
  if (r) return r;
  char prefix[1100], suffix[1100] = "";
  if (snprintf(prefix, sizeof prefix, "%s", path) >= (int)sizeof prefix) return NULL;
  for (;;) {
    char *slash = strrchr(prefix, '/');
    char tail[1100];
    if (snprintf(tail, sizeof tail, "/%s%s", slash ? slash + 1 : prefix, suffix) >= (int)sizeof tail) return NULL;
    memcpy(suffix, tail, strlen(tail) + 1);
    if (!slash) strcpy(prefix, ".");
    else if (slash == prefix) slash[1] = 0;
    else *slash = 0;
    r = realpath(prefix, NULL);
    if (r) {
      size_t n = strlen(r) + strlen(suffix) + 1;
      char *out = malloc(n);
      if (out) snprintf(out, n, "%s%s", r, suffix);
      free(r); return out;
    }
    if (!slash || !strcmp(prefix, "/")) return NULL;
  }
}

static bool paths_overlap(const char *a, const char *b) {
  if (!a || !*a || !b || !*b) return false;
  char *ar = resolve_future(a), *br = resolve_future(b);
  if (ar) a = ar;
  if (br) b = br;
  size_t an = strlen(a), bn = strlen(b);
  while (an > 1 && a[an - 1] == '/') an--;
  while (bn > 1 && b[bn - 1] == '/') bn--;
  size_t n = an < bn ? an : bn;
  bool same = !strncasecmp(a, b, n) && (an == bn || (an > bn ? a[n] : b[n]) == '/');
  free(ar); free(br);
  return same;
}

static bool active_path_busy(const char *path);

static job_status g_status;
static job_request g_queue[E2A_QUEUE_MAX];
static int g_qlen;
static atomic_int g_cancel, g_pause;
/* source and destination of the running job, watched for drive removal */
static char g_live_dest[600], g_live_src[600];
static pthread_t g_sched;
static char g_runtime_source[32] = "embedded", g_runtime_sha[65], g_runtime_path[320];
static bool g_runtime_ok;
/* the last conversion started, so a failed or cancelled one can be started again */
static job_request g_last_req;
static bool g_have_last;

/* ------------------------------------------------------------- progress -- */
static double g_rate_t0, g_rate_b0, g_last_report;
#define RATE_WIN 40   /* samples of the speed window: 40 x 0.5 s = 20 s */
static double g_win_t[RATE_WIN], g_win_b[RATE_WIN], g_skipped;
static int g_win_head, g_win_n;

/* bytes counted done without being read or written (a volume already checked while packing, a loose file
 * already copied before a pause): they move the progress, never the speed */
static void on_skip(void *ctx, uint64_t bytes) {
  (void)ctx;
  pthread_mutex_lock(&g_mx);
  g_skipped += (double)bytes;
  pthread_mutex_unlock(&g_mx);
}

static void set_phase(const char *phase, int pct, const char *msg) {
  pthread_mutex_lock(&g_mx);
  snprintf(g_status.phase, sizeof g_status.phase, "%s", phase);
  g_status.percent = pct;
  if (msg) snprintf(g_status.message, sizeof g_status.message, "%s", msg);
  pthread_mutex_unlock(&g_mx);
}

static void begin_phase(const char *phase, const char *msg, uint64_t base) {
  pthread_mutex_lock(&g_mx);
  g_status.phase_base = base;
  g_status.bytes_done = base;
  int pct = g_status.bytes_total ? (int)(base * 100 / g_status.bytes_total) : 0;
  pthread_mutex_unlock(&g_mx);
  set_phase(phase, pct, msg);
  g_rate_t0 = e2a_now(); g_rate_b0 = (double)base; g_last_report = 0;
  pthread_mutex_lock(&g_mx);   /* each phase has its own pace: the window starts afresh */
  g_win_n = 0; g_win_head = 0; g_skipped = 0;
  g_status.speed_bps = 0;
  pthread_mutex_unlock(&g_mx);
}

static void on_progress(void *ctx, uint64_t phase_done, const char *msg) {
  double t = e2a_now();
  pthread_mutex_lock(&g_mx);
  g_status.bytes_done = g_status.phase_base + phase_done;
  if (g_status.bytes_done > g_status.bytes_total) g_status.bytes_done = g_status.bytes_total;
  /* the speed of the last 20 seconds of real work: a true sliding window (one sample per half second), so
   * neither a fast start (cached reads, small files) nor bytes counted without being read (skipped, see
   * on_skip) weigh on it for minutes afterwards */
  if (t - g_win_t[(g_win_head + RATE_WIN - 1) % RATE_WIN] >= 0.5 || g_win_n == 0) {
    g_win_t[g_win_head] = t;
    g_win_b[g_win_head] = (double)g_status.bytes_done - g_skipped;
    g_win_head = (g_win_head + 1) % RATE_WIN;
    if (g_win_n < RATE_WIN) g_win_n++;
  }
  if (g_win_n >= 4) {
    int newest = (g_win_head + RATE_WIN - 1) % RATE_WIN, oldest = (g_win_head + RATE_WIN - g_win_n) % RATE_WIN;
    double dt = g_win_t[newest] - g_win_t[oldest];
    if (dt >= 2.0) {
      double sp = (g_win_b[newest] - g_win_b[oldest]) / dt;
      g_status.speed_bps = sp > 0 ? sp : 0;
    }
  }
  /* time left phase by phase: packing and copying at the speed measured now, the verification at its own
   * speed (it only reads and decodes: several times faster), learned on the previous run */
  const char *ph = (const char *)ctx;
  bool verifying = ph && strcmp(ph, "verify") == 0;
  uint64_t left = g_status.bytes_total - g_status.bytes_done;
  if (g_status.speed_bps > 1.0) {
    double eta;
    if (verifying || !g_status.verify_bytes) eta = (double)left / g_status.speed_bps;
    else {
      uint64_t vb = g_status.verify_bytes < left ? g_status.verify_bytes : left;
      double vr = est_verify_rate();
      if (vr <= 0) vr = g_status.speed_bps * 4.0;
      eta = (double)(left - vb) / g_status.speed_bps + (double)vb / vr;
    }
    g_status.eta_seconds = (int64_t)(eta + 0.5);
  } else g_status.eta_seconds = -1;
  /* the converted size, refined with what the packed part really takes */
  e2a_pack_rates pr;
  e2a_pack_rates_get(&pr);
  if (pr.in_bytes >= (256ull << 20) && pr.out_bytes) {
    double r = (double)pr.out_bytes / (double)pr.in_bytes;
    uint64_t done_in = pr.in_bytes + pr.autoloose_bytes;
    uint64_t rest = g_status.pack_total > done_in ? g_status.pack_total - done_in : 0;
    g_status.estimated_bytes = pr.out_bytes + (uint64_t)((double)rest * r) + g_status.loose_total + pr.autoloose_bytes + (2u << 20);
  }
  g_status.percent = g_status.bytes_total ? (int)(g_status.bytes_done * 100 / g_status.bytes_total) : 0;
  snprintf(g_status.phase, sizeof g_status.phase, "%s", (const char *)ctx);
  if (t - g_last_report > 0.5) { g_last_report = t; snprintf(g_status.message, sizeof g_status.message, "%s", msg); }
  pthread_mutex_unlock(&g_mx);
}

static int fs_space(const char *path, uint64_t *total, uint64_t *avail) {
  struct statfs sf;
  if (statfs(path, &sf) != 0) return -1;
  int64_t av = (int64_t)sf.f_bavail;
  if (av < 0) av = 0;
  *total = (uint64_t)sf.f_blocks * (uint64_t)sf.f_bsize;
  *avail = (uint64_t)av * (uint64_t)sf.f_bsize;
  return 0;
}

/* total size of every regular file under root (packs included) */
static uint64_t dir_bytes(const char *root) {
  uint64_t total = 0;
  DIR *d = opendir(root);
  if (!d) return 0;
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0 || strcmp(de->d_name, ".e2a-state") == 0) continue;
    char child[1100];
    snprintf(child, sizeof child, "%s/%s", root, de->d_name);
    struct stat st;
    if (stat(child, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) total += dir_bytes(child);
    else if (S_ISREG(st.st_mode)) total += (uint64_t)st.st_size;
  }
  closedir(d);
  return total;
}

/* --------------------------------------------------------------- runtime -- */
static bool looks_like_prx(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return false;
  uint8_t m[4] = {0};
  size_t got = 0;
  e2a_read_full(fd, m, 4, &got);
  close(fd);
  /* PS5 SELF/PRX magic */
  return got == 4 && m[0] == 0x4F && m[1] == 0x15 && m[2] == 0x3D && m[3] == 0x1D;
}

static void detect_runtime(void) {
  char hex[65];
  if (e2a_is_file(E2A_SPRX_SMP) && looks_like_prx(E2A_SPRX_SMP) && e2a_sha256_file(E2A_SPRX_SMP, hex) == 0) {
    snprintf(g_runtime_source, sizeof g_runtime_source, "shadowmount");
    snprintf(g_runtime_path, sizeof g_runtime_path, "%s", E2A_SPRX_SMP);
    snprintf(g_runtime_sha, sizeof g_runtime_sha, "%s", hex);
    g_runtime_ok = true;
  } else {
    e2a_sha256 c;
    uint8_t d[32];
    e2a_sha256_init(&c);
    e2a_sha256_update(&c, libSceAmpr_sprx, libSceAmpr_sprx_len);
    e2a_sha256_final(&c, d);
    for (int i = 0; i < 32; i++) snprintf(g_runtime_sha + i * 2, 3, "%02x", d[i]);
    snprintf(g_runtime_source, sizeof g_runtime_source, "embedded");
    snprintf(g_runtime_path, sizeof g_runtime_path, "libSceAmpr.sprx %s (embedded)", E2A_AMPR_RUNTIME_VERSION);
    g_runtime_ok = libSceAmpr_sprx_len > 0 && libSceAmpr_sprx[0] == 0x4F;
  }
  e2a_log("AMPR runtime: %s sha256=%s%s", g_runtime_path, g_runtime_sha, g_runtime_ok ? "" : " (WARNING: not a PRX?)");
}

int job_runtime_json(char *json, size_t cap) {
  char p[400];
  e2a_json_escape(g_runtime_path, p, sizeof p);
  snprintf(json, cap, "{\"source\":\"%s\",\"path\":\"%s\",\"sha256\":\"%s\",\"embedded_version\":\"%s\",\"format\":\"%s\",\"ok\":%s}",
           g_runtime_source, p, g_runtime_sha, E2A_AMPR_RUNTIME_VERSION, E2A_AMPR_FORMAT, g_runtime_ok ? "true" : "false");
  return 0;
}

/* Prefer the emulator ShadowMountPlus keeps up to date, else the embedded copy. */
static int install_runtime(const char *out_dir) {
  char dir[700], dst[800];
  snprintf(dir, sizeof dir, "%s/fakelib", out_dir);
  if (e2a_mkdir_p(dir) != 0) return -1;
  snprintf(dst, sizeof dst, "%s/libSceAmpr.sprx", dir);
  char existing[65];
  if (e2a_is_file(dst) && e2a_sha256_file(dst, existing) == 0 && !strcmp(existing, g_runtime_sha)) return 0;
  if (strcmp(g_runtime_source, "shadowmount") == 0 && e2a_copy_file(E2A_SPRX_SMP, dst, time(NULL)) == 0) {
    e2a_log("fakelib: copied %s", E2A_SPRX_SMP);
    return 0;
  }
  if (e2a_write_file_atomic(dst, libSceAmpr_sprx, libSceAmpr_sprx_len) != 0) return -1;
  e2a_log("fakelib: embedded libSceAmpr.sprx %s (%u bytes)", E2A_AMPR_RUNTIME_VERSION, libSceAmpr_sprx_len);
  return 0;
}

/* -------------------------------------------------------------- request -- */
static int request_save(const char *state_dir, const job_request *r) {
  job_request copy = *r;
  memcpy(copy.magic, E2A_REQ_MAGIC, 8);
  char path[800];
  snprintf(path, sizeof path, "%s/request.bin", state_dir);
  return e2a_write_file_atomic(path, &copy, sizeof copy);
}

static int request_load(const char *state_dir, job_request *r) {
  char path[800];
  snprintf(path, sizeof path, "%s/request.bin", state_dir);
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  size_t got = 0;
  memset(r, 0, sizeof *r);
  int rc = -1;
  if (e2a_read_full(fd, r, sizeof *r, &got) == 0) {
    if (got == sizeof *r && memcmp(r->magic, E2A_REQ_MAGIC, 8) == 0) rc = 0;
    /* saved by an older version: same fields, without the E2AREQ05 tail */
    else if (got == offsetof(job_request, force) && memcmp(r->magic, E2A_REQ_MAGIC_V4, 8) == 0) {
      memset((char *)r + offsetof(job_request, force), 0, sizeof *r - offsetof(job_request, force));
      rc = 0;
    }
  }
  close(fd);
  const size_t boolean_fields[] = {offsetof(job_request, clean_leftovers), offsetof(job_request, overwrite),
    offsetof(job_request, remove_source), offsetof(job_request, resume), offsetof(job_request, verify),
    offsetof(job_request, dedup), offsetof(job_request, force)};
  for (size_t i = 0; i < sizeof boolean_fields / sizeof boolean_fields[0]; i++)
    if (((const unsigned char *)r)[boolean_fields[i]] > 1) rc = -1;
  if (rc == 0 && (!memchr(r->source, 0, sizeof r->source) || !r->source[0] ||
      !memchr(r->dest_base, 0, sizeof r->dest_base) || !memchr(r->name, 0, sizeof r->name) ||
      !memchr(r->smp_title, 0, sizeof r->smp_title) || !e2a_safe_relative(r->name) || strchr(r->name, '/') ||
      r->level < 1 || r->level > 12 || r->workers < 1 || r->workers > 16 || r->cache_mode < 0 || r->cache_mode > 1)) rc = -1;
  return rc;
}

static int stage_set(const char *state_dir, const char *stage) {
  char path[700];
  snprintf(path, sizeof path, "%s/stage", state_dir);
  return e2a_write_file_atomic(path, stage, strlen(stage));
}

static void stage_get(const char *state_dir, char *out, size_t cap) {
  char path[700], *txt = NULL;
  snprintf(path, sizeof path, "%s/stage", state_dir);
  snprintf(out, cap, "pack");
  if (e2a_read_text(path, &txt, NULL, 32) == 0) { snprintf(out, cap, "%s", txt); free(txt); }
}

/* ------------------------------------------------------------------ run -- */
/* stable code of a pause reason (the reasons themselves are English, for the log) */
static const char *pause_code(const char *why) {
  if (!why || !why[0]) return "paused_user";
  if (strncmp(why, "game launched", 13) == 0) return "paused_game";
  if (strstr(why, "destination full")) return "paused_full";
  if (strstr(why, "destination storage disconnected") || strstr(why, "storage disconnected or read-only")) return "paused_dest";
  if (strstr(why, "source disconnected")) return "paused_source";
  return "paused_user";
}

/* What "replace" may erase at the destination: a game this tool converted (its pack manifest), a conversion
 * left unfinished (its state folder), or an empty folder. Never anything else: a folder of the same name can be
 * the original game itself, or another dump, and "replace" must not cost the only copy of a game. */
static bool dest_replaceable(const char *dest) {
  char p[800];
  snprintf(p, sizeof p, "%s/ampr_assets.index", dest);
  if (e2a_is_file(p)) return true;
  snprintf(p, sizeof p, "%s/.e2a-state", dest);
  if (e2a_is_dir(p)) return true;
  DIR *d = opendir(dest);
  if (!d) return false;
  bool empty = true;
  struct dirent *de;
  while (empty && (de = readdir(d)))
    if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) empty = false;
  closedir(d);
  return empty;
}

#define FAIL(c, m) do { err_code = (c); err = (m); goto done; } while (0)

static bool backport_ampr_conflict(const char *tid, char *found, size_t cap);

static void run_job(job_request req) {
  e2a_list entries;
  memset(&entries, 0, sizeof entries);
  char dest[600] = {0}, state_dir[700] = {0}, stage[32] = "pack";
  int rc = -1;
  bool paused = false, resumable_error = false, created = false;
  const char *err = "unknown error", *err_code = "unknown";
  bool fakelib_fixed = false;
  e2a_pack_stats st;
  e2a_verify_stats vs;
  memset(&st, 0, sizeof st);
  memset(&vs, 0, sizeof vs);
  e2a_game game;
  memset(&game, 0, sizeof game);
  e2a_profile prof;
  memset(&prof, 0, sizeof prof);
  bool have_prof = false;

  pthread_mutex_lock(&g_mx);
  g_last_req = req;
  g_have_last = true;
  pthread_mutex_unlock(&g_mx);

  /* images managed by ShadowMountPlus are mounted through its API for the duration of the job */
  char src[600];
  bool smp_mounted = false;
  snprintf(src, sizeof src, "%s", req.source);
  if (req.smp_title[0]) {
    static char smp_err[256];
    char reason[32] = "", mp[600], pj[700];
    /* already mounted by ShadowMountPlus (persistent_image_mounts=1): read it there, nothing to ask */
    if (smp_image_mount_point(req.source, mp, sizeof mp)) {
      snprintf(pj, sizeof pj, "%s/sce_sys/param.json", mp);
      if (e2a_is_file(pj)) {
        snprintf(src, sizeof src, "%s", mp);
        e2a_log("image already mounted by ShadowMountPlus at %s", mp);
      }
    }
    if (strcmp(src, req.source) == 0) {
      /* ShadowMountPlus mounts nothing while a title runs (this app included) or while it prepares
       * one: the job waits for it, asking again every few seconds (never in a tight loop) */
      bool waited = false;
      for (unsigned delay = 5;; delay = delay < 20 ? delay + 5 : 20) {
        set_phase("mount", 0, "asking ShadowMountPlus to mount the image");
        smp_result r = smp_mount(req.smp_title, src, sizeof src, reason, sizeof reason, smp_err, sizeof smp_err);
        if (r == SMP_OK) { smp_mounted = true; break; }
        if (r != SMP_BUSY) {
          snprintf(src, sizeof src, "%s", req.source);
          FAIL(r == SMP_UNREACHABLE ? "smp_unreachable" : r == SMP_NOT_MANAGED ? "smp_not_managed" : r == SMP_NO_PATH ? "smp_no_path" : "smp_error",
               smp_err);
        }
        smp_game sg;   /* the title waiting, shown with its name and cover */
        memset(&sg, 0, sizeof sg);
        bool named = !waited && smp_game_info(req.smp_title, &sg) == 0;
        if (!waited) e2a_log("ShadowMountPlus is busy (%s): the conversion waits for it", reason[0] ? reason : "EBUSY");
        waited = true;
        pthread_mutex_lock(&g_mx);
        if (named) {
          snprintf(g_status.title_id, sizeof g_status.title_id, "%s", sg.title_id);
          snprintf(g_status.title, sizeof g_status.title, "%s", sg.title);
        }
        g_status.waiting = true;
        snprintf(g_status.phase, sizeof g_status.phase, "waiting");
        snprintf(g_status.code, sizeof g_status.code, "smp_busy");
        snprintf(g_status.reason, sizeof g_status.reason, "%s", reason);
        snprintf(g_status.message, sizeof g_status.message, "%s", smp_err);
        pthread_mutex_unlock(&g_mx);
        for (unsigned t = 0; t < delay * 4 && !g_cancel; t++) usleep(250000);
        g_pause = 0;   /* nothing written yet: a pause has nothing to stop */
        if (g_cancel) FAIL("cancelled", "cancelled");
      }
      pthread_mutex_lock(&g_mx);
      g_status.waiting = false;
      g_status.code[0] = g_status.reason[0] = 0;
      pthread_mutex_unlock(&g_mx);
      if (waited) e2a_notify("exfat2ampr: ShadowMountPlus mounted %s\nThe conversion starts", req.smp_title);
    }
  }

  if (e2a_read_game(src, &game) != 0) {
    if (req.resume) FAIL("source_missing", "source not available (plug the USB drive / mount the image, then resume)");
    FAIL("not_game", "not a PS5 game folder (sce_sys/param.json + eboot.bin)");
  }
  {
    char manifest[700];
    snprintf(manifest, sizeof manifest, "%s/ampr_assets.index", src);
    if (e2a_is_file(manifest)) FAIL("already_ampr", "source is already converted: copy or repair it instead");
  }
  covers_put_from_dir(game.title_id, src);   /* the image is open now: its pictures are kept for later */
  {
    char bp[400];
    if (backport_ampr_conflict(game.title_id, bp, sizeof bp))
      e2a_log("warning: the backport %s brings its own AMPR library: ShadowMountPlus overlays it on the game and it would "
              "replace the emulator, so the converted game would not start. Rename that file (for example to .off) "
              "before launching the converted game.", bp);
  }
  /* a game and its own conversion never run together */
  if (activity_title_running(game.title_id)) {
    if (req.resume) {   /* resumed while its game is still open: wait for it again, nothing is lost */
      e2a_pack_set_pause_reason("game launched: its conversion is paused and resumes when the game is closed");
      snprintf(dest, sizeof dest, "%s/%s", req.dest_base, req.name);
      activity_pause_for(game.title_id, dest);
      paused = true;
      goto done;
    }
    FAIL("game_running", "this game is running: close it, then start its conversion");
  }
  if (!req.resume && !req.force) {
    char why[200];
    int c = e2a_ampr_compat(src, why, sizeof why);
    e2a_log("AMPR compatibility of %s: %s", game.title_id, why);
    if (c == 0) FAIL("not_ampr", "not compatible: this game does not use AMPR (libSceAmpr), packing its files would break it");
  }
  if (!req.name[0]) snprintf(req.name, sizeof req.name, "%s", game.title_id);
  if (!req.dest_base[0]) snprintf(req.dest_base, sizeof req.dest_base, "%s", E2A_HOMEBREW_DIR);
#ifndef E2A_HOST
  /* only a connected, writable destination from the mount table (never a system folder) */
  if (!storage_dest_allowed(req.dest_base)) {
    if (req.resume) FAIL("dest_missing", "destination storage not connected: plug it back in, then resume");
    FAIL("dest_unavailable", "destination storage not available (not connected, read-only or not a storage folder)");
  }
#endif
  snprintf(dest, sizeof dest, "%s/%s", req.dest_base, req.name);
  snprintf(state_dir, sizeof state_dir, "%s/.e2a-state", dest);
  pthread_mutex_lock(&g_mx);
  snprintf(g_status.dest, sizeof g_status.dest, "%s", dest);
  snprintf(g_status.title_id, sizeof g_status.title_id, "%s", game.title_id);
  snprintf(g_status.title, sizeof g_status.title, "%s", game.title);
  pthread_mutex_unlock(&g_mx);
  e2a_log("job%s: %s [%s] %s -> %s (lz4 %d, %d workers, dedup %d, verify %d)", req.resume ? " (resume)" : "",
          game.title, game.title_id, src, dest, req.level, req.workers, req.dedup, req.verify);
  if (req.level >= 10)   /* LZ4HC 10-12 switch to the optimal parser: measured 35 MB/s against 150-190 at level 9 */
    e2a_log("note: LZ4 level %d is several times slower to compress than 9 for about 1%% of space", req.level);

  if (paths_overlap(src, dest)) {
    FAIL("dest_is_source", "source and destination overlap");
  }
  if (req.resume) {
    if (!e2a_is_dir(state_dir)) FAIL("nothing_to_resume", "nothing to resume in the destination");
    stage_get(state_dir, stage, sizeof stage);
    if (strcmp(stage, "pack") && strcmp(stage, "copy") && strcmp(stage, "verify")) FAIL("state_invalid", "unknown conversion stage");
  } else {
    if (e2a_is_dir(dest)) {
      if (!req.overwrite) FAIL("dest_exists", "destination already exists (tick 'replace' to overwrite)");
      if (!dest_replaceable(dest))
        FAIL("dest_not_converted", "a folder that is not a converted game has this name at the destination (the original?): never replaced");
      set_phase("cleanup", 0, "removing previous destination");
      if (e2a_rm_rf(dest) != 0) FAIL("cleanup_failed", "cannot remove previous destination");
    }
    if (e2a_mkdir_p(state_dir) != 0) FAIL("dest_create", "cannot create destination (storage not mounted or read-only?)");
    created = true;
    if (stage_set(state_dir, "pack") != 0) FAIL("state_write", "cannot persist conversion state");
  }
  pthread_mutex_lock(&g_mx);
  snprintf(g_live_dest, sizeof g_live_dest, "%s", dest);
  snprintf(g_live_src, sizeof g_live_src, "%s", src);
  pthread_mutex_unlock(&g_mx);

  set_phase("scan", 0, "scanning source");
  if (e2a_scan_tree(src, &entries, req.clean_leftovers) != 0) FAIL("scan_failed", "scan failed");
  e2a_blind_tune(&entries);

  /* per-game profile (re-applied on resume too, so the level stays consistent) */
  {
    char ppath[sizeof g_status.profile];
    if (e2a_profile_find(game.title_id, src, ppath, sizeof ppath) == 0 && e2a_profile_load(ppath, &prof) == 0) {
      size_t changed = 0;
      have_prof = true;
      e2a_profile_apply(&prof, &entries, &changed);
      if (prof.level > 0) req.level = prof.level;
      e2a_log("profile applied: %zu file(s) changed", changed);
      pthread_mutex_lock(&g_mx);
      snprintf(g_status.profile, sizeof g_status.profile, "%s", ppath);
      pthread_mutex_unlock(&g_mx);
    }
  }
  if (request_save(state_dir, &req) != 0) FAIL("state_write", "cannot persist conversion request");

  /* fakelib/libSceAmpr.sprx is part of the deployed set: make sure it is in the index with its real size/mtime */
  set_phase("fakelib", 0, "installing libSceAmpr.sprx");
  if (install_runtime(dest) != 0) FAIL("fakelib_failed", "cannot install fakelib/libSceAmpr.sprx");
  {
    char sprx[800];
    struct stat sst;
    snprintf(sprx, sizeof sprx, "%s/fakelib/libSceAmpr.sprx", dest);
    if (stat(sprx, &sst) != 0) FAIL("fakelib_failed", "fakelib missing");
    bool found = false;
    for (size_t i = 0; i < entries.n; i++) {
      if (strcasecmp(entries.v[i].rel, "fakelib/libSceAmpr.sprx") == 0) {
        found = true; entries.v[i].synthetic = true; entries.v[i].action = E2A_LOOSE;
        entries.v[i].size = (uint64_t)sst.st_size; entries.v[i].mtime = (int64_t)sst.st_mtime;
      }
    }
    if (!found) e2a_list_add(&entries, "fakelib/libSceAmpr.sprx", (uint64_t)sst.st_size, (int64_t)sst.st_mtime, true);
  }
  /* the game plugins (PlayGo stub, DLC modules): what this game needs, at their current release, written to the
   * destination's fakelib and listed with their real size like the runtime (the copy of loose files skips them) */
  {
    char rep[400];
    unsigned written = 0;
    set_phase("fakelib", 0, "installing the game plugins");
    if (plugins_apply_to(src, dest, &written, rep, sizeof rep) < 0) e2a_log("plugins of %s: %s", dest, rep);
    for (int k = 0; k < PLUGINS_N; k++) {
      if (!(written & (1u << k))) continue;
      char rel[80], full[800];
      struct stat pst;
      snprintf(rel, sizeof rel, "fakelib/%s", plugins_name(k));
      snprintf(full, sizeof full, "%s/%s", dest, rel);
      if (stat(full, &pst) != 0) continue;
      bool had = false;
      for (size_t i = 0; i < entries.n; i++)
        if (strcasecmp(entries.v[i].rel, rel) == 0) {
          had = true; entries.v[i].synthetic = true; entries.v[i].action = E2A_LOOSE;
          entries.v[i].size = (uint64_t)pst.st_size; entries.v[i].mtime = (int64_t)pst.st_mtime;
        }
      if (!had) e2a_list_add(&entries, rel, (uint64_t)pst.st_size, (int64_t)pst.st_mtime, true);
    }
  }
  e2a_list_sort(&entries);
  /* big games: the emulator keeps every block in memory (its caches shrink as the manifest grows) and refuses
   * a pack beyond 16 M blocks; bigger blocks on the biggest files keep a 300 GB game within its budget */
  {
    char note[300];
    uint64_t blocks = e2a_plan_budget(&entries, E2A_BLOCK_BUDGET, note, sizeof note);
    if (note[0]) e2a_log("%s", note);
    if (blocks > E2A_BLOCK_LIMIT)
      FAIL("too_many_blocks", "too many blocks for the emulator (over 16 million): leave the biggest files loose or give them bigger blocks in the profile");
  }

  uint64_t verify_bytes = req.verify ? entries.total_bytes : 0;
  /* the forecast: the game's own blocks sampled the way the packer will treat them */
  e2a_estimate est;
  if (est_from_list(src, &entries, req.level, &est) != 0) est_from_history(game.title_id, entries.total_bytes, false, &est);
  uint64_t image_bytes = req.smp_title[0] && e2a_is_file(req.source) ? (uint64_t)e2a_file_size(req.source) : 0;
  e2a_log("forecast: %.2f GB of files (%.2f GB left as they are) -> about %.2f GB (%s)%s%.2f GB image", entries.total_bytes / 1e9,
          est.loose / 1e9, est.predicted / 1e9, est.method, image_bytes ? ", from a " : "", image_bytes / 1e9);
  pthread_mutex_lock(&g_mx);
  g_status.image_bytes = image_bytes;
  g_status.estimated_bytes = est.predicted;
  g_status.est_ratio = est.ratio;
  snprintf(g_status.est_method, sizeof g_status.est_method, "%s", est.method);
  g_status.verify_bytes = verify_bytes;
  g_status.loose_total = est.loose;
  g_status.pack_total = entries.total_bytes - est.loose;
  g_status.source_bytes = entries.total_bytes;
  g_status.bytes_total = entries.total_bytes + verify_bytes;
  g_status.bytes_done = 0;
  g_status.eta_seconds = -1;
  pthread_mutex_unlock(&g_mx);

  if (!req.resume) {
    uint64_t fs_total = 0, fs_avail = 0;
    if (fs_space(req.dest_base, &fs_total, &fs_avail) == 0) {
      e2a_log("storage %s: %llu MiB free", req.dest_base, (unsigned long long)(fs_avail >> 20));
      /* AMPR usually halves a game; refuse when even a very good ratio could not fit */
      if (fs_avail < entries.total_bytes / 3) FAIL("space_insufficient", "not enough free space on the selected storage");
    }
  }

  char index_path[700];
  snprintf(index_path, sizeof index_path, "%s/ampr_emu.index", dest);
  if (!e2a_is_file(index_path)) {
    set_phase("index", 0, "writing ampr_emu.index");
    if (e2a_write_index(index_path, &entries) != 0) FAIL("index_failed", "index failed");
  }

  e2a_pack_opts o;
  memset(&o, 0, sizeof o);
  uint64_t verified_bits[16] = {0};   /* the volumes the pack read back: the final verification skips them */
  o.src_root = src; o.out_dir = dest; o.state_dir = state_dir;
  o.verified_bits = req.verify ? verified_bits : NULL;   /* no verification wanted: no read-back either */
  o.level = req.level; o.workers = req.workers; o.dedup = req.dedup;
  o.max_pack_size = E2A_MAX_PACK; o.cancel = &g_cancel; o.pause = &g_pause; o.progress = on_progress; o.skip = on_skip;
#ifdef E2A_HOST
  {   /* the CI: small volumes make a small game roll over, so the read-back of finished volumes is exercised */
    const char *mp = getenv("E2A_TEST_MAX_PACK_MB");
    if (mp && atoll(mp) > 0) o.max_pack_size = (uint64_t)atoll(mp) << 20;
  }
#endif
  o.test_pause_after = req.test_pause_after;
  if (have_prof && prof.has_runtime) {
    o.rt_decoded = prof.rt_decoded; o.rt_physical = prof.rt_physical; o.rt_workers = prof.rt_workers; o.rt_reserve = prof.rt_reserve;
  } else if (req.cache_mode == 1) {
    /* larger caches for big open-world games; stays well inside the emulator's 384 MiB pool */
    o.rt_decoded = 192ull << 20; o.rt_physical = 64ull << 20; o.rt_workers = 6; o.rt_reserve = 1;
  }

  /* compressible bytes decide the overall progress of the pack phase */
  uint64_t packed_total = 0, loose_total = 0;
  for (size_t i = 0; i < entries.n; i++) {
    if (entries.v[i].synthetic) continue;
    if (entries.v[i].action != E2A_LOOSE) packed_total += entries.v[i].size; else loose_total += entries.v[i].size;
  }

  /* A final manifest is the publication commit record (written after every volume).
   * A crash before stage=copy must not recompress an already committed pack. */
  if (req.resume && strcmp(stage, "pack") == 0) {
    char published[700];
    snprintf(published, sizeof published, "%s/ampr_assets.index", dest);
    if (e2a_is_file(published)) {
      resumable_error = true;
      if (e2a_manifest_actions(dest, &entries) != 0) FAIL("manifest_mismatch", "published manifest invalid; source kept");
      if (stage_set(state_dir, "copy") != 0) FAIL("state_write", "cannot persist recovered publication");
      e2a_pack_clear_checkpoint(state_dir);
      snprintf(stage, sizeof stage, "copy");
    }
  }
  double pack_secs = 0;   /* a full packing run, timed: the next forecasts learn this level's speed */
  if (strcmp(stage, "pack") == 0) {
    resumable_error = true;
    o.resume = req.resume && e2a_pack_has_checkpoint(state_dir);
    char cp[800];
    snprintf(cp, sizeof cp, "%s/state.bin", state_dir);
    if (req.resume && e2a_is_file(cp) && !o.resume) FAIL("state_invalid", "checkpoint damaged; source kept");
    begin_phase("pack", "compressing (LZ4)", 0);
    o.ctx = (void *)"pack";
    double pt0 = e2a_now();
    int r = e2a_pack(&entries, &o, &st);
    if (r == E2A_PACK_PAUSED) { paused = true; goto done; }
    if (r != E2A_PACK_OK) FAIL(g_cancel ? "cancelled" : "pack_failed", g_cancel ? "cancelled" : "pack failed (see log)");
    if (!o.resume) pack_secs = e2a_now() - pt0;
    if (stage_set(state_dir, "copy") != 0) FAIL("state_write", "cannot persist copy stage; resume to recover");
    e2a_test_crash("copy_committed");
    e2a_pack_clear_checkpoint(state_dir);
    snprintf(stage, sizeof stage, "copy");
  } else {
    /* pack already finished in a previous run: recover the final per-file actions from the manifest */
    if (e2a_manifest_actions(dest, &entries) != 0) FAIL("manifest_mismatch", "manifest does not match the source (cannot resume)");
  }
  /* bytes that ended up loose (auto-loose included) are accounted for by the copy phase */
  packed_total = 0; loose_total = 0;
  for (size_t i = 0; i < entries.n; i++) {
    if (entries.v[i].synthetic) continue;
    if (entries.v[i].action != E2A_LOOSE) packed_total += entries.v[i].size; else loose_total += entries.v[i].size;
  }
  if (pack_secs > 60.0 && packed_total > (1ull << 30)) {
    est_set_pack_rate(req.level, (double)packed_total / pack_secs);
    e2a_log("pack speed at LZ4 %d: %.0f MB/s (remembered for the next forecasts)", req.level, packed_total / pack_secs / 1e6);
  }

  if (req.resume && !strcmp(stage, "verify")) {
    char integrity[700];
    snprintf(integrity, sizeof integrity, "%s/ampr_assets.index.loose", dest);
    if (!e2a_is_file(integrity)) snprintf(stage, sizeof stage, "copy");
  }
  if (strcmp(stage, "copy") == 0) {
    resumable_error = true;
    begin_phase("copy", "copying loose files", packed_total);
    o.ctx = (void *)"copy";
    int r = e2a_copy_loose(&entries, &o);
    if (r == E2A_PACK_PAUSED) { paused = true; goto done; }
    if (r != E2A_PACK_OK) FAIL(g_cancel ? "cancelled" : "copy_failed", g_cancel ? "cancelled" : "loose copy failed (see log)");
    if (stage_set(state_dir, "verify") != 0) FAIL("state_write", "cannot persist verification stage; resume to recover");
    snprintf(stage, sizeof stage, "verify");
  }

  if (strcmp(stage, "verify") == 0 && req.verify) {
    resumable_error = true;
    begin_phase("verify", "checking every block", packed_total + loose_total);
    o.ctx = (void *)"verify";
    e2a_test_crash("verify_start");
    double vt0 = e2a_now();
    int r = e2a_verify(dest, &o, &vs);
    double vel = e2a_now() - vt0;   /* its speed, for the next forecasts (a long enough run only) */
    if (r == E2A_PACK_OK && vel > 20.0 && vs.bytes > (1ull << 30)) est_set_verify_rate((double)vs.bytes / vel);
    if (r == E2A_PACK_PAUSED) { paused = true; goto done; }
    if (r != E2A_PACK_OK) {
      resumable_error = false;
      FAIL(g_cancel ? "cancelled" : "verify_failed", g_cancel ? "cancelled" : (vs.first_error[0] ? vs.first_error : "verification failed"));
    }
  }

  /* finished: drop the checkpoint folder, write the report */
  e2a_rm_rf(state_dir);
  {
    uint64_t packed_bytes = dir_bytes(dest);
    pthread_mutex_lock(&g_mx);
    g_status.packed_bytes = packed_bytes;
    g_status.estimated_bytes = packed_bytes;
    est_remember(game.title_id, entries.total_bytes, g_status.image_bytes, packed_bytes);   /* the next forecasts learn */
    e2a_log("sizes: %.2f GB of files -> %.2f GB converted (forecast was %.2f GB, %s)", entries.total_bytes / 1e9, packed_bytes / 1e9,
            est.predicted / 1e9, est.method);
    g_status.shared_bytes = st.shared_bytes;
    pthread_mutex_unlock(&g_mx);
    char mpath[700], t[256], p[400], sj[1300], dj[1300];
    e2a_json_escape(game.title, t, sizeof t);
    e2a_json_escape(have_prof ? prof.path : "", p, sizeof p);
    e2a_json_escape(req.source, sj, sizeof sj);
    e2a_json_escape(dest, dj, sizeof dj);
    snprintf(mpath, sizeof mpath, "%s/%s.exfat2ampr.json", req.dest_base, req.name);
    FILE *f = fopen(mpath, "w");
    if (f) {
      fprintf(f, "{\"tool\":\"exfat2ampr %s\",\"title_id\":\"%s\",\"title\":\"%s\",\"version\":\"%s\",\"source\":\"%s\","
                 "\"dest\":\"%s\",\"lz4_level\":%d,\"dedup\":%s,\"verified\":%s,\"profile\":\"%s\",\"runtime\":\"%s\","
                 "\"source_bytes\":%llu,\"packed_bytes\":%llu,\"shared_bytes\":%llu,"
                 "\"files_packed\":%llu,\"files_loose\":%llu,\"chunks\":%llu,\"chunks_lz4\":%llu,\"chunks_raw\":%llu,"
                 "\"chunks_shared\":%llu,\"volumes\":%llu}\n",
              E2A_VERSION, game.title_id, t, game.version, sj, dj, req.level, req.dedup ? "true" : "false",
              req.verify ? "true" : "false", p, g_runtime_sha,
              (unsigned long long)entries.total_bytes, (unsigned long long)packed_bytes, (unsigned long long)st.shared_bytes,
              (unsigned long long)st.files_packed, (unsigned long long)st.files_loose, (unsigned long long)st.chunks,
              (unsigned long long)st.chunks_lz4, (unsigned long long)st.chunks_raw, (unsigned long long)st.chunks_shared,
              (unsigned long long)st.packs);
      fclose(f);
    }
  }
  /* ShadowMountPlus 1.7beta5+: a title whose fakelib policy is "disabled" would start without the
   * AMPR emulator and crash. "emulators" is the smallest policy that still loads libSceAmpr.sprx. */
  {
    smp_game sg;
    if (smp_game_info(game.title_id, &sg) == 0 && strcmp(sg.fakelib_mode, "disabled") == 0 &&
        smp_set_fakelib(game.title_id, "emulators") == 0)
      fakelib_fixed = true;
  }
  {   /* before its first start: anything that would stop the game, said now rather than on the home screen */
    char le[160], lw[160];
    set_phase("launch_check", 100, "checking the game can start");
    job_launch_check(dest, le, sizeof le, lw, sizeof lw);
    pthread_mutex_lock(&g_mx);
    g_status.launch_checked = true;
    snprintf(g_status.launch_errors, sizeof g_status.launch_errors, "%s", le);
    snprintf(g_status.launch_warnings, sizeof g_status.launch_warnings, "%s", lw);
    pthread_mutex_unlock(&g_mx);
  }
  if (req.remove_source) {
    set_phase("remove", 100, "removing source");
    if (smp_mounted) {
      char e2[256];
      if (smp_unmount(req.smp_title, e2, sizeof e2) == 0) smp_mounted = false;
      if (!smp_mounted && unlink(req.source) != 0) e2a_log("could not remove image %s", req.source);
    } else if (e2a_rm_rf(req.source) != 0) e2a_log("could not remove source %s", req.source);
    if (!e2a_is_dir(req.source) && !e2a_is_file(req.source))
      e2a_log("source removed as asked, after a verified conversion: %s (the game is now %s)", req.source, dest);
  }
  rc = 0;

done:
  pthread_mutex_lock(&g_mx);
  g_live_dest[0] = g_live_src[0] = 0;
  pthread_mutex_unlock(&g_mx);
  /* a drive removed in the middle: stop cleanly (resumable once it is back), never "failed" */
  if (rc != 0 && !paused && !g_cancel && dest[0] && (created || req.resume) &&
      (!e2a_is_dir(dest) || !(e2a_is_dir(src) || e2a_is_file(src)))) {
    paused = true;
    e2a_pack_set_pause_reason(!e2a_is_dir(dest) ? "destination storage disconnected: plug it back in, then resume"
                                                : "source disconnected: plug it back in, then resume");
  }
  if (smp_mounted) {  /* always give the image back to ShadowMountPlus (it blocks its scans while held) */
    char e2[256];
    set_phase("unmount", g_status.percent, "releasing the image");
    smp_unmount(req.smp_title, e2, sizeof e2);
  }
  pthread_mutex_lock(&g_mx);
  g_status.finished = (uint64_t)time(NULL);
  g_status.waiting = false;
  g_status.reason[0] = 0;
  if (rc == 0) {
    g_status.state = JOB_DONE;
    g_status.percent = 100;
    g_status.eta_seconds = 0;
    g_status.bytes_done = g_status.bytes_total;
    snprintf(g_status.phase, sizeof g_status.phase, "done");
    snprintf(g_status.code, sizeof g_status.code, "%s", fakelib_fixed ? "done_fakelib" : "done");
    snprintf(g_status.message, sizeof g_status.message, "%s -> %.230s", game.title_id, dest);
  } else if (paused) {
    g_status.state = JOB_PAUSED;
    snprintf(g_status.phase, sizeof g_status.phase, "paused");
    snprintf(g_status.code, sizeof g_status.code, "%s", pause_code(e2a_pack_pause_reason()));
    snprintf(g_status.message, sizeof g_status.message, "%s", e2a_pack_pause_reason()[0] ? e2a_pack_pause_reason() : "paused: resume from the page");
  } else {
    g_status.state = g_cancel ? JOB_CANCELLED : JOB_ERROR;
    snprintf(g_status.phase, sizeof g_status.phase, "%s", g_cancel ? "cancelled" : "error");
    snprintf(g_status.code, sizeof g_status.code, "%s", g_cancel ? "cancelled" : err_code);
    snprintf(g_status.message, sizeof g_status.message, "%s", err);
  }
  pthread_mutex_unlock(&g_mx);

  if (rc == 0) {
    e2a_notify("exfat2ampr: %s ready\n%s\n%llu MiB -> %llu MiB", game.title_id, dest,
               (unsigned long long)(entries.total_bytes >> 20), (unsigned long long)(g_status.packed_bytes >> 20));
  } else if (paused) {
    e2a_notify("exfat2ampr: %s paused\n%s", game.title_id[0] ? game.title_id : req.name, g_status.message);
  } else if (g_cancel) {
    e2a_notify("exfat2ampr: cancelled");
    if (dest[0] && (created || req.resume)) e2a_rm_rf(dest);
  } else {
    e2a_log("job failed: %s", err);
    if (resumable_error && dest[0] && e2a_is_dir(state_dir)) {
      e2a_notify("exfat2ampr: FAILED (%s)\nThe conversion can be resumed from the page.", err);
    } else {
      e2a_notify("exfat2ampr: FAILED\n%s", err);
      /* never leave a half-built game, but never touch a folder this run did not create */
      if (dest[0] && created && e2a_is_dir(dest)) e2a_rm_rf(dest);
    }
  }
  if (have_prof) e2a_profile_free(&prof);
  e2a_list_free(&entries);
}

/* ------------------------------------------------------------ scheduler -- */
static void *scheduler(void *arg) {
  (void)arg;
  for (;;) {
    pthread_mutex_lock(&g_mx);
    while (g_qlen == 0) pthread_cond_wait(&g_cv, &g_mx);
    job_request req = g_queue[0];
    memmove(&g_queue[0], &g_queue[1], (size_t)(g_qlen - 1) * sizeof g_queue[0]);
    g_qlen--;
    memset(&g_status, 0, sizeof g_status);
    g_status.state = JOB_RUNNING;
    g_last_req = req;
    snprintf(g_status.dest, sizeof g_status.dest, "%s/%s", req.dest_base, req.name);
    g_status.eta_seconds = -1;
    g_status.started = (uint64_t)time(NULL);
    snprintf(g_status.phase, sizeof g_status.phase, "starting");
    g_cancel = 0; g_pause = 0;
    pthread_mutex_unlock(&g_mx);
    e2a_pack_set_pause_reason("");
    run_job(req);
  }
  return NULL;
}

int job_init(void) {
  e2a_mkdir_p(E2A_STATE_DIR);
  e2a_mkdir_p(E2A_PROFILE_DIR);
  detect_runtime();
  return pthread_create(&g_sched, NULL, scheduler, NULL) == 0 ? 0 : -1;
}

int job_enqueue(const job_request *req) {
  job_request normalized = *req;
  if (!normalized.dest_base[0]) snprintf(normalized.dest_base, sizeof normalized.dest_base, "%s", E2A_HOMEBREW_DIR);
  if (!normalized.name[0]) {
    e2a_game game;
    if (normalized.smp_title[0]) snprintf(normalized.name, sizeof normalized.name, "%s", normalized.smp_title);
    else if (e2a_read_game(normalized.source, &game) == 0) snprintf(normalized.name, sizeof normalized.name, "%s", game.title_id);
  }
  if (!e2a_safe_relative(normalized.name) || strchr(normalized.name, '/')) return -1;
  req = &normalized;
  char dest[700];
  snprintf(dest, sizeof dest, "%s/%s", req->dest_base, req->name);
  pthread_mutex_lock(&g_op_mx);
  if (g_op_path[0] && (paths_overlap(g_op_path, req->source) || paths_overlap(g_op_path, dest))) {
    pthread_mutex_unlock(&g_op_mx); return -2;
  }
  pthread_mutex_lock(&g_mx);
  bool dup = g_status.state == JOB_RUNNING && paths_overlap(g_status.dest, dest);
  for (int i = 0; !dup && i < g_qlen; i++) {
    char queued[700];
    snprintf(queued, sizeof queued, "%s/%s", g_queue[i].dest_base, g_queue[i].name);
    dup = paths_overlap(queued, dest);
  }
  if (dup) { pthread_mutex_unlock(&g_mx); pthread_mutex_unlock(&g_op_mx); return -2; }
  if (g_qlen >= E2A_QUEUE_MAX) { pthread_mutex_unlock(&g_mx); pthread_mutex_unlock(&g_op_mx); return -1; }
  job_request r = *req;
  if (r.level < 1) r.level = 1;
  if (r.level > 12) r.level = 12;
  if (r.workers < 1) r.workers = 6;
  if (r.workers > 16) r.workers = 16;
  if (r.remove_source) r.verify = true;   /* the original is deleted only after a verified conversion */
  g_queue[g_qlen++] = r;
  int pos = g_qlen - 1 + (g_status.state == JOB_RUNNING ? 1 : 0);
  pthread_cond_broadcast(&g_cv);
  pthread_mutex_unlock(&g_mx);
  pthread_mutex_unlock(&g_op_mx);
  return pos;
}

int job_resume_level(const char *dest, int level) {
  char state_dir[700];
  snprintf(state_dir, sizeof state_dir, "%s/.e2a-state", dest);
  job_request r;
  if (request_load(state_dir, &r) != 0) return -1;
  char expected[700];
  snprintf(expected, sizeof expected, "%s/%s", r.dest_base, r.name);
  char *saved_path = realpath(expected, NULL), *requested_path = realpath(dest, NULL);
  bool same_dest = saved_path && requested_path && !strcmp(saved_path, requested_path);
  free(saved_path); free(requested_path);
  if (!same_dest) return -1;
  /* the level can change on a resume: every block is compressed on its own, blocks of both levels mix */
  if (level >= 1 && level <= 12 && level != r.level) {
    e2a_log("resume of %s: LZ4 level %d -> %d", dest, r.level, level);
    r.level = level;
  }
  r.resume = true;
  r.overwrite = false;
  r.test_pause_after = 0;   /* the test hook is one-shot */
  /* refuse a second resume of the same conversion (running or already queued) */
  pthread_mutex_lock(&g_mx);
  bool dup = g_status.state == JOB_RUNNING && strcmp(g_status.dest, dest) == 0;
  for (int i = 0; !dup && i < g_qlen; i++) {
    char qd[600];
    snprintf(qd, sizeof qd, "%s/%s", g_queue[i].dest_base, g_queue[i].name);
    if (g_queue[i].resume && strcmp(qd, dest) == 0) dup = true;
  }
  pthread_mutex_unlock(&g_mx);
  if (dup) return -2;   /* already running or waiting in the queue */
  return job_enqueue(&r) >= 0 ? 0 : -3;
}

int job_resume(const char *dest) { return job_resume_level(dest, 0); }

int job_discard(const char *dest) {
  char state_dir[700];
  snprintf(state_dir, sizeof state_dir, "%s/.e2a-state", dest);
  if (!e2a_is_dir(state_dir)) return -1;  /* only interrupted conversions can be discarded here */
  pthread_mutex_lock(&g_mx);
  bool busy = g_status.state == JOB_RUNNING && strcmp(g_status.dest, dest) == 0;
  pthread_mutex_unlock(&g_mx);
  if (busy || !job_claim_path(dest, false)) return -1;
  e2a_log("discarding %s", dest);
  int rc = e2a_rm_rf(dest);
  job_release_path();
  return rc;
}

void job_cancel(void) { g_cancel = 1; }
void job_pause(void) { g_pause = 1; }
void job_pause_because(const char *why) { e2a_pack_set_pause_reason(why); g_pause = 1; }

void job_watch_storage(void) {
  char d[600], s[600];
  pthread_mutex_lock(&g_mx);
  snprintf(d, sizeof d, "%s", g_live_dest);
  snprintf(s, sizeof s, "%s", g_live_src);
  bool running = g_status.state == JOB_RUNNING;
  pthread_mutex_unlock(&g_mx);
  if (!running || !d[0] || g_pause) return;
  if (!e2a_is_dir(d)) {
    e2a_log("destination %s disappeared: pausing", d);
    job_pause_because("destination storage disconnected: plug it back in, then resume");
  } else if (s[0] && !e2a_is_dir(s)) {
    e2a_log("source %s disappeared: pausing", s);
    job_pause_because("source disconnected: plug it back in, then resume");
  }
}
bool job_running(void) { pthread_mutex_lock(&g_mx); bool r = g_status.state == JOB_RUNNING; pthread_mutex_unlock(&g_mx); return r; }

bool job_can_retry(void) {
  pthread_mutex_lock(&g_mx);
  bool r = g_have_last && (g_status.state == JOB_ERROR || g_status.state == JOB_CANCELLED);
  pthread_mutex_unlock(&g_mx);
  return r;
}

int job_retry(void) {
  pthread_mutex_lock(&g_mx);
  bool ok = g_have_last && (g_status.state == JOB_ERROR || g_status.state == JOB_CANCELLED);
  job_request r = g_last_req;
  char dest[600];
  snprintf(dest, sizeof dest, "%s", g_status.dest);
  pthread_mutex_unlock(&g_mx);
  if (!ok) return -1;
  /* stopped after a checkpoint was written: carry on from it, else start again */
  char state_dir[700];
  snprintf(state_dir, sizeof state_dir, "%s/.e2a-state", dest);
  if (dest[0] && e2a_is_dir(state_dir)) return job_resume(dest);
  r.resume = false;
  r.test_pause_after = 0;
  return job_enqueue(&r) >= 0 ? 0 : -1;
}

int job_dismiss(void) {
  pthread_mutex_lock(&g_mx);
  int rc = -1;
  if (g_status.state != JOB_RUNNING) {
    memset(&g_status, 0, sizeof g_status);
    g_status.state = JOB_IDLE;
    g_status.eta_seconds = -1;
    g_have_last = false;
    rc = 0;
  }
  pthread_mutex_unlock(&g_mx);
  return rc;
}
bool job_busy(void) { pthread_mutex_lock(&g_mx); bool r = g_status.state == JOB_RUNNING || g_qlen > 0; pthread_mutex_unlock(&g_mx); return r; }

void job_get_status(job_status *out) {
  pthread_mutex_lock(&g_mx);
  *out = g_status;
  out->queue_len = g_qlen;
  for (int i = 0; i < g_qlen && i < E2A_QUEUE_MAX; i++) {
    const char *base = strrchr(g_queue[i].source, '/');
    snprintf(out->queue[i], sizeof out->queue[i], "%s%s", g_queue[i].resume ? "resume " : "", base ? base + 1 : g_queue[i].source);
  }
  pthread_mutex_unlock(&g_mx);
}

/* -------------------------------------------------------------- storage -- */
int job_list_destinations(char *json, size_t cap) { return storage_list_json(json, cap); }

/* -------------------------------------------------------------- sources -- */
static const char *SCAN_ROOTS[] = {
#ifdef E2A_HOST
  "homebrew", "e2a-state/shadowmnt",
#else
  "/mnt/usb0/homebrew", "/mnt/usb1/homebrew", "/mnt/usb2/homebrew", "/mnt/usb3/homebrew",
  "/mnt/usb4/homebrew", "/mnt/usb5/homebrew", "/mnt/usb6/homebrew", "/mnt/usb7/homebrew",
  "/mnt/ext0/homebrew", "/mnt/ext1/homebrew", E2A_SMP_MOUNT_DIR, "/data/etaHEN/games", "/data/homebrew",
  /* etaHEN's game folders on the drives (exFAT ignores case: EtaHen/Games), then the drives' top level:
   * dumps are kept anywhere, e.g. <drive>/<release name>/ or <drive>/<folder>/<game> */
  "/mnt/usb0/etaHEN/games", "/mnt/usb1/etaHEN/games", "/mnt/usb2/etaHEN/games", "/mnt/usb3/etaHEN/games",
  "/mnt/usb4/etaHEN/games", "/mnt/usb5/etaHEN/games", "/mnt/usb6/etaHEN/games", "/mnt/usb7/etaHEN/games",
  "/mnt/ext0/etaHEN/games", "/mnt/ext1/etaHEN/games",
  "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3", "/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7",
  "/mnt/ext0", "/mnt/ext1",
#endif
  NULL};

static bool is_image_name(const char *name) {
  size_t n = strlen(name);
  static const char *ext[] = {".exfat", ".ffpkg", ".ffpfs", ".ffpfsc", NULL};
  for (int i = 0; ext[i]; i++) { size_t m = strlen(ext[i]); if (n > m && strcasecmp(name + n - m, ext[i]) == 0) return true; }
  return false;
}

/* a mounted image shows up as <mountdir>/<image name without extension>_<hash> */
static bool image_mounted(const char *name, char *out, size_t cap) {
  char stem[300];
  snprintf(stem, sizeof stem, "%s", name);
  char *dot = strrchr(stem, '.');
  if (dot) *dot = 0;
  DIR *d = opendir(E2A_SMP_MOUNT_DIR);
  if (!d) return false;
  struct dirent *de;
  bool found = false;
  while ((de = readdir(d)) != NULL) {
    size_t sl = strlen(stem);
    if (strncmp(de->d_name, stem, sl) == 0 && (de->d_name[sl] == '_' || de->d_name[sl] == 0)) {
      snprintf(out, cap, "%s/%s", E2A_SMP_MOUNT_DIR, de->d_name);
      found = true;
      break;
    }
  }
  closedir(d);
  return found;
}

#define SMP_MAX_GAMES 512

/* ------------------------------------------------------- source removal -- */
static pthread_mutex_t g_del_mx = PTHREAD_MUTEX_INITIALIZER;
static bool g_del_active, g_del_ok;
static char g_del_path[600], g_del_title[128], g_del_code[32];
static unsigned g_del_seq;

static void *del_thread(void *arg) {
  (void)arg;
  char path[sizeof g_del_path];
  pthread_mutex_lock(&g_del_mx);
  memcpy(path, g_del_path, sizeof path);
  pthread_mutex_unlock(&g_del_mx);
  bool dir = e2a_is_dir(path);
  int rc = dir ? e2a_rm_rf(path) : unlink(path);
  if (rc == 0 && dir) {   /* the conversion report next to a converted folder goes with it */
    char report[700];
    snprintf(report, sizeof report, "%s.exfat2ampr.json", path);
    unlink(report);
  }
  job_release_path();
  e2a_log("delete %s: %s", path, rc == 0 ? "done" : "failed");
  pthread_mutex_lock(&g_del_mx);
  g_del_active = false;
  g_del_ok = rc == 0;
  snprintf(g_del_code, sizeof g_del_code, "%s", rc == 0 ? "deleted" : "delete_failed");
  g_del_seq++;
  pthread_mutex_unlock(&g_del_mx);
  return NULL;
}

/* inside root, one or two levels down (a game kept in a sub-folder); exFAT ignores case */
static bool path_in(const char *path, const char *root) {
  size_t n = strlen(root);
  if (strncasecmp(path, root, n) != 0 || path[n] != '/' || !path[n + 1] || path[n + 1] == '/') return false;
  const char *s = strchr(path + n + 1, '/');
  return !s || (s[1] && !strchr(s + 1, '/'));
}

static bool is_image_name(const char *name);

/* A copy the user wants gone or set aside: a game or app folder, or an image ShadowMountPlus knows, inside the
 * scan roots, used by nothing (the conversion, a running title, this app, a mounted image). title/tid filled. */
enum { SRC_TITLE_CAP = 128, SRC_TID_CAP = 16 };
static int source_action_check(const char *path, bool *dir_out, char title[SRC_TITLE_CAP], char tid[SRC_TID_CAP], char *code, size_t cap) {
#define REFUSE(c) do { snprintf(code, cap, "%s", (c)); return -1; } while (0)
  struct stat st;
  if (!path || !path[0] || strstr(path, "..") || strlen(path) >= sizeof g_del_path) REFUSE("not_deletable");
  if (lstat(path, &st) != 0) REFUSE("source_missing");
  if (S_ISLNK(st.st_mode)) REFUSE("not_deletable");
  bool dir = S_ISDIR(st.st_mode);
  *dir_out = dir;
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  bool placed = false;
  for (int r = 0; SCAN_ROOTS[r] && !placed; r++)   /* never inside ShadowMountPlus' mounted images */
    if (strcmp(SCAN_ROOTS[r], E2A_SMP_MOUNT_DIR) != 0) placed = path_in(path, SCAN_ROOTS[r]);
  if (dir) {
    e2a_game g;
    if (e2a_read_game(path, &g) != 0) REFUSE("not_deletable");   /* only a game or app folder */
    const char *name = g.title[0] ? g.title : g.title_id;
    size_t nl = strnlen(name, SRC_TITLE_CAP - 1);
    memcpy(title, name, nl);
    title[nl] = 0;
    memcpy(tid, g.title_id, SRC_TID_CAP);
    tid[SRC_TID_CAP - 1] = 0;
  } else {
    if (!S_ISREG(st.st_mode) || !is_image_name(base)) REFUSE("not_deletable");
    /* an image ShadowMountPlus knows (manual.lst, other roots): its title, and a place to delete it from */
    smp_game *games = calloc(SMP_MAX_GAMES, sizeof *games);
    int n = games ? smp_list_games(games, SMP_MAX_GAMES) : -1;
    for (int i = 0; i < n; i++)
      if (strcmp(games[i].source_type, "image") == 0 && strcmp(games[i].path, path) == 0) {
        placed = true;
        memcpy(title, games[i].title, SRC_TITLE_CAP);
        memcpy(tid, games[i].title_id, SRC_TID_CAP);
        title[SRC_TITLE_CAP - 1] = tid[SRC_TID_CAP - 1] = 0;
      }
    free(games);
    if (!title[0]) snprintf(title, SRC_TITLE_CAP, "%.100s", base);
  }
  if (!placed) REFUSE("not_deletable");
  /* nothing in use: the conversion (running or queued), a running title, this app, a mounted image */
  bool used = false;
  pthread_mutex_lock(&g_mx);
  if (g_status.state == JOB_RUNNING)
    used = !strcmp(g_last_req.source, path) || !strcmp(g_live_src, path) || !strcmp(g_live_dest, path) || !strcmp(g_status.dest, path);
  for (int i = 0; i < g_qlen && !used; i++) {
    char qd[600];
    snprintf(qd, sizeof qd, "%s/%s", g_queue[i].dest_base, g_queue[i].name);
    used = !strcmp(g_queue[i].source, path) || !strcmp(qd, path);
  }
  pthread_mutex_unlock(&g_mx);
  if (used || job_path_busy(path)) REFUSE("in_use");
  if (tid[0] && activity_title_running(tid)) REFUSE("game_running");
  if (!strcmp(tid, E2A_APP_TITLE_ID) && activity_app_alive()) REFUSE("app_in_use");
  char mp[600];
  if (!dir && smp_image_mount_point(path, mp, sizeof mp)) REFUSE("source_mounted");
  return 0;
#undef REFUSE
}

int job_delete_source(const char *path, char *code, size_t cap) {
#define REFUSE(c) do { snprintf(code, cap, "%s", (c)); return -1; } while (0)
  char title[128] = "", tid[16] = "";
  bool dir = false;
  if (source_action_check(path, &dir, title, tid, code, cap) != 0) return -1;
  if (!job_claim_path(path, true)) REFUSE("in_use");
  pthread_mutex_lock(&g_del_mx);
  if (g_del_active) { pthread_mutex_unlock(&g_del_mx); job_release_path(); REFUSE("delete_busy"); }
  g_del_active = true;
  memcpy(g_del_path, path, strlen(path) + 1);
  memcpy(g_del_title, title, sizeof g_del_title);
  g_del_code[0] = 0;
  pthread_mutex_unlock(&g_del_mx);
  e2a_log("deleting %s (%s) on request", path, title);
  pthread_t th;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&th, &at, del_thread, NULL) != 0) del_thread(NULL);
  pthread_attr_destroy(&at);
  return 0;
#undef REFUSE
}

/* An image set aside rather than deleted: renamed <name>.off, which ShadowMountPlus no longer mounts (two copies
 * of one title make it remount them in turn every scan). Undone by renaming it back. Folders are not concerned:
 * ShadowMountPlus finds a game folder whatever its name. */
int job_set_aside(const char *path, char *code, size_t cap) {
  char title[128] = "", tid[16] = "", to[1100];
  bool dir = false;
  if (source_action_check(path, &dir, title, tid, code, cap) != 0) return -1;
  if (dir) { snprintf(code, cap, "not_deletable"); return -1; }
  snprintf(to, sizeof to, "%s.off", path);
  struct stat tst;
  if (lstat(to, &tst) == 0) { snprintf(code, cap, "exists"); return -1; }
  if (!job_claim_path(path, true)) { snprintf(code, cap, "in_use"); return -1; }
  int moved = rename(path, to);
  job_release_path();
  if (moved != 0) { snprintf(code, cap, "delete_failed"); return -1; }
  e2a_log("set aside %s (%s): renamed %s", path, title, to);
  return 0;
}

void job_delete_status_json(char *out, size_t cap) {
  char p[1300], t[300];
  pthread_mutex_lock(&g_del_mx);
  e2a_json_escape(g_del_path, p, sizeof p);
  e2a_json_escape(g_del_title, t, sizeof t);
  snprintf(out, cap, "\"deleting\":%s,\"delete_seq\":%u,\"delete_ok\":%s,\"delete_code\":\"%s\",\"delete_path\":\"%s\",\"delete_title\":\"%s\"",
           g_del_active ? "true" : "false", g_del_seq, g_del_ok ? "true" : "false", g_del_code, p, t);
  pthread_mutex_unlock(&g_del_mx);
}

/* g: ShadowMountPlus' entry for this image (NULL when it does not manage it); same: its entry for
 * another copy of the same title (a duplicate is shown with that name and picture) */
static int add_image_json(char *json, size_t cap, size_t *pos, bool *first, const char *path, const smp_game *g,
                          bool smp_up, const smp_game *same) {
  char p[1300], t[300], rt[1300], ver[64], tid[16] = "";
  const smp_game *named = g ? g : same;
  if (g) memcpy(tid, g->title_id, sizeof tid);
  else {
    const char *base = strrchr(path, '/');
    e2a_title_from_name(base ? base + 1 : path, tid);
  }
  tid[sizeof tid - 1] = 0;
  e2a_json_escape(path, p, sizeof p);
  e2a_json_escape(named ? named->title : "", t, sizeof t);
  e2a_json_escape(g ? g->runtime_path : "", rt, sizeof rt);
  e2a_json_escape(named ? named->version : "", ver, sizeof ver);
  int64_t sz = e2a_file_size(path);
  /* selectable when ShadowMountPlus manages it (mounted on demand), or already mounted */
  const char *why = g ? "" : smp_up ? "not_managed" : "smp_offline";
  int n = snprintf(json + *pos, cap - *pos,
                   "%s{\"type\":\"image\",\"path\":\"%s\",\"smp_title\":\"%s\",\"title_id\":\"%s\",\"title\":\"%s\",\"version\":\"%s\","
                   "\"image_type\":\"%s\",\"mounted\":%s,\"mount\":\"%s\",\"size\":%lld,\"selectable\":%s,\"reason\":\"%s\",\"fakelib\":%s}",
                   *first ? "" : ",", p, g ? g->title_id : "", tid, t, ver, g ? g->image_type : "",
                   g && g->mounted ? "true" : "false", g && g->mounted ? rt : "", (long long)(sz < 0 ? 0 : sz),
                   g ? "true" : "false", why, !g || g->fakelib_enabled ? "true" : "false");
  if (n < 0 || (size_t)n >= cap - *pos) return -1;
  *pos += (size_t)n;
  *first = false;
  return 0;
}

/* The listing being built. A folder can be reached from two roots (a drive's top level and its homebrew
 * folder) and exFAT ignores case (Homebrew = homebrew): every path is listed once. */
#define SEEN_MAX 1024
typedef struct {
  char *json;
  size_t cap, pos;
  bool first, full, smp_up;
  smp_game *games;
  int ngames;
  bool *listed;
  char (*seen)[600];
  int nseen;
} scan_ctx;

static bool seen_add(scan_ctx *c, const char *path) {
  for (int i = 0; i < c->nseen; i++)
    if (!strcasecmp(c->seen[i], path)) return false;
  if (c->seen && c->nseen < SEEN_MAX) snprintf(c->seen[c->nseen++], sizeof c->seen[0], "%s", path);
  return true;
}

static void emit_folder(scan_ctx *c, const char *path, const e2a_game *g) {
  char p[1300], t[256], ampr[700], st[700];
  e2a_json_escape(path, p, sizeof p);
  e2a_json_escape(g->title, t, sizeof t);
  snprintf(ampr, sizeof ampr, "%s/ampr_assets.index", path);
  snprintf(st, sizeof st, "%s/.e2a-state", path);
  char pl[16];
  plugins_state(path, pl);
  int n = snprintf(c->json + c->pos, c->cap - c->pos,
                   "%s{\"type\":\"folder\",\"path\":\"%s\",\"title_id\":\"%s\",\"title\":\"%s\",\"version\":\"%s\",\"ampr\":%s,\"partial\":%s,\"selectable\":true,\"plugins\":\"%s\"}",
                   c->first ? "" : ",", p, g->title_id, t, g->version, e2a_is_file(ampr) ? "true" : "false", e2a_is_dir(st) ? "true" : "false", pl);
  if (n < 0 || (size_t)n >= c->cap - c->pos) { c->full = true; return; }
  c->pos += (size_t)n;
  c->first = false;
}

static void scan_dir(scan_ctx *c, const char *dir, int depth);

static void scan_entry(scan_ctx *c, const char *path, const char *name, int depth) {
  if (e2a_is_dir(path)) {
    e2a_game g;
    if (e2a_read_game(path, &g) != 0) {
      if (depth == 0) scan_dir(c, path, 1);   /* a plain folder (a release, etaHEN's Games): the games inside */
      return;
    }
    if (seen_add(c, path)) emit_folder(c, path, &g);
    return;
  }
  if (!is_image_name(name) || !seen_add(c, path)) return;
  const smp_game *match = NULL;
  for (int i = 0; i < c->ngames; i++) {
    if (strcmp(c->games[i].source_type, "image") == 0 && strcasecmp(c->games[i].path, path) == 0) {
      match = &c->games[i];
      if (c->listed) c->listed[i] = true;
      break;
    }
  }
  if (!match && !c->smp_up) {  /* API offline: fall back to an already mounted image */
    char mnt[600];
    if (image_mounted(name, mnt, sizeof mnt)) {
      e2a_game g;
      if (e2a_read_game(mnt, &g) == 0) {
        char t[256], m[1300], p[1300], pl[16];
        e2a_json_escape(g.title, t, sizeof t);
        e2a_json_escape(mnt, m, sizeof m);
        e2a_json_escape(path, p, sizeof p);
        plugins_state(mnt, pl);
        int n = snprintf(c->json + c->pos, c->cap - c->pos,
                         "%s{\"type\":\"folder\",\"path\":\"%s\",\"title_id\":\"%s\",\"title\":\"%s\",\"version\":\"%s\",\"ampr\":false,\"partial\":false,\"selectable\":true,\"image\":\"%s\",\"plugins\":\"%s\"}",
                         c->first ? "" : ",", m, g.title_id, t, g.version, p, pl);
        if (n > 0 && (size_t)n < c->cap - c->pos) { c->pos += (size_t)n; c->first = false; }
        return;
      }
    }
  }
  const smp_game *same = NULL;   /* a copy ShadowMountPlus skipped: the managed one gives its name */
  char guess[16];
  if (!match && e2a_title_from_name(name, guess))
    for (int i = 0; i < c->ngames && !same; i++)
      if (!strcmp(c->games[i].title_id, guess)) same = &c->games[i];
  if (add_image_json(c->json, c->cap, &c->pos, &c->first, path, match, c->smp_up, same) != 0) c->full = true;
}

static void scan_dir(scan_ctx *c, const char *dir, int depth) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *de;
  while (!c->full && (de = readdir(d)) != NULL) {
    if (de->d_name[0] == '.' || de->d_name[0] == '$' || !strcasecmp(de->d_name, "System Volume Information")) continue;
    char path[600];
    int n = snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
    if (n < 0 || (size_t)n >= sizeof path || strcmp(path, E2A_SMP_MOUNT_DIR) == 0) continue;
    scan_entry(c, path, de->d_name, depth);
  }
  closedir(d);
}

int job_list_sources(char *json, size_t cap) {
  scan_ctx c;
  memset(&c, 0, sizeof c);
  c.json = json;
  c.cap = cap;
  c.first = true;
  json[c.pos++] = '[';
  c.games = calloc(SMP_MAX_GAMES, sizeof *c.games);
  c.ngames = c.games ? smp_list_games(c.games, SMP_MAX_GAMES) : -1;
  c.smp_up = c.ngames >= 0;
  if (c.ngames < 0) c.ngames = 0;
  c.listed = c.ngames > 0 ? calloc((size_t)c.ngames, sizeof *c.listed) : NULL;
  c.seen = calloc(SEEN_MAX, sizeof *c.seen);
  for (int r = 0; SCAN_ROOTS[r] && !c.full; r++) {
    /* with the SMP API, mounted images are listed as images, not as raw /mnt/shadowmnt folders */
    if (strcmp(SCAN_ROOTS[r], E2A_SMP_MOUNT_DIR) == 0) {
      if (!c.smp_up) scan_dir(&c, SCAN_ROOTS[r], 1);
      continue;
    }
    scan_dir(&c, SCAN_ROOTS[r], 0);
  }
  /* what ShadowMountPlus manages elsewhere (manual.lst, its own scan paths): images and game folders */
  for (int i = 0; i < c.ngames && !c.full; i++) {
    const smp_game *g = &c.games[i];
    if ((c.listed && c.listed[i]) || !g->source_available) continue;
    if (strcmp(g->source_type, "image") == 0) {
      if (seen_add(&c, g->path) && add_image_json(c.json, c.cap, &c.pos, &c.first, g->path, g, c.smp_up, NULL) != 0) c.full = true;
    } else if (strcmp(g->source_type, "folder") == 0 && e2a_is_dir(g->path)) {
      e2a_game eg;
      if (e2a_read_game(g->path, &eg) == 0 && seen_add(&c, g->path)) emit_folder(&c, g->path, &eg);
    }
  }
  free(c.seen);
  free(c.listed);
  free(c.games);
  if (c.pos + 2 >= cap) return -1;
  json[c.pos++] = ']';
  json[c.pos] = 0;
  return 0;
}

/* ------------------------------------------------------------- preflight -- */
static void add_code(char *list, size_t cap, const char *code) {
  size_t n = strlen(list);
  snprintf(list + n, cap - n, "%s\"%s\"", n ? "," : "", code);
}

/* A backport (ShadowMountPlus overlays <scan path>/backports/<TITLE_ID>/ on the running game, fakelib included)
 * that brings its own libSceAmpr.sprx would put the system's AMPR library back in place of the emulator: the
 * converted game could not read its packed files and would not start. Returns true and the file found. */
static bool backport_ampr_conflict(const char *tid, char *found, size_t cap) {
  if (!tid || !tid[0]) return false;
  static const char *const roots[] = {"/data/homebrew", "/data/etaHEN/games", "/mnt/ext0/homebrew", "/mnt/ext0/etaHEN/games",
                                      "/mnt/ext1/homebrew", "/mnt/ext1/etaHEN/games", "/mnt/usb0/homebrew", "/mnt/usb0/etaHEN/games",
                                      "/mnt/usb1/homebrew", "/mnt/usb1/etaHEN/games", NULL};
  static const char *const names[] = {"libSceAmpr.sprx", "libsceampr.sprx", "libSceAmpr.prx", NULL};
  char p[400];
  for (int r = 0; roots[r]; r++)
    for (int n = 0; names[n]; n++) {
      snprintf(p, sizeof p, "%s/backports/%.12s/fakelib/%s", roots[r], tid, names[n]);
      if (e2a_is_file(p)) { snprintf(found, cap, "%s", p); return true; }
    }
  return false;
}

int job_preflight(const char *path, const char *smp, const char *dest_base, char *json, size_t cap) {
  char errors[256] = "", warnings[256] = "", title[256] = "", tid[16] = "", root[600] = "";
  const char *compat = "later";
  uint64_t source_bytes = 0, image_bytes = 0;
  bool packed_image = false;   /* a compressed image (pfsc) hides the size of what it holds */
  e2a_game g;
  memset(&g, 0, sizeof g);
  if (smp && smp[0]) {
    smp_game sg;
    if (!e2a_is_file(path)) add_code(errors, sizeof errors, "source_missing");
    else image_bytes = (uint64_t)e2a_file_size(path);
    if (smp_game_info(smp, &sg) == 0) {
      packed_image = strcmp(sg.image_type, "pfsc") == 0 || strcmp(sg.image_type, "pfs") == 0;
      snprintf(tid, sizeof tid, "%s", sg.title_id);
      e2a_json_escape(sg.title, title, sizeof title);
      char mp[600];
      if (sg.mounted && sg.runtime_path[0]) snprintf(root, sizeof root, "%s", sg.runtime_path);
      else if (smp_image_mount_point(path, mp, sizeof mp)) snprintf(root, sizeof root, "%s", mp);
      if (strcmp(sg.fakelib_mode, "disabled") == 0) add_code(warnings, sizeof warnings, "fakelib_disabled");
      /* not mounted yet: ShadowMountPlus will refuse while a title runs, and the job will wait */
      char running[1][16];
      if (!root[0] && activity_app_alive()) add_code(warnings, sizeof warnings, "smp_wait_app");
      else if (!root[0] && activity_running_titles(running, 1) > 0) add_code(warnings, sizeof warnings, "smp_wait_game");
    } else snprintf(tid, sizeof tid, "%s", smp);
  } else if (!path || !path[0] || !e2a_is_dir(path)) add_code(errors, sizeof errors, "source_missing");
  else if (e2a_read_game(path, &g) != 0) add_code(errors, sizeof errors, "not_game");
  else {
    snprintf(root, sizeof root, "%s", path);
    snprintf(tid, sizeof tid, "%s", g.title_id);
    e2a_json_escape(g.title, title, sizeof title);
    char ai[700];
    snprintf(ai, sizeof ai, "%s/ampr_assets.index", path);
    if (e2a_is_file(ai)) add_code(errors, sizeof errors, "already_ampr");
    smp_game sg;
    if (smp_game_info(g.title_id, &sg) == 0 && strcmp(sg.fakelib_mode, "disabled") == 0) add_code(warnings, sizeof warnings, "fakelib_disabled");
  }
  if (root[0]) {
    char why[200];
    int c = e2a_ampr_compat(root, why, sizeof why);
    compat = c == 1 ? "yes" : c == 0 ? "no" : "unknown";
    if (c == 0) add_code(errors, sizeof errors, "not_ampr");
    else if (c < 0) add_code(warnings, sizeof warnings, "ampr_unknown");
  }
  if (tid[0] && activity_title_running(tid)) add_code(errors, sizeof errors, "game_running");
  {
    char bp[400];
    if (backport_ampr_conflict(tid, bp, sizeof bp)) add_code(warnings, sizeof warnings, "backport_ampr");
  }

  /* destination: must be a connected, writable storage of the mount table */
  e2a_storage st[E2A_MAX_STORAGES], *d = NULL;
  int n = storage_scan(st, E2A_MAX_STORAGES, true);
  const char *want = dest_base && dest_base[0] ? dest_base : E2A_HOMEBREW_DIR;
  for (int i = 0; i < n; i++)
    if (!strcmp(st[i].base, want)) d = &st[i];
  /* the forecast: the game's own files sampled when they can be read (a folder, a mounted image), else what
   * an earlier run of this title taught, else the average of past conversions */
  e2a_estimate est;
  memset(&est, 0, sizeof est);
  if (!root[0] || est_from_tree(root, 9, &est) != 0) est_from_history(tid, image_bytes, packed_image, &est);
  source_bytes = est.content ? est.content : image_bytes;
  uint64_t need_typ = est.predicted, need_max = source_bytes + (256ull << 20);
  if (!est.content && packed_image) need_max = image_bytes * 2 + (256ull << 20);   /* what a pfsc holds is unknown */
  if (!d) add_code(errors, sizeof errors, "dest_missing");
  else if (!d->writable) add_code(errors, sizeof errors, "dest_readonly");
  else {
    if (need_typ && d->avail < need_typ / 10 * 9) add_code(errors, sizeof errors, "space_insufficient");
    else if (source_bytes && d->avail < need_max) add_code(warnings, sizeof warnings, "space_low");
    char out[700];
    snprintf(out, sizeof out, "%s/%s", want, tid);
    if (tid[0] && e2a_is_dir(out)) {
      if (dest_replaceable(out)) add_code(warnings, sizeof warnings, "already_exists");
      else add_code(errors, sizeof errors, "dest_not_converted");   /* the original, another dump: never replaced */
    }
  }
  /* how long it should take at each LZ4 level (packing only; the verification is the same at every level) */
  char durations[200] = "";
  for (int lv = 1, at = 0; lv <= 12 && at >= 0 && (size_t)at < sizeof durations; lv++)
    at += snprintf(durations + at, sizeof durations - (size_t)at, "%s%lld", lv > 1 ? "," : "",
                   (long long)est_duration(source_bytes, lv, false));
  int64_t verify_s = est_duration(source_bytes, 9, true) - est_duration(source_bytes, 9, false);
  int w = snprintf(json, cap,
                   "{\"title_id\":\"%s\",\"title\":\"%s\",\"source_bytes\":%llu,\"image_bytes\":%llu,\"estimate\":\"%s\","
                   "\"need_typical\":%llu,\"need_max\":%llu,\"duration_by_level\":[%s],\"verify_seconds\":%lld,"
                   "\"dest_free\":%llu,\"dest_total\":%llu,\"compatible\":\"%s\",\"errors\":[%s],\"warnings\":[%s]}",
                   tid, title, (unsigned long long)source_bytes, (unsigned long long)image_bytes, est.method ? est.method : "",
                   (unsigned long long)need_typ, (unsigned long long)need_max, durations, (long long)(source_bytes ? verify_s : -1),
                   (unsigned long long)(d ? d->avail : 0), (unsigned long long)(d ? d->total : 0), compat, errors, warnings);
  return w > 0 && (size_t)w < cap ? 0 : -1;
}

/* ---------------------------------------------------------- launch check -- */
static bool file_sha_is(const char *path, const char *sha) {
  char hex[65];
  return sha[0] && e2a_sha256_file(path, hex) == 0 && strcmp(hex, sha) == 0;
}

int job_launch_check(const char *game_dir, char *errors, size_t ecap, char *warnings, size_t wcap) {
  if (ecap) errors[0] = 0;
  if (wcap) warnings[0] = 0;
  e2a_game g;
  memset(&g, 0, sizeof g);
  if (e2a_read_game(game_dir, &g) != 0) { add_code(errors, ecap, "not_game"); return 1; }
  /* what can be fixed is fixed first: the index rebuilt from the manifest, the execute bits put back */
  char report[400], p[1300];
  int fixes = e2a_repair_converted(game_dir, report, sizeof report);
  if (fixes < 0) add_code(errors, ecap, "repair_failed");
  else if (fixes > 0) add_code(warnings, wcap, "repaired");
  if (fixes != 0) e2a_log("launch check %s: %s", game_dir, report);
  struct stat st;
  snprintf(p, sizeof p, "%s/eboot.bin", game_dir);
  if (stat(p, &st) != 0 || !(st.st_mode & 0111)) add_code(errors, ecap, "eboot_exec");
  /* the AMPR emulator the game carries (fakelib), and the one ShadowMountPlus puts in its place at launch */
  char mine[65];
  {
    e2a_sha256 c;
    uint8_t d[32];
    e2a_sha256_init(&c);
    e2a_sha256_update(&c, libSceAmpr_sprx, libSceAmpr_sprx_len);
    e2a_sha256_final(&c, d);
    for (int i = 0; i < 32; i++) snprintf(mine + i * 2, 3, "%02x", d[i]);
  }
  snprintf(p, sizeof p, "%s/fakelib/libSceAmpr.sprx", game_dir);
  if (!e2a_is_file(p)) add_code(errors, ecap, "no_emulator");
  else if (!file_sha_is(p, mine) && !file_sha_is(p, g_runtime_sha)) add_code(warnings, wcap, "emulator_other");
  /* fakelib2 replaces fakelib as a whole: without the emulator in it, the game starts without one */
  snprintf(p, sizeof p, "%s/fakelib2", game_dir);
  if (e2a_is_dir(p)) {
    snprintf(p, sizeof p, "%s/fakelib2/libSceAmpr.sprx", game_dir);
    if (!e2a_is_file(p)) add_code(errors, ecap, "fakelib2_no_ampr");
  }
  char bp[400];
  if (backport_ampr_conflict(g.title_id, bp, sizeof bp)) {
    add_code(errors, ecap, "backport_ampr");
    e2a_log("launch check %s: the backport brings its own AMPR library: %s", game_dir, bp);
  }
  smp_game sg;
  if (smp_game_info(g.title_id, &sg) == 0 && strcmp(sg.fakelib_mode, "disabled") == 0) add_code(errors, ecap, "fakelib_disabled");
  if (e2a_is_file(E2A_SPRX_SMP) && !file_sha_is(E2A_SPRX_SMP, mine)) add_code(warnings, wcap, "emus_ampr_other");
  e2a_log("launch check %s: %s%s%s%s", game_dir, errors[0] ? "would not start: " : "ready to start", errors,
          warnings[0] ? "; notes: " : "", warnings);
  return errors[0] ? 1 : 0;
}

static bool active_path_busy(const char *path) {
  pthread_mutex_lock(&g_mx);
  bool busy = paths_overlap(g_live_src, path) || paths_overlap(g_live_dest, path) ||
              (g_status.state == JOB_RUNNING && (paths_overlap(g_last_req.source, path) || paths_overlap(g_status.dest, path)));
  for (int i = 0; !busy && i < g_qlen; i++) {
    char qd[700];
    snprintf(qd, sizeof qd, "%s/%s", g_queue[i].dest_base, g_queue[i].name);
    busy = paths_overlap(g_queue[i].source, path) || paths_overlap(qd, path);
  }
  pthread_mutex_unlock(&g_mx);
  return busy;
}

bool job_path_busy(const char *path) {
  bool busy = active_path_busy(path);
  if (busy) return true;
  /* paused conversions: their saved request names the source */
  e2a_storage stg[E2A_MAX_STORAGES];
  int nstg = storage_scan(stg, E2A_MAX_STORAGES, false);
  for (int s = 0; s < nstg && !busy; s++) {
    const char *base = stg[s].base;
    DIR *d = opendir(base);
    if (!d) continue;
    struct dirent *de;
    while (!busy && (de = readdir(d)) != NULL) {
      if (de->d_name[0] == '.') continue;
      char dest[700], state_dir[800];
      snprintf(dest, sizeof dest, "%.299s/%.255s", base, de->d_name);
      snprintf(state_dir, sizeof state_dir, "%s/.e2a-state", dest);
      job_request r;
      if (!e2a_is_dir(state_dir) || request_load(state_dir, &r) != 0) continue;
      busy = paths_overlap(r.source, path) || paths_overlap(dest, path);
    }
    closedir(d);
  }
  return busy;
}

bool job_claim_path(const char *path, bool protect_paused) {
  pthread_mutex_lock(&g_op_mx);
  bool busy = g_op_path[0] || (protect_paused ? job_path_busy(path) : active_path_busy(path));
  if (!busy) snprintf(g_op_path, sizeof g_op_path, "%s", path);
  pthread_mutex_unlock(&g_op_mx);
  return !busy;
}

void job_release_path(void) {
  pthread_mutex_lock(&g_op_mx);
  g_op_path[0] = 0;
  pthread_mutex_unlock(&g_op_mx);
}

int job_list_resumable(char *json, size_t cap) {
  size_t pos = 0;
  bool first = true;
  json[pos++] = '[';
  e2a_storage stg[E2A_MAX_STORAGES];
  int nstg = storage_scan(stg, E2A_MAX_STORAGES, false);
  for (int s = 0; s < nstg; s++) {
    char base[sizeof stg[s].base];
    memcpy(base, stg[s].base, sizeof base);
    base[sizeof base - 1] = 0;
    DIR *d = opendir(base);
    if (!d) continue;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
      if (de->d_name[0] == '.') continue;
      char dest[600], state_dir[700];
      snprintf(dest, sizeof dest, "%s/%s", base, de->d_name);
      snprintf(state_dir, sizeof state_dir, "%s/.e2a-state", dest);
      job_request r;
      if (!e2a_is_dir(state_dir) || request_load(state_dir, &r) != 0) continue;
      char stage[32], dj[1300], sj[1300], nj[400];
      stage_get(state_dir, stage, sizeof stage);
      e2a_json_escape(dest, dj, sizeof dj);
      e2a_json_escape(r.source, sj, sizeof sj);
      e2a_json_escape(r.name, nj, sizeof nj);   /* free text from the page ('"' and '\' allowed) */
      pthread_mutex_lock(&g_mx);
      bool active = g_status.state == JOB_RUNNING && strcmp(g_status.dest, dest) == 0;
      pthread_mutex_unlock(&g_mx);
      int n = snprintf(json + pos, cap - pos,
                       "%s{\"dest\":\"%s\",\"source\":\"%s\",\"name\":\"%s\",\"stage\":\"%s\",\"source_available\":%s,\"active\":%s,\"level\":%d}",
                       first ? "" : ",", dj, sj, nj, stage, (e2a_is_dir(r.source) || e2a_is_file(r.source)) ? "true" : "false", active ? "true" : "false", r.level);
      if (n < 0 || (size_t)n >= cap - pos) break;
      pos += (size_t)n;
      first = false;
    }
    closedir(d);
  }
  if (pos + 2 >= cap) return -1;
  json[pos++] = ']';
  json[pos] = 0;
  return 0;
}

int job_list_profiles(char *json, size_t cap) {
  size_t pos = 0;
  bool first = true;
  json[pos++] = '[';
  DIR *d = opendir(E2A_PROFILE_DIR);
  if (d) {
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
      size_t n = strlen(de->d_name);
      if (n < 6 || strcasecmp(de->d_name + n - 5, ".toml") != 0) continue;
      char name[128], esc[300];
      snprintf(name, sizeof name, "%.*s", (int)(n - 5), de->d_name);
      e2a_json_escape(name, esc, sizeof esc);
      int w = snprintf(json + pos, cap - pos, "%s\"%s\"", first ? "" : ",", esc);
      if (w < 0 || (size_t)w >= cap - pos) break;
      pos += (size_t)w;
      first = false;
    }
    closedir(d);
  }
  if (pos + 2 >= cap) return -1;
  json[pos++] = ']';
  json[pos] = 0;
  return 0;
}
