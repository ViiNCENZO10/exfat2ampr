/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * Minimal TOML reader for ampr_pack-style profiles: [[rule]] tables with
 * action / include / exclude, and [pack] compression_level. Everything else is ignored. */
#include "profile.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static char *trim(char *s) {
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
  size_t n = strlen(s);
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
  return s;
}

/* strip a # comment that is not inside a string */
static void strip_comment(char *s) {
  bool in = false;
  for (char *p = s; *p; p++) {
    if (*p == '\\' && in && p[1]) { p++; continue; }
    if (*p == '"') in = !in;
    else if (*p == '#' && !in) { *p = 0; return; }
  }
}

static int push_str(char ***arr, size_t *n, const char *s, size_t len) {
  char **na = realloc(*arr, (*n + 1) * sizeof **arr);
  if (!na) return -1;
  *arr = na;
  char *copy = malloc(len + 1);
  if (!copy) return -1;
  memcpy(copy, s, len); copy[len] = 0;
  (*arr)[(*n)++] = copy;
  return 0;
}

/* parse every "..." string of a (possibly multi-line, already joined) array literal */
static int parse_string_array(const char *v, char ***arr, size_t *n) {
  const char *p = v;
  while ((p = strchr(p, '"')) != NULL) {
    p++;
    char buf[512];
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < sizeof buf) {
      if (*p == '\\' && p[1]) p++;
      buf[o++] = *p++;
    }
    if (*p != '"') return -1;
    p++;
    if (o && push_str(arr, n, buf, o) != 0) return -1;
  }
  return 0;
}

uint64_t e2a_parse_size(const char *v) {
  while (*v == ' ' || *v == '"' || *v == '\'') v++;
  char *end = NULL;
  double x = strtod(v, &end);
  if (!end || end == v || x < 0) return 0;
  while (*end == ' ') end++;
  uint64_t mul = 1;
  switch (*end) {
    case 'k': case 'K': mul = 1024ull; break;
    case 'm': case 'M': mul = 1024ull * 1024; break;
    case 'g': case 'G': mul = 1024ull * 1024 * 1024; break;
    default: break;
  }
  return (uint64_t)(x * (double)mul + 0.5);
}

static uint8_t size_to_shift(const char *v) {
  uint64_t n = e2a_parse_size(v);
  if (n < (16u << 10) || n > (1u << 20) || (n & (n - 1))) return 0;   /* power of two, 16 KiB .. 1 MiB */
  return (uint8_t)__builtin_ctzll(n);
}

static int parse_layout(const char *v) {
  if (strstr(v, "streaming")) return E2A_LAYOUT_STREAMING;
  if (strstr(v, "random")) return E2A_LAYOUT_RANDOM;
  if (strstr(v, "mixed")) return E2A_LAYOUT_MIXED;
  return -1;   /* auto */
}

static int parse_action(const char *v) {
  if (strstr(v, "keep")) return E2A_ACTION_KEEP;
  if (strstr(v, "compress")) return E2A_COMPRESS;
  if (strstr(v, "store")) return E2A_STORE;
  if (strstr(v, "loose")) return E2A_LOOSE;
  return -1;
}

