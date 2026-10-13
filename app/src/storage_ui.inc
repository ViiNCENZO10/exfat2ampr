/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * Destination list and conversion check screens. Included by main.c (shares its state); kept
 * apart so main.c stays readable. Not compiled on its own (.inc). */

/* ---------------------------------------------------------- destinations -- */
static int g_dest_sel;          /* highlighted row */
static bool g_dest_info;        /* OPTIONS: details of the highlighted storage */

static void dest_name(const dest_t *d, char *out, size_t cap) {
  if (!strcmp(d->kind, "internal")) scopy(out, cap, T(S_DEST_INTERNAL));
  else if (!strcmp(d->kind, "usb")) snprintf(out, cap, T(S_DEST_USB), d->index);
  else snprintf(out, cap, T(S_DEST_EXT), d->index);
}

/* ----------------------------------------------------------- speed test -- */
enum { SPEED_UNKNOWN, SPEED_SLOW, SPEED_OK, SPEED_FAST };

/* random reads in a big file are what a game does; without one, the write speed tells the drive's class */
static int speed_class(const dest_t *d) {
  if (d->read_mbps >= 0) return d->read_mbps >= 800 ? SPEED_FAST : d->read_mbps >= 200 ? SPEED_OK : SPEED_SLOW;
  if (d->write_mbps >= 0) return d->write_mbps >= 800 ? SPEED_FAST : d->write_mbps >= 150 ? SPEED_OK : SPEED_SLOW;
  return SPEED_UNKNOWN;
}

static int speed_of(const dest_t *d) { return d->read_mbps >= 0 ? d->read_mbps : d->write_mbps; }

/* the fastest measured destination that can take games, -1 when none was measured */
static int speed_best(void) {
  int best = -1;
  for (int i = 0; i < g_ndests; i++)
    if (g_dests[i].writable && speed_of(&g_dests[i]) > 0 && (best < 0 || speed_of(&g_dests[i]) > speed_of(&g_dests[best]))) best = i;
  return best;
}

/* the test runs on its own thread (a few seconds); speed_poll hands the result to the main thread */
static struct { char path[300], err[300]; int status, read, write; } g_speed;
static int g_speed_state;   /* 0 idle, 1 running, 2 finished */

static void *speed_thread(void *arg) {
  (void)arg;
  char form[700] = "";
  form_add(form, sizeof form, "path", g_speed.path);
  jdoc d;
  g_speed.status = api_call("/api/speedtest", form, &d);
  g_speed.read = d.root ? (int)json_num(d.root, "read_mbps", -1) : -1;
  g_speed.write = d.root ? (int)json_num(d.root, "write_mbps", -1) : -1;
  scopy(g_speed.err, sizeof g_speed.err, d.root ? json_str(d.root, "error", "") : "");
  json_free(&d);
  __atomic_store_n(&g_speed_state, 2, __ATOMIC_RELEASE);
  return NULL;
}

static void speed_start(const dest_t *d) {
  if (__atomic_load_n(&g_speed_state, __ATOMIC_ACQUIRE) == 1) return;
  if (!d->writable) { toast_err(T(S_DEST_CANT)); snd_cue_frame(SND_ERROR); return; }
  scopy(g_speed.path, sizeof g_speed.path, d->path);
  __atomic_store_n(&g_speed_state, 1, __ATOMIC_RELEASE);
  toast(T(S_SPEED_TESTING));
  snd_cue_frame(SND_SELECT);
  pthread_t th;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&th, &at, speed_thread, NULL) != 0) speed_thread(NULL);
  pthread_attr_destroy(&at);
}

