/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * "Optimize this game" and "Convert everything" from the controller. Included by main.c (shares its state);
 * not compiled on its own (.inc).
 *
 * The optimization is the engine's trace work (/api/traces, /api/trace/<op>, the web page's section 7):
 * the trace emulator goes into the game, the player plays a session, the session is collected, a profile is
 * built from every session (block sizes, layout, hot files, the emulator's caches), the trace emulator is
 * removed, and the next conversion of the game uses the profile. Game folders kept in a homebrew folder. */

enum { OZ_ENABLE, OZ_PLAY, OZ_COLLECT, OZ_PROFILE, OZ_DISABLE, OZ_CONVERT, OZ_STEPS };
static int g_oz_sel;
static char g_oz_path[600], g_oz_tid[16], g_oz_title[160];
static struct { bool found, converted, trace_mode, pending, profile; int collected; } g_oz;

static void oz_refresh(void) {
  jdoc d;
  memset(&g_oz, 0, sizeof g_oz);
  if (api_call("/api/traces", NULL, &d) == 200 && d.root && d.root->type == J_ARR)
    for (const jnode *n = d.root->child; n; n = n->next) {
      if (strcmp(json_str(n, "path", ""), g_oz_path) != 0) continue;
      g_oz.found = true;
      g_oz.converted = json_bool(n, "converted", false);
      g_oz.trace_mode = json_bool(n, "trace_mode", false);
      g_oz.pending = json_bool(n, "pending", false);
      g_oz.profile = json_bool(n, "profile", false);
      g_oz.collected = (int)json_num(n, "collected", 0);
    }
  json_free(&d);
}

static bool oz_done(int step) {
  switch (step) {
    case OZ_ENABLE: return g_oz.trace_mode || g_oz.collected > 0;
    case OZ_PLAY: return g_oz.pending || g_oz.collected > 0;
    case OZ_COLLECT: return g_oz.collected > 0 && !g_oz.pending;
    case OZ_PROFILE: return g_oz.profile;
    case OZ_DISABLE: return g_oz.profile && !g_oz.trace_mode;
    default: return false;
  }
}

static void open_optimize(const game_t *g) {
  if (g->image) { toast_err(T(S_OZ_FOLDER_ONLY)); return; }
  scopy(g_oz_path, sizeof g_oz_path, g->path);
  scopy(g_oz_tid, sizeof g_oz_tid, g->title_id);
  scopy(g_oz_title, sizeof g_oz_title, g->title);
  oz_refresh();
  if (!g_oz.found) { toast_err(T(S_OZ_FOLDER_ONLY)); return; }
  g_oz_sel = 0;
  while (g_oz_sel < OZ_CONVERT && oz_done(g_oz_sel)) g_oz_sel++;   /* where the player stopped */
  g_screen = SCR_OPTIMIZE;
  snd_cue_frame(SND_SELECT);
}

/* one engine step; the engine answers {"ok":true,"info":...} or {"error":...} (English, shown as it is) */
static bool oz_call(const char *op, char *info, size_t cap) {
  char path[64], form[1400] = "";
  snprintf(path, sizeof path, "/api/trace/%s", op);
  if (!strcmp(op, "generate")) form_add(form, sizeof form, "title_id", g_oz_tid);
  else form_add(form, sizeof form, "path", g_oz_path);
  jdoc d;
  int st = api_call(path, form, &d);
  bool ok = st == 200;
  scopy(info, cap, d.root ? json_str(d.root, ok ? "info" : "error", "") : "");
  if (!ok && !info[0]) scopy(info, cap, st < 0 ? T(S_TIMEOUT) : "?");
  json_free(&d);
  return ok;
}

static void oz_fail(const char *why) {
  char msg[400];
  snprintf(msg, sizeof msg, T(S_OZ_FAILED), why);
  toast_err(msg);
}

static void oz_run(int step) {
  char info[300], msg[400];
  switch (step) {
    case OZ_ENABLE:
      if (oz_call("enable", info, sizeof info)) { toast(T(S_OZ_ENABLED)); snd_cue_frame(SND_DONE); } else oz_fail(info);
      break;
    case OZ_PLAY: toast(T(S_OZ_PLAY_HINT)); snd_cue_frame(SND_SELECT); break;
    case OZ_COLLECT:
      if (oz_call("collect", info, sizeof info)) {
        snprintf(msg, sizeof msg, T(S_OZ_SESSIONS), atoi(info));
        toast(msg);
        snd_cue_frame(SND_DONE);
      } else oz_fail(info);
      break;
    case OZ_PROFILE:
      if (oz_call("generate", info, sizeof info)) { toast(T(S_OZ_PROFILE_READY)); snd_cue_frame(SND_DONE); } else oz_fail(info);
      break;
    case OZ_DISABLE:
      if (oz_call("disable", info, sizeof info)) { toast(T(S_OZ_DISABLED)); snd_cue_frame(SND_DONE); } else oz_fail(info);
      break;
    case OZ_CONVERT:
      if (g_oz.converted) { toast(T(S_OZ_NEED_SOURCE)); snd_cue_frame(SND_SELECT); break; }
      for (int i = 0; i < g_ngames; i++) {
        g_games[i].marked = !strcmp(g_games[i].path, g_oz_path) && g_games[i].selectable;
        if (g_games[i].marked) g_sel = i;
      }
      load_dests(false);
      g_opt.remove = false;   /* the source stays: a profile is applied to the next conversion of it */
      g_opt_sel = OPT_START;
      g_screen = SCR_OPTIONS;
      snd_cue_frame(SND_SELECT);
      return;
    default: break;
  }
  oz_refresh();
}

static void draw_optimize(void) {
  static const str_id steps[OZ_STEPS] = {S_OZ_S1, S_OZ_S2, S_OZ_S3, S_OZ_S4, S_OZ_S5, S_OZ_S6};
  static anim_val sel_v;
  const int x = 80, y = 150, w = 1180, row = 100;
  gfx_text(&FONT_TITLE, x, 60, T(S_OZ_TITLE), C_TEXT, 0);
  panel(x, y + 20, w, row * OZ_STEPS + 40);
  float sv = anim_track(&sel_v, (float)g_oz_sel, 22.0f, 0.002f);
  draw_focus(x + 12, y + 40 + (int)(sv * (float)row), w - 24, row - 10, 20);
  for (int i = 0; i < OZ_STEPS; i++) {
    int ry = y + 40 + i * row;
    row_push(i);
    bool done = oz_done(i);
    gfx_circle(x + 62, ry + 45, 24, done ? C_OK : C_PANEL_HI);
    if (done) {
      gfx_line(x + 51, ry + 46, x + 59, ry + 54, 4, RGB(255, 255, 255));
      gfx_line(x + 59, ry + 54, x + 74, ry + 37, 4, RGB(255, 255, 255));
    } else {
      char num[4];
      snprintf(num, sizeof num, "%d", i + 1);
      gfx_text_center(&FONT_HEAD, x + 62, ry + 45 - FONT_HEAD.line / 2, num, C_TEXT);
    }
    gfx_text_wrap(&FONT_BODY, x + 112, ry + 12, T(steps[i]), i == g_oz_sel ? C_TEXT : C_DIM, w - 160, 2);
    gfx_pop();
  }
  /* the game, what has been gathered so far, what it is for */
  const int dx = 1300, dw = 540;
  panel(dx, y + 20, dw, row * OZ_STEPS + 40);
  game_t fake;
  memset(&fake, 0, sizeof fake);
  scopy(fake.path, sizeof fake.path, g_oz_path);
  scopy(fake.title_id, sizeof fake.title_id, g_oz_tid);
  scopy(fake.title, sizeof fake.title, g_oz_title);
  const game_t *g = NULL;
  for (int i = 0; i < g_ngames && !g; i++)
    if (!strcmp(g_games[i].path, g_oz_path)) g = &g_games[i];
  draw_cover(g ? g : &fake, dx + (dw - 200) / 2, y + 56, 200, 22, true);
  int ty = y + 284;
  gfx_text(&FONT_HEAD, dx + 32, ty, g_oz_title, C_TEXT, dw - 64);
  ty += 52;
  char line[200];
  snprintf(line, sizeof line, T(S_OZ_SESSIONS), g_oz.collected);
  gfx_text(&FONT_BODY, dx + 32, ty, g_oz.pending ? T(S_OZ_PENDING) : line, g_oz.pending ? C_WARN : C_DIM, dw - 64);
  ty += 44;
  if (g_oz.profile) {
    gfx_text(&FONT_BODY, dx + 32, ty, T(S_OZ_PROFILE_READY), C_OK, dw - 64);
    ty += 44;
  }
  gfx_text_wrap(&FONT_SMALL, dx + 32, ty + 10, T(S_OZ_INTRO), C_DIM, dw - 64, 6);
}

static void optimize_input(uint32_t p) {
  if (p & BTN_CIRCLE) { g_screen = SCR_MAIN; snd_cue_frame(SND_BACK); return; }
  if (p & BTN_UP) move_sel(&g_oz_sel, OZ_STEPS, -1);
  if (p & BTN_DOWN) move_sel(&g_oz_sel, OZ_STEPS, 1);
  if (p & BTN_TRIANGLE) { oz_refresh(); toast(T(S_REFRESHED)); snd_cue_frame(SND_SELECT); }
  if (p & BTN_CROSS) oz_run(g_oz_sel);
}

/* Convert everything: every game that can be converted, one copy per title, then the usual options */
static void convert_all(void) {
  int n = 0;
  for (int i = 0; i < g_ngames; i++) {
    game_t *g = &g_games[i];
    bool playing = g->title_id[0] && !strcmp(g->title_id, g_st.game_running);
    bool twin = false;
    for (int k = 0; k < i && !twin; k++) twin = g_games[k].marked && g->title_id[0] && !strcmp(g_games[k].title_id, g->title_id);
    g->marked = g->selectable && !g->ampr && !g->partial && !playing && !twin;
    n += g->marked;
  }
  if (!n) { toast_err(T(S_CONVERT_ALL_NONE)); snd_cue_frame(SND_ERROR); return; }
  char msg[160];
  snprintf(msg, sizeof msg, "%d %s", n, T(S_GAMES_SELECTED));
  toast(msg);
  load_dests(false);
  g_opt.remove = false;
  g_opt_sel = OPT_START;
  g_screen = SCR_OPTIONS;
  snd_cue_frame(SND_SELECT);
}