int e2a_profile_load(const char *path, e2a_profile *p) {
  memset(p, 0, sizeof *p);
  p->level = -1;
  snprintf(p->path, sizeof p->path, "%s", path);
  char *doc = NULL;
  int64_t fsz = e2a_file_size(path);
  if (fsz > (4 << 20)) { e2a_log("profile %s is larger than 4 MiB: ignored", path); return -1; }
  if (e2a_read_text(path, &doc, NULL, 4 << 20) != 0) return -1;

  enum { SEC_NONE, SEC_RULE, SEC_PACK, SEC_RUNTIME } sec = SEC_NONE;
  e2a_rule *cur = NULL;
  char *line = doc, *next;
  const size_t ACC_CAP = 1 << 16;   /* per call: the job thread and the HTTP thread both parse profiles */
  char *acc = malloc(ACC_CAP);
  if (!acc) { free(doc); return -1; }
  while (line && *line) {
    next = strchr(line, '\n');
    if (next) *next++ = 0;
    strip_comment(line);
    char *s = trim(line);
    if (!*s) { line = next; continue; }
    if (s[0] == '[') {
      if (strncmp(s, "[[rule]]", 8) == 0) {
        e2a_rule *nr = realloc(p->rules, (p->n + 1) * sizeof *nr);
        if (!nr) { free(doc); free(acc); return -1; }
        p->rules = nr;
        cur = &p->rules[p->n++];
        memset(cur, 0, sizeof *cur);
        cur->action = E2A_COMPRESS;
        cur->layout = -1;
        sec = SEC_RULE;
      } else if (strncmp(s, "[pack]", 6) == 0) sec = SEC_PACK;
      else if (strncmp(s, "[runtime]", 9) == 0) sec = SEC_RUNTIME;
      else sec = SEC_NONE;
      line = next;
      continue;
    }
    char *eq = strchr(s, '=');
    if (!eq) { line = next; continue; }
    *eq = 0;
    char *key = trim(s);
    char *val = trim(eq + 1);
    /* join a multi-line array until the closing bracket */
    snprintf(acc, ACC_CAP, "%s", val);
    if (val[0] == '[' && !strchr(val, ']')) {
      while (next && *next && !strchr(acc, ']')) {
        char *l2 = next;
        next = strchr(l2, '\n');
        if (next) *next++ = 0;
        strip_comment(l2);
        size_t cur_len = strlen(acc);
        if (cur_len + 1 < ACC_CAP) snprintf(acc + cur_len, ACC_CAP - cur_len, " %s", trim(l2));
      }
      val = acc;
    }
    if (sec == SEC_RULE && cur) {
      if (strcmp(key, "action") == 0) { int a = parse_action(val); if (a >= 0) cur->action = (uint8_t)a; }
      else if (strcmp(key, "include") == 0) parse_string_array(val, &cur->include, &cur->ninc);
      else if (strcmp(key, "exclude") == 0) parse_string_array(val, &cur->exclude, &cur->nexc);
      else if (strcmp(key, "block_size") == 0) cur->block_shift = size_to_shift(val);
      else if (strcmp(key, "layout") == 0) cur->layout = (int8_t)parse_layout(val);
      else if (strcmp(key, "hot") == 0) cur->hot = strstr(val, "true") != NULL;
    } else if (sec == SEC_PACK) {
      if (strcmp(key, "compression_level") == 0) p->level = atoi(val);
      /* only "loose" is honoured: a packing default would bypass the safety list */
      else if (strcmp(key, "default_action") == 0) p->default_loose = parse_action(val) == E2A_LOOSE;
      else if (strcmp(key, "default_block_size") == 0 || strcmp(key, "block_size") == 0) p->default_shift = size_to_shift(val);
    } else if (sec == SEC_RUNTIME) {
      /* validated like AMPRCFG1: multiples of 16 KiB, decoded <= 512 MiB, physical <= 128 MiB, 1..16 workers */
      if (strcmp(key, "decoded_cache_bytes") == 0) { p->rt_decoded = e2a_parse_size(val) & ~((uint64_t)(16u << 10) - 1); p->has_runtime = true; }
      else if (strcmp(key, "physical_cache_bytes") == 0) { p->rt_physical = e2a_parse_size(val) & ~((uint64_t)(16u << 10) - 1); p->has_runtime = true; }
      else if (strcmp(key, "workers") == 0) { p->rt_workers = (uint32_t)atoi(val); p->has_runtime = true; }
      else if (strcmp(key, "latency_reserve_workers") == 0) { p->rt_reserve = (uint32_t)atoi(val); p->has_runtime = true; }
    }
    line = next;
  }
  free(doc);
  free(acc);
  if (p->rt_decoded > (512ull << 20)) p->rt_decoded = 512ull << 20;
  if (p->rt_physical > (128ull << 20)) p->rt_physical = 128ull << 20;
  if (p->rt_workers > 16) p->rt_workers = 16;
  if (p->has_runtime && p->rt_workers == 0) p->rt_workers = 4;
  if (p->rt_reserve >= p->rt_workers) p->rt_reserve = p->rt_workers ? p->rt_workers - 1 : 0;
  e2a_log("profile %s: %zu rules%s%s", path, p->n, p->level > 0 ? ", level" : "", p->has_runtime ? ", runtime" : "");
  return 0;
}

static bool any_match(char **pats, size_t n, const char *rel) {
  for (size_t i = 0; i < n; i++) if (e2a_glob_match(pats[i], rel)) return true;
  return false;
}