static bool speed_poll(void) {
  if (__atomic_load_n(&g_speed_state, __ATOMIC_ACQUIRE) != 2) return false;
  __atomic_store_n(&g_speed_state, 0, __ATOMIC_RELEASE);
  load_dests(false);
  char msg[400];
  if (g_speed.status == 200) {
    if (g_speed.read >= 0) snprintf(msg, sizeof msg, T(S_SPEED_READ), g_speed.read);
    else snprintf(msg, sizeof msg, T(S_SPEED_WRITE), g_speed.write);
    toast(msg);
    snd_cue_frame(SND_DONE);
  } else {
    snprintf(msg, sizeof msg, T(S_OZ_FAILED), g_speed.err[0] ? g_speed.err : T(S_TIMEOUT));
    toast_err(msg);
    snd_cue_frame(SND_ERROR);
  }
  return true;
}

static const char *dest_reason(const dest_t *d) {
  if (!strcmp(d->reason, "read_only")) return T(S_DEST_READONLY);
  if (!strcmp(d->reason, "no_write")) return T(S_DEST_NOWRITE);
  if (!strcmp(d->reason, "full")) return T(S_DEST_FULL);
  return "";
}

static void dest_space(const dest_t *d, char *out, size_t cap) {
  char f[32], t[32];
  fmt_bytes(f, sizeof f, d->free);
  fmt_bytes(t, sizeof t, d->total);
  snprintf(out, cap, T(S_DEST_AVAIL), f, t);
}

/* internal console, USB stick, M.2 card */
static void draw_storage_icon(const char *kind, int x, int y, int s, uint32_t c) {
  if (!strcmp(kind, "usb")) {
    gfx_round_rect(x, y + s * 28 / 100, s * 68 / 100, s * 44 / 100, s / 10, c);
    gfx_rect(x + s * 68 / 100, y + s * 36 / 100, s * 26 / 100, s * 28 / 100, c);
    gfx_rect(x + s * 74 / 100, y + s * 42 / 100, s * 6 / 100, s * 6 / 100, C_BG_BOT);
    gfx_rect(x + s * 84 / 100, y + s * 42 / 100, s * 6 / 100, s * 6 / 100, C_BG_BOT);
  } else if (!strcmp(kind, "extended")) {
    gfx_round_rect(x, y + s * 34 / 100, s, s * 32 / 100, s / 20, c);
    for (int i = 0; i < 3; i++) gfx_rect(x + s * (12 + i * 26) / 100, y + s * 40 / 100, s * 18 / 100, s * 20 / 100, C_BG_BOT);
    gfx_rect(x + s * 90 / 100, y + s * 34 / 100, s * 4 / 100, s * 10 / 100, C_BG_BOT);
  } else {
    /* the console standing up: black core between two white wings, blue light strip */
    int cx = x + s / 2;
    gfx_round_rect(cx - s * 15 / 100, y + s * 5 / 100, s * 30 / 100, s * 90 / 100, s / 14, RGB(24, 28, 38));
    gfx_round_rect(x + s * 14 / 100, y, s * 22 / 100, s, s / 8, c);
    gfx_round_rect(x + s * 64 / 100, y, s * 22 / 100, s, s / 8, c);
    gfx_rect(cx - 2, y + s * 12 / 100, 4, s * 76 / 100, C_ACCENT);
  }
}

static void draw_usage_bar(int x, int y, int w, const dest_t *d) {
  gfx_round_rect(x, y, w, 12, 6, C_PANEL_HI);
  if (d->total <= 0) return;
  double used = 1.0 - d->free / d->total;
  if (used < 0) used = 0;
  if (used > 1) used = 1;
  uint32_t c = !d->writable ? RGBA(255, 255, 255, 60) : used > 0.95 ? C_ERR : used > 0.85 ? C_WARN : C_ACCENT;
  int fw = (int)(w * used);
  if (fw > 12) gfx_round_rect(x, y, fw, 12, 6, c);
}

static void draw_dest(void) {
  gfx_text(&FONT_TITLE, 80, 60, T(S_DESTINATION), C_TEXT, 0);
  gfx_text_wrap(&FONT_BODY, 84, 128, T(S_DEST_SUB), C_DIM, 1700, 1);
  static anim_val w_v, sel_v, info_v;
  const int x = 80, y = 190, row = 128, rows = 6;
  /* the list narrows and the details slide in from the right (OPTIONS) */
  int w = (int)anim_track(&w_v, g_dest_info ? 1120.0f : 1760.0f, 14.0f, 0.5f);
  float ie = anim_track(&info_v, g_dest_info ? 1.0f : 0.0f, 14.0f, 0.01f);
  panel(x, y, w, row * rows + 24);
  if (!g_ndests) { gfx_text(&FONT_BODY, x + 40, y + 40, T(S_NO_DEST), C_ERR, w - 80); return; }
  static int scroll;
  if (g_dest_sel < scroll) scroll = g_dest_sel;
  if (g_dest_sel >= scroll + rows) scroll = g_dest_sel - rows + 1;
  float sv = anim_track(&sel_v, (float)(g_dest_sel - scroll), 22.0f, 0.002f);
  row_push(g_dest_sel - scroll);
  draw_focus(x + 12, y + 12 + (int)(sv * (float)row), w - 24, row - 10, 22);
  gfx_pop();
  for (int i = 0; i < rows && scroll + i < g_ndests; i++) {
    const dest_t *d = &g_dests[scroll + i];
    int ry = y + 12 + i * row;
    bool chosen = scroll + i == g_opt.dest && d->writable;
    row_push(i);
    uint32_t tc = d->writable ? C_TEXT : C_DIM;
    draw_storage_icon(d->kind, x + 40, ry + 22, 72, d->writable ? RGB(225, 232, 245) : RGBA(255, 255, 255, 70));
    char name[96], space[128], line[200];
    dest_name(d, name, sizeof name);
    dest_space(d, space, sizeof space);
    int nw = gfx_text(&FONT_HEAD, x + 140, ry + 14, name, tc, w - 420);
    if (d->fstype[0]) gfx_text(&FONT_SMALL, x + 140 + nw + 16, ry + 22, d->fstype, C_DIM, 0);
    snprintf(line, sizeof line, "%s", space);
    int sw = gfx_text(&FONT_BODY, x + 140, ry + 58, line, d->writable ? C_TEXT : C_DIM, w - 420);
    int cls = speed_class(d);
    if (cls != SPEED_UNKNOWN) {   /* the measured speed, its class in colour, the fastest one recommended */
      char sp[160], num[64];
      if (d->read_mbps >= 0) snprintf(num, sizeof num, T(S_SPEED_READ), d->read_mbps);
      else snprintf(num, sizeof num, T(S_SPEED_WRITE), d->write_mbps);
      snprintf(sp, sizeof sp, "%s · %s", num, T(cls == SPEED_FAST ? S_SPEED_FAST : cls == SPEED_OK ? S_SPEED_OK : S_SPEED_SLOW));
      int spw = gfx_text(&FONT_BODY, x + 140 + sw + 28, ry + 58, sp, cls == SPEED_FAST ? C_OK : cls == SPEED_OK ? C_WARN : C_ERR,
                         w - 460 - sw - 28);
      if (scroll + i == speed_best() && !chosen) {
        int bw = gfx_text_width(&FONT_SMALL, T(S_SPEED_BEST)) + 28;
        gfx_round_rect(x + 140 + sw + 28 + spw + 16, ry + 60, bw, 32, 16, WITH_ALPHA(C_ACCENT, 70));
        gfx_text(&FONT_SMALL, x + 140 + sw + 28 + spw + 30, ry + 76 - FONT_SMALL.line / 2, T(S_SPEED_BEST), C_TEXT, 0);
      }
    }
    draw_usage_bar(x + 140, ry + 100, w - 460, d);
    const char *why = dest_reason(d);
    if (chosen) {
      int bw = gfx_text_width(&FONT_BODY, T(S_DEST_CHOSEN)) + 48;
      gfx_round_rect(x + w - 40 - bw, ry + 38, bw, 44, 22, C_OK);
      gfx_text(&FONT_BODY, x + w - 40 - bw + 24, ry + 60 - FONT_BODY.line / 2, T(S_DEST_CHOSEN), RGB(10, 30, 20), 0);
    } else if (why[0]) {
      gfx_text_right(&FONT_BODY, x + w - 40, ry + 44, why, d->writable ? C_WARN : C_ERR);
    }
    gfx_pop();
  }
  bool only_internal = g_ndests == 1 && !strcmp(g_dests[0].kind, "internal");
  if (only_internal) {
    row_push(1);
    gfx_text_wrap(&FONT_BODY, x + 40, y + row + 40, T(S_DEST_ONLY_INTERNAL), C_DIM, w - 80, 2);
    gfx_pop();
  }

  if (ie > 0.01f && g_dest_sel < g_ndests) {
    const dest_t *d = &g_dests[g_dest_sel];
    const int ix = 1240, iw = 600;
    gfx_push((int)((1.0f - ie) * 80.0f), 0, (int)(255.0f * ie));
    panel(ix, y, iw, row * rows + 24);
    char name[96], v[64];
    dest_name(d, name, sizeof name);
    gfx_text(&FONT_HEAD, ix + 32, y + 30, name, C_TEXT, iw - 64);
    struct { str_id k; const char *v; } rows_[7];
    char cap[32], fr[32];
    fmt_bytes(cap, sizeof cap, d->total);
    fmt_bytes(fr, sizeof fr, d->free);
    scopy(v, sizeof v, d->writable ? T(S_YES) : dest_reason(d));
    rows_[0].k = S_DEST_MOUNT; rows_[0].v = d->mount;
    rows_[1].k = S_DEST_DEVICE; rows_[1].v = d->device[0] ? d->device : "—";
    rows_[2].k = S_DEST_FS; rows_[2].v = d->fstype[0] ? d->fstype : "—";
    rows_[3].k = S_DEST_CAPACITY; rows_[3].v = cap;
    rows_[4].k = S_DEST_FREE; rows_[4].v = fr;
    rows_[5].k = S_DEST_WRITE; rows_[5].v = v;
    rows_[6].k = S_DEST_FOLDER; rows_[6].v = d->path;
    for (int i = 0; i < 7; i++) {
      gfx_text(&FONT_SMALL, ix + 32, y + 100 + i * 96, T(rows_[i].k), C_DIM, iw - 64);
      gfx_text(&FONT_BODY, ix + 32, y + 132 + i * 96, rows_[i].v, C_TEXT, iw - 64);
    }
    gfx_pop();
  }
}