static bool is_protected(const char *rel) {
  const char *dot = strrchr(rel, '.');
  if (strncasecmp(rel, "sce_", 4) == 0 || strncasecmp(rel, "fakelib/", 8) == 0 || strncasecmp(rel, "fakelib2/", 9) == 0 ||
      strcasecmp(rel, "eboot.bin") == 0 || strncasecmp(rel, "ampr_assets", 11) == 0 || strcasecmp(rel, "ampr_emu.index") == 0)
    return true;
  if (!dot) return false;
  return strcasecmp(dot, ".prx") == 0 || strcasecmp(dot, ".sprx") == 0 || strcasecmp(dot, ".elf") == 0 ||
         strcasecmp(dot, ".self") == 0;
}

/* exact paths (no wildcard, with a folder) go into a hash table: generated profiles list thousands
 * of files and matching every file against every pattern would take minutes on the console */
static const char *exact_path(const char *pat) {
  if (pat[0] == '/') pat++;
  if (strncasecmp(pat, "app0/", 5) == 0) pat += 5;
  return strpbrk(pat, "*?") || !strchr(pat, '/') ? NULL : pat;
}

typedef struct { uint64_t h; const char *path; uint32_t rule; } exact_slot;

/* ampr_pack.py semantics: the last matching rule defines action AND geometry of the file */
void e2a_profile_apply(const e2a_profile *p, e2a_list *l, size_t *changed) {
  size_t ch = 0, nexact = 0, cap = 1, nglob = 0;
  for (size_t r = 0; r < p->n; r++)
    if (!p->rules[r].nexc) for (size_t i = 0; i < p->rules[r].ninc; i++) if (exact_path(p->rules[r].include[i])) nexact++;
  while (cap < nexact * 2) cap *= 2;
  exact_slot *tab = nexact ? calloc(cap, sizeof *tab) : NULL;
  uint32_t *glob = malloc((p->n ? p->n : 1) * sizeof *glob);
  if (changed) *changed = 0;
  if (!glob) { free(tab); return; }
  for (size_t r = 0; r < p->n; r++) {
    const e2a_rule *rule = &p->rules[r];
    bool is_glob = rule->nexc > 0 || !tab;
    for (size_t i = 0; i < rule->ninc && !is_glob; i++) if (!exact_path(rule->include[i])) is_glob = true;
    if (is_glob) glob[nglob++] = (uint32_t)r;
    if (!tab || rule->nexc) continue;
    for (size_t i = 0; i < rule->ninc; i++) {
      const char *ep = exact_path(rule->include[i]);
      if (!ep) continue;
      uint64_t h = e2a_fnv1a64_fold(ep, strlen(ep));
      size_t pos = h & (cap - 1);
      while (tab[pos].path && !(tab[pos].h == h && strcasecmp(tab[pos].path, ep) == 0)) pos = (pos + 1) & (cap - 1);
      tab[pos].h = h; tab[pos].path = ep; tab[pos].rule = (uint32_t)r;   /* later rules win */
    }
  }
  for (size_t i = 0; i < l->n; i++) {
    e2a_entry *e = &l->v[i];
    if (e->synthetic || e->size == 0) continue;
    long best = -1;
    if (tab) {
      uint64_t h = e2a_fnv1a64_fold(e->rel, strlen(e->rel));
      for (size_t pos = h & (cap - 1); tab[pos].path; pos = (pos + 1) & (cap - 1))
        if (tab[pos].h == h && strcasecmp(tab[pos].path, e->rel) == 0) { best = (long)tab[pos].rule; break; }
    }
    for (size_t g = nglob; g-- > 0;) {
      const e2a_rule *rule = &p->rules[glob[g]];
      if ((long)glob[g] <= best) break;
      if (!any_match(rule->include, rule->ninc, e->rel) || any_match(rule->exclude, rule->nexc, e->rel)) continue;
      best = (long)glob[g];
      break;
    }
    const e2a_rule *win = best >= 0 ? &p->rules[best] : NULL;
    if (!win) {
      if (p->default_loose && e->action != E2A_LOOSE) {
        e->action = E2A_LOOSE; e->block_shift = 0; e->layout = E2A_LAYOUT_MIXED; e->hot = false;
        ch++;
      } else if (p->default_shift && e->action != E2A_LOOSE && !e->block_shift) e->block_shift = p->default_shift;
      continue;
    }
    uint8_t action = win->action == E2A_ACTION_KEEP ? e->action : win->action;
    /* never pack executables, system folders or index files whatever the profile says */
    if (action != E2A_LOOSE && is_protected(e->rel)) continue;
    if (win->action == E2A_ACTION_KEEP && action == E2A_LOOSE) continue;
    uint8_t shift = win->block_shift ? win->block_shift : p->default_shift;
    uint8_t layout = win->layout >= 0 ? (uint8_t)win->layout : win->hot ? E2A_LAYOUT_RANDOM : E2A_LAYOUT_MIXED;
    if (action != e->action || shift != e->block_shift || layout != e->layout || win->hot != e->hot) ch++;
    e->action = action;
    e->block_shift = action == E2A_LOOSE ? 0 : shift;
    e->layout = action == E2A_LOOSE ? E2A_LAYOUT_MIXED : layout;
    e->hot = action != E2A_LOOSE && win->hot;
  }
  free(tab);
  free(glob);
  if (changed) *changed = ch;
}