/* --------------------------------------------------------------- checks -- */
#define MAX_CHECKS 32
typedef struct {
  int game;
  bool ok, overwrite_needed;
  char errors[6][24], warnings[6][24], compat[12];
  int nerr, nwarn;
  double source_bytes, need_typ, need_max, dest_free, dest_total, image_bytes;
} check_t;
static check_t g_checks[MAX_CHECKS];
static int g_nchecks, g_check_scroll;

static bool has_warning(const check_t *c, const char *code) {
  for (int i = 0; i < c->nwarn; i++)
    if (!strcmp(c->warnings[i], code)) return true;
  return false;
}

static const char *code_text(const char *c) {
  static const struct { const char *code; str_id s; } map[] = {
    {"source_missing", S_E_SOURCE_MISSING}, {"not_game", S_E_NOT_GAME}, {"game_running", S_E_GAME_RUNNING},
    {"not_ampr", S_COMPAT_NO}, {"ampr_unknown", S_COMPAT_UNKNOWN}, {"dest_missing", S_E_DEST_MISSING},
    {"dest_readonly", S_E_DEST_READONLY}, {"space_insufficient", S_E_SPACE}, {"space_low", S_W_SPACE_LOW},
    {"already_ampr", S_W_ALREADY_AMPR},
    /* the engine's result codes (Progress tab, start refusals) */
    {"dest_unavailable", S_E_DEST_MISSING}, {"dest_exists", S_W_EXISTS}, {"dest_is_source", S_X_DEST_IS_SOURCE},
    {"nothing_to_resume", S_X_NOTHING_TO_RESUME}, {"dest_create", S_X_DEST_CREATE}, {"scan_failed", S_X_SCAN_FAILED},
    {"fakelib_failed", S_X_FAKELIB_FAILED}, {"index_failed", S_X_INDEX_FAILED}, {"pack_failed", S_X_PACK_FAILED},
    {"copy_failed", S_X_COPY_FAILED}, {"verify_failed", S_X_VERIFY_FAILED}, {"manifest_mismatch", S_X_MANIFEST},
    {"cancelled", S_X_CANCELLED}, {"unknown", S_X_UNKNOWN}, {"smp_unreachable", S_X_SMP_UNREACHABLE},
    {"smp_not_managed", S_X_SMP_NOT_MANAGED}, {"smp_no_path", S_X_SMP_NO_PATH}, {"smp_error", S_X_SMP_ERROR},
    {"smp_busy", S_X_SMP_GAME_ACTIVE}, {"paused_user", S_X_PAUSED_USER}, {"paused_game", S_PAUSE_GAME},
    {"paused_dest", S_PAUSE_DEST}, {"paused_source", S_PAUSE_SRC}, {"paused_full", S_PAUSE_FULL}, {"done", S_X_DONE},
    {"done_fakelib", S_X_DONE_FAKELIB}, {"queue_full", S_X_QUEUE_FULL}, {"already_running", S_X_ALREADY_RUNNING}, {"bad_path", S_X_BAD_PATH}, {"bad_name", S_X_BAD_PATH},
    /* checks before a conversion */
    {"smp_wait_app", S_W_SMP_WAIT_APP}, {"smp_wait_game", S_W_SMP_WAIT_GAME}, {"fakelib_disabled", S_W_FAKELIB_DISABLED}, {"backport_ampr", S_W_BACKPORT_AMPR},
    /* deleting a copy from the list */
    {"in_use", S_X_IN_USE}, {"source_mounted", S_X_SOURCE_MOUNTED}, {"not_deletable", S_X_NOT_DELETABLE},
    {"delete_failed", S_X_DELETE_FAILED}, {"delete_busy", S_X_DELETE_BUSY}, {"app_in_use", S_X_APP_IN_USE}};
  for (size_t i = 0; i < sizeof map / sizeof *map; i++)
    if (!strcmp(map[i].code, c)) return T(map[i].s);
  return c;
}

static void run_checks(void) {
  g_nchecks = 0;
  g_check_scroll = 0;
  int targets[MAX_CHECKS], n = 0;
  for (int i = 0; i < g_ngames && n < MAX_CHECKS; i++)
    if (g_games[i].marked) targets[n++] = i;
  if (!n && g_ngames) targets[n++] = g_sel;
  for (int k = 0; k < n; k++) {
    const game_t *g = &g_games[targets[k]];
    check_t *c = &g_checks[g_nchecks++];
    memset(c, 0, sizeof *c);
    c->game = targets[k];
    char q[2600] = "";
    form_add(q, sizeof q, "path", g->path);
    if (g->image && g->smp[0]) form_add(q, sizeof q, "smp", g->smp);
    form_add(q, sizeof q, "dest", g_ndests ? g_dests[g_opt.dest].path : "");
    jdoc d;
    if (api_call("/api/check", q, &d) == 200 && d.root) {
      c->source_bytes = json_num(d.root, "source_bytes", 0);
      c->need_typ = json_num(d.root, "need_typical", 0);
      c->image_bytes = json_num(d.root, "image_bytes", 0);
      c->need_max = json_num(d.root, "need_max", 0);
      c->dest_free = json_num(d.root, "dest_free", 0);
      c->dest_total = json_num(d.root, "dest_total", 0);
      scopy(c->compat, sizeof c->compat, json_str(d.root, "compatible", "later"));
      const jnode *e = json_get(d.root, "errors"), *w = json_get(d.root, "warnings");
      for (const jnode *x = e ? e->child : NULL; x && c->nerr < 6; x = x->next)
        if (x->type == J_STR) scopy(c->errors[c->nerr++], 24, x->str);
      for (const jnode *x = w ? w->child : NULL; x && c->nwarn < 6; x = x->next)
        if (x->type == J_STR) scopy(c->warnings[c->nwarn++], 24, x->str);
    } else {
      scopy(c->errors[c->nerr++], 24, "dest_missing");
    }
    json_free(&d);
    c->overwrite_needed = has_warning(c, "already_exists") && !g_opt.overwrite;
    c->ok = c->nerr == 0 && !c->overwrite_needed;
  }
  g_dirty = true;
}