void e2a_profile_free(e2a_profile *p) {
  for (size_t r = 0; r < p->n; r++) {
    for (size_t i = 0; i < p->rules[r].ninc; i++) free(p->rules[r].include[i]);
    for (size_t i = 0; i < p->rules[r].nexc; i++) free(p->rules[r].exclude[i]);
    free(p->rules[r].include); free(p->rules[r].exclude);
  }
  free(p->rules);
  memset(p, 0, sizeof *p);
}

int e2a_profile_find(const char *title_id, const char *source_root, char *out, size_t cap) {
  snprintf(out, cap, "%s/exfat2ampr.toml", source_root);
  if (e2a_is_file(out)) return 0;
  snprintf(out, cap, "%s/%s.toml", E2A_PROFILE_DIR, title_id);
  if (e2a_is_file(out)) return 0;
  for (int u = 0; u < 8; u++) {
    snprintf(out, cap, "/mnt/usb%d/homebrew/profiles/%s.toml", u, title_id);
    if (e2a_is_file(out)) return 0;
  }
  out[0] = 0;
  return -1;
}

/* saves and USB imports write through the same "<file>.e2a-tmp" names: one writer at a time
 * (each HTTP connection has its own thread) */
static pthread_mutex_t g_prof_mx = PTHREAD_MUTEX_INITIALIZER;

int e2a_profile_save(const char *title_id, const char *toml, size_t len) {
  if (!title_id[0] || strchr(title_id, '/') || strstr(title_id, "..")) return -1;
  if (e2a_mkdir_p(E2A_PROFILE_DIR) != 0) return -1;
  char path[400];
  snprintf(path, sizeof path, "%s/%s.toml", E2A_PROFILE_DIR, title_id);
  pthread_mutex_lock(&g_prof_mx);
  int rc = e2a_write_file_atomic(path, toml, len) != 0 ? -1 : 0;
  if (rc == 0) {
    e2a_profile p;
    rc = e2a_profile_load(path, &p);
    e2a_profile_free(&p);
  }
  pthread_mutex_unlock(&g_prof_mx);
  return rc;
}

int e2a_profile_import_usb(void) {
  int copied = 0;
  pthread_mutex_lock(&g_prof_mx);
  for (int u = 0; u < 8; u++) {
    char dir[64];
    snprintf(dir, sizeof dir, "/mnt/usb%d/homebrew/profiles", u);
    DIR *d = opendir(dir);
    if (!d) continue;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
      size_t n = strlen(de->d_name);
      if (n < 6 || strcasecmp(de->d_name + n - 5, ".toml") != 0) continue;
      char src[400], dst[400];
      snprintf(src, sizeof src, "%s/%s", dir, de->d_name);
      snprintf(dst, sizeof dst, "%s/%s", E2A_PROFILE_DIR, de->d_name);
      e2a_mkdir_p(E2A_PROFILE_DIR);
      if (e2a_copy_file(src, dst, 0) == 0) copied++;
    }
    closedir(d);
  }
  pthread_mutex_unlock(&g_prof_mx);
  if (copied) e2a_log("imported %d profile(s) from USB", copied);
  return copied;
}