static int checks_ok(void) {
  int n = 0;
  for (int i = 0; i < g_nchecks; i++) n += g_checks[i].ok;
  return n;
}

static void draw_checks(void) {
  gfx_text(&FONT_TITLE, 80, 60, T(S_CHECK_TITLE), C_TEXT, 0);
  const int x = 80, y = 150, w = 1180, row = 168, rows = 4;
  panel(x, y, w, row * rows + 24);
  if (g_check_scroll > g_nchecks - rows) g_check_scroll = g_nchecks > rows ? g_nchecks - rows : 0;
  for (int i = 0; i < rows && g_check_scroll + i < g_nchecks; i++) {
    const check_t *c = &g_checks[g_check_scroll + i];
    const game_t *g = &g_games[c->game];
    int ry = y + 16 + i * row;
    row_push(i);
    draw_cover(g, x + 28, ry + 8, 120, 18, false);
    int tx = x + 172, tw = w - 200;
    gfx_text(&FONT_HEAD, tx, ry + 4, g->title, c->ok ? C_TEXT : C_DIM, tw - 160);
    if (!c->ok) gfx_text_right(&FONT_BODY, x + w - 32, ry + 8, T(S_SKIPPED), C_ERR);
    char a[32], b[32], m[32], line[300];
    fmt_bytes(a, sizeof a, c->source_bytes);
    fmt_bytes(b, sizeof b, c->need_typ);
    fmt_bytes(m, sizeof m, c->need_max);
    if (c->source_bytes > 0) {
      snprintf(line, sizeof line, T(S_NEED_FMT), a, b, m);
      if (c->image_bytes > 0) {   /* the image it comes from: a compressed one is smaller than what it holds */
        char im[32];
        fmt_bytes(im, sizeof im, c->image_bytes);
        size_t n = strlen(line);
        snprintf(line + n, sizeof line - n, T(S_SIZE_IMAGE), im);
      }
      gfx_text(&FONT_BODY, tx, ry + 48, line, C_TEXT, tw);
    }
    const char *compat = !strcmp(c->compat, "yes") ? T(S_COMPAT_YES) : !strcmp(c->compat, "no") ? T(S_COMPAT_NO)
                         : !strcmp(c->compat, "unknown") ? T(S_COMPAT_UNKNOWN) : T(S_COMPAT_LATER);
    uint32_t cc = !strcmp(c->compat, "yes") ? C_OK : !strcmp(c->compat, "no") ? C_ERR : C_WARN;
    gfx_text(&FONT_SMALL, tx, ry + 90, compat, cc, tw);
    /* the most important message: first error, else the overwrite note, else the first warning */
    const char *msg = NULL;
    uint32_t mc = C_WARN;
    for (int k = 0; k < c->nerr && !msg; k++)
      if (strcmp(c->errors[k], "not_ampr")) { msg = code_text(c->errors[k]); mc = C_ERR; }
    if (!msg && c->overwrite_needed) { msg = T(S_W_EXISTS); mc = C_ERR; }
    if (!msg && has_warning(c, "already_exists")) msg = T(S_W_EXISTS_OVERWRITE);
    for (int k = 0; k < c->nwarn && !msg; k++)
      if (strcmp(c->warnings[k], "ampr_unknown") && strcmp(c->warnings[k], "already_exists")) msg = code_text(c->warnings[k]);
    if (msg) gfx_text(&FONT_SMALL, tx, ry + 124, msg, mc, tw);
    gfx_pop();
  }

  /* right: destination, totals, options */
  const int dx = 1300, dw = 540;
  panel(dx, y, dw, row * rows + 24);
  row_push(1);
  int cy = y + 30;
  if (g_ndests) {
    const dest_t *d = &g_dests[g_opt.dest];
    char name[96], space[128];
    dest_name(d, name, sizeof name);
    dest_space(d, space, sizeof space);
    gfx_text(&FONT_SMALL, dx + 32, cy, T(S_DESTINATION), C_DIM, 0);
    draw_storage_icon(d->kind, dx + 32, cy + 40, 56, RGB(225, 232, 245));
    gfx_text(&FONT_HEAD, dx + 108, cy + 38, name, C_TEXT, dw - 140);
    gfx_text(&FONT_SMALL, dx + 108, cy + 82, space, C_DIM, dw - 140);
    cy += 140;
  }
  double typ = 0, max = 0, avail = g_ndests ? g_dests[g_opt.dest].free : 0;
  for (int i = 0; i < g_nchecks; i++)
    if (g_checks[i].ok) { typ += g_checks[i].need_typ; max += g_checks[i].need_max; }
  char a[32], b[32], line[200];
  fmt_bytes(a, sizeof a, typ);
  fmt_bytes(b, sizeof b, max);
  gfx_text(&FONT_SMALL, dx + 32, cy, T(S_TOTAL_NEED), C_DIM, 0);
  snprintf(line, sizeof line, "≈ %s", a);
  gfx_text(&FONT_HEAD, dx + 32, cy + 32, line, max > avail ? C_WARN : C_OK, dw - 64);
  snprintf(line, sizeof line, T(S_MAX_FMT), b);
  gfx_text(&FONT_SMALL, dx + 32, cy + 80, line, C_DIM, dw - 64);
  if (max > avail && typ <= avail) gfx_text_wrap(&FONT_SMALL, dx + 32, cy + 112, T(S_W_SPACE_LOW), C_WARN, dw - 64, 2);
  cy += 190;
  snprintf(line, sizeof line, "LZ4 %d · %s · %s", g_opt.level, g_opt.cache ? T(S_CACHE_LARGE) : T(S_CACHE_STD),
           g_opt.verify ? T(S_VERIFY) : "—");
  gfx_text_wrap(&FONT_SMALL, dx + 32, cy, line, C_DIM, dw - 64, 2);
  cy += 80;
  gfx_text_wrap(&FONT_SMALL, dx + 32, cy, g_opt.remove ? T(S_REMOVE_NOTE) : T(S_ORIGINALS_KEPT), g_opt.remove ? C_WARN : C_OK, dw - 64, 3);
  cy += 110;
  int ok = checks_ok();
  snprintf(line, sizeof line, T(S_WILL_CONVERT), ok, g_nchecks - ok);
  gfx_text_wrap(&FONT_BODY, dx + 32, cy, ok ? line : T(S_NOTHING_TO_START), ok ? C_TEXT : C_ERR, dw - 64, 2);
  gfx_pop();
}

static void start_checked(void) {
  int ok = 0;
  g_app_started_at = plat_ms();
  for (int i = 0; i < g_nchecks; i++)
    if (g_checks[i].ok && start_one(&g_games[g_checks[i].game]) == 0) { ok++; g_games[g_checks[i].game].marked = false; }
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
