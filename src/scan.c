/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * Source tree scan + blind pack profile (same spirit as Lazy_AMPR's safety list). */
#include "scan.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Everything matching these stays loose (never packed). */
static const char *LOOSE_EXT[] = {
  ".elf", ".self", ".prx", ".sprx", ".bin", ".dat",
  ".json", ".xml", ".utoc",
  ".mp4", ".webm", ".mkv", ".bk2", ".bik", ".usm",
  ".ogg", ".opus", ".mp3", ".at9", ".wem",
  ".zip", ".7z", ".rar", ".crc", ".runtime", ".index", ".pak", NULL};
static const char *LOOSE_TOP_DIRS[] = {
  "sce_module", "sce_sys", "system", "fakelib", "mods", "save", "trophy2", "uds", "_DUPLEX_", "decrypted",
  "movie", "movies", "video", "videos", "cinematic", "cutscene", NULL};
static const char *LOOSE_NAMES[] = {"eboot.bin", "ampr_emu.index", "ampr_commands.bin", "apr_emu.log", NULL};
static const char *LOOSE_PREFIX[] = {"movie", "soundbank", "wem", NULL};

static const char *base_name(const char *rel) { const char *s = strrchr(rel, '/'); return s ? s + 1 : rel; }

static bool ends_with_ci(const char *s, const char *suf) {
  size_t n = strlen(s), m = strlen(suf);
  return n >= m && strcasecmp(s + n - m, suf) == 0;
}

bool e2a_is_leftover(const char *rel) {
  if (strncmp(rel, "_DUPLEX_/", 9) == 0 || strcmp(rel, "_DUPLEX_") == 0) return true;
  if (strncasecmp(rel, "Unreal Engine/Saved/", 20) == 0 || strcasecmp(rel, "Unreal Engine/Saved") == 0) return true;
  /* <project>/Saved/... : second component == Saved */
  const char *s = strchr(rel, '/');
  if (s && (strncasecmp(s, "/Saved/", 7) == 0 || strcasecmp(s, "/Saved") == 0)) return true;
  return false;
}

/* Without traces (Lazy_AMPR blind profile): audio is read in small random pieces and early,
 * so 16 KiB random hot blocks; everything else keeps 64 KiB mixed blocks. */
static const char *AUDIO_DIRS[] = {"audio", "sound", "sounds", "sfx", "music", "voice", "voices", "vo", NULL};

void e2a_blind_tune(e2a_list *l) {
  for (size_t i = 0; i < l->n; i++) {
    e2a_entry *e = &l->v[i];
    if (e->action == E2A_LOOSE || e->synthetic) continue;
    const char *slash = strchr(e->rel, '/');
    if (!slash) continue;
    size_t top = (size_t)(slash - e->rel);
    for (int k = 0; AUDIO_DIRS[k]; k++) {
      if (strlen(AUDIO_DIRS[k]) == top && strncasecmp(e->rel, AUDIO_DIRS[k], top) == 0) {
        e->block_shift = 14;
        e->layout = E2A_LAYOUT_RANDOM;
        e->hot = true;
        break;
      }
    }
  }
}

static uint64_t blocks_of(const e2a_entry *e) {
  unsigned shift = e->block_shift ? e->block_shift : E2A_BLOCK_SHIFT;
  uint64_t block = 1ull << shift;
  return (e->size + block - 1) / block;
}

static int by_size_desc(const void *a, const void *b) {
  const e2a_entry *const *x = a, *const *y = b;
  return (*x)->size < (*y)->size ? 1 : (*x)->size > (*y)->size ? -1 : 0;
}

uint64_t e2a_plan_budget(e2a_list *l, uint64_t budget, char *note, size_t cap) {
  note[0] = 0;
  uint64_t total = 0;
  size_t ncand = 0;
  for (size_t i = 0; i < l->n; i++) {
    const e2a_entry *e = &l->v[i];
    if (e->action == E2A_LOOSE || e->synthetic) continue;
    total += blocks_of(e);
    if (!e->hot && e->layout != E2A_LAYOUT_RANDOM && e->size >= (64ull << 20)) ncand++;
  }
  if (total <= budget || ncand == 0) return total;
  /* the biggest files first: one of them weighs as much as thousands of small ones */
  const e2a_entry **cand = malloc(ncand * sizeof *cand);
  if (!cand) return total;
  size_t k = 0;
  for (size_t i = 0; i < l->n; i++) {
    const e2a_entry *e = &l->v[i];
    if (e->action == E2A_LOOSE || e->synthetic || e->hot || e->layout == E2A_LAYOUT_RANDOM || e->size < (64ull << 20)) continue;
    cand[k++] = e;
  }
  qsort(cand, ncand, sizeof *cand, by_size_desc);
  uint64_t before = total;
  int changed = 0;
  unsigned top = 0;
  for (unsigned target = 17; target <= 19 && total > budget; target++) {   /* 128 KiB, 256 KiB, 512 KiB */
    for (size_t i = 0; i < ncand && total > budget; i++) {
      e2a_entry *e = (e2a_entry *)cand[i];
      unsigned shift = e->block_shift ? e->block_shift : E2A_BLOCK_SHIFT;
      if (shift >= target) continue;
      total -= blocks_of(e);
      e->block_shift = (uint8_t)target;
      total += blocks_of(e);
      changed++;
      top = target;
    }
  }
  free(cand);
  if (changed)
    snprintf(note, cap, "big game: %llu blocks estimated, %d large files moved to blocks of up to %u KiB: %llu blocks%s",
             (unsigned long long)before, changed, 1u << (top - 10), (unsigned long long)total, total > budget ? " (still over the budget)" : "");
  return total;
}

int e2a_classify(const char *rel) {
  const char *name = base_name(rel);
  for (int i = 0; LOOSE_NAMES[i]; i++) if (strcasecmp(name, LOOSE_NAMES[i]) == 0) return E2A_LOOSE;
  for (int i = 0; LOOSE_EXT[i]; i++) if (ends_with_ci(name, LOOSE_EXT[i])) return E2A_LOOSE;
  for (int i = 0; LOOSE_PREFIX[i]; i++) if (strncasecmp(name, LOOSE_PREFIX[i], strlen(LOOSE_PREFIX[i])) == 0) return E2A_LOOSE;
  const char *slash = strchr(rel, '/');
  if (!slash) return E2A_LOOSE;  /* root files without a folder: keep loose (safe) */
  size_t top_len = (size_t)(slash - rel);
  for (int i = 0; LOOSE_TOP_DIRS[i]; i++)
    if (strlen(LOOSE_TOP_DIRS[i]) == top_len && strncasecmp(rel, LOOSE_TOP_DIRS[i], top_len) == 0) return E2A_LOOSE;
  return E2A_COMPRESS;
}

/* ------------------------------------------------------------------ glob -- */
static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static bool gm(const char *p, const char *s) {
  while (*p) {
    if (p[0] == '*' && p[1] == '*') {
      p += 2;
      if (*p == '/') p++;
      for (const char *t = s;; t++) {
        if (gm(p, t)) return true;
        if (!*t) return false;
      }
    }
    if (*p == '*') {
      p++;
      for (const char *t = s;; t++) {
        if (gm(p, t)) return true;
        if (!*t || *t == '/') return false;
      }
    }
    if (*p == '?') {
      if (!*s || *s == '/') return false;
      p++; s++;
      continue;
    }
    if (lower(*p) != lower(*s)) return false;
    p++; s++;
  }
  return *s == 0;
}

bool e2a_glob_match(const char *pattern, const char *rel) {
  if (pattern[0] == '/') pattern++;
  if (strncasecmp(pattern, "app0/", 5) == 0) pattern += 5;
  if (gm(pattern, rel)) return true;
  if (!strchr(pattern, '/')) return gm(pattern, base_name(rel));
  return false;
}

/* ------------------------------------------------------------------ list -- */
int e2a_list_add(e2a_list *l, const char *rel, uint64_t size, int64_t mtime, bool synthetic) {
  if (l->n == l->cap) {
    size_t nc = l->cap ? l->cap * 2 : 1024;
    e2a_entry *nv = realloc(l->v, nc * sizeof *nv);
    if (!nv) return -1;
    l->v = nv; l->cap = nc;
  }
  e2a_entry *e = &l->v[l->n];
  memset(e, 0, sizeof *e);
  e->rel = strdup(rel);
  size_t klen = 6 + strlen(rel);
  e->key = malloc(klen + 1);
  if (!e->rel || !e->key) return -1;
  memcpy(e->key, "/app0/", 6);
  memcpy(e->key + 6, rel, strlen(rel) + 1);
  e2a_fold_ascii(e->key);
  e->size = size; e->mtime = mtime; e->synthetic = synthetic;
  e->action = (uint8_t)(size == 0 ? E2A_LOOSE : e2a_classify(rel));
  l->n++;
  l->total_bytes += size;
  return 0;
}

static int walk(const char *root, const char *rel, e2a_list *out, bool clean) {
  char full[1024];
  if (rel[0]) snprintf(full, sizeof full, "%s/%s", root, rel); else snprintf(full, sizeof full, "%s", root);
  DIR *d = opendir(full);
  if (!d) { e2a_log("opendir failed: %s", full); return -1; }
  struct dirent *de;
  int rc = 0;
  while ((de = readdir(d)) != NULL) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
    if (strchr(de->d_name, '\t') || strchr(de->d_name, '\n') || strchr(de->d_name, '\r')) continue;
    char child[1024], path[1100];
    if (rel[0]) snprintf(child, sizeof child, "%s/%s", rel, de->d_name); else snprintf(child, sizeof child, "%s", de->d_name);
    snprintf(path, sizeof path, "%s/%s", root, child);
    struct stat st;
    if (stat(path, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      if (strcmp(de->d_name, ".e2a-state") == 0) continue;
      if (!rel[0] && strcasecmp(de->d_name, "fakelib2") == 0) {
        char marker[1200];
        snprintf(marker, sizeof marker, "%s/.e2a-trace", path);
        if (e2a_is_file(marker)) { e2a_log("skip trace-mode fakelib2 (debug emulator)"); continue; }
      }
      if (clean && e2a_is_leftover(child)) { e2a_log("skip leftover: %s", child); continue; }
      if (walk(root, child, out, clean) != 0) rc = -1;
    } else if (S_ISREG(st.st_mode)) {
      const char *name = de->d_name;
      if (strcasecmp(name, "ampr_commands.bin") == 0 || strcasecmp(name, "apr_emu.log") == 0) continue;
      if (strcasecmp(child, "ampr_emu.index") == 0 || strcasecmp(child, "ampr_assets.index") == 0) continue;
      if (strncasecmp(child, "ampr_assets.index.", 18) == 0) continue;
      if (strncasecmp(child, "ampr_assets-", 12) == 0 && ends_with_ci(child, ".pak")) continue;
      if (ends_with_ci(name, ".e2a-tmp") || strcasecmp(name, "exfat2ampr.toml") == 0) continue;
      if (e2a_list_add(out, child, (uint64_t)st.st_size, (int64_t)st.st_mtime, false) != 0) { rc = -1; break; }
    }
  }
  closedir(d);
  return rc;
}

int e2a_scan_tree(const char *root, e2a_list *out, bool clean_leftovers) {
  memset(out, 0, sizeof *out);
  return walk(root, "", out, clean_leftovers);
}

static int cmp_key(const void *a, const void *b) {
  return strcmp(((const e2a_entry *)a)->key, ((const e2a_entry *)b)->key);
}

void e2a_list_sort(e2a_list *l) { qsort(l->v, l->n, sizeof *l->v, cmp_key); }

void e2a_list_free(e2a_list *l) {
  for (size_t i = 0; i < l->n; i++) { free(l->v[i].rel); free(l->v[i].key); }
  free(l->v);
  memset(l, 0, sizeof *l);
}

/* ------------------------------------------------------------ param.json -- */
static int json_str(const char *doc, const char *key, char *out, size_t cap) {
  char pat[64];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char *p = strstr(doc, pat);
  if (!p) return -1;
  p = strchr(p + strlen(pat), ':');
  if (!p) return -1;
  p++;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  if (*p != '"') return -1;
  p++;
  size_t o = 0;
  while (*p && *p != '"' && o + 1 < cap) {
    if (*p == '\\' && p[1]) {
      p++;
      if (*p == 'u') {  /* skip \uXXXX without running past the end of a truncated document */
        p++;
        for (int k = 0; k < 4 && *p; k++) p++;
        out[o++] = '?';
        continue;
      }
    }
    out[o++] = *p++;
  }
  out[o] = 0;
  return 0;
}

/* ---------------------------------------------------------- compatibility -- */
/* 1 = needle found, 0 = not found (whole file read), -1 = unreadable; *plain = executable strings seen */
static int file_has(const char *path, const char *needle, const char *witness, bool *plain) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  const size_t BUF = 4u << 20, KEEP = 64;
  uint8_t *buf = malloc(BUF + KEEP);
  if (!buf) { close(fd); return -1; }
  size_t nl = strlen(needle), wl = strlen(witness), have = 0;
  int found = 0;
  for (;;) {
    ssize_t r = read(fd, buf + have, BUF);
    if (r < 0) { found = -1; break; }
    if (r == 0) break;
    size_t n = have + (size_t)r;
    for (size_t i = 0; i + nl <= n && !found; i++)
      if (buf[i] == (uint8_t)needle[0] && !memcmp(buf + i, needle, nl)) found = 1;
    for (size_t i = 0; i + wl <= n && !*plain; i++)
      if (buf[i] == (uint8_t)witness[0] && !memcmp(buf + i, witness, wl)) *plain = true;
    if (found) break;
    have = n < KEEP ? n : KEEP;   /* keep a tail: a name may straddle two reads */
    memmove(buf, buf + n - have, have);
  }
  free(buf);
  close(fd);
  return found;
}

static int imports(const char *root, const char *lib, char *detail, size_t cap) {
  char path[1100];
  bool plain = false;   /* "libkernel" is imported by every executable: seen = strings are readable */
  snprintf(path, sizeof path, "%s/eboot.bin", root);
  int r = file_has(path, lib, "libkernel", &plain);
  if (r == 1) { snprintf(detail, cap, "eboot.bin imports %s", lib); return 1; }
  snprintf(path, sizeof path, "%s/sce_module", root);
  DIR *d = opendir(path);
  if (d) {
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
      if (!ends_with_ci(de->d_name, ".prx") && !ends_with_ci(de->d_name, ".sprx")) continue;
      snprintf(path, sizeof path, "%s/sce_module/%s", root, de->d_name);
      if (file_has(path, lib, "libkernel", &plain) == 1) {
        snprintf(detail, cap, "sce_module/%s imports %s", de->d_name, lib);
        closedir(d);
        return 1;
      }
    }
    closedir(d);
  }
  if (!plain) { snprintf(detail, cap, "eboot.bin is encrypted or unreadable: cannot tell"); return -1; }
  snprintf(detail, cap, "no %s import in eboot.bin or sce_module", lib);
  return 0;
}

int e2a_ampr_compat(const char *root, char *detail, size_t cap) { return imports(root, "libSceAmpr", detail, cap); }

int e2a_game_imports(const char *root, const char *lib) {
  char detail[200];
  return imports(root, lib, detail, sizeof detail);
}

bool e2a_title_from_name(const char *name, char out[16]) {
  for (const char *p = name; p && *p; p++) {
    int k = 0;
    while (k < 4 && p[k] >= 'A' && p[k] <= 'Z') k++;
    if (k < 4) continue;
    int d = 0;
    while (d < 5 && p[4 + d] >= '0' && p[4 + d] <= '9') d++;
    if (d == 5 && !(p[9] >= '0' && p[9] <= '9')) {
      memcpy(out, p, 9);
      out[9] = 0;
      return true;
    }
  }
  out[0] = 0;
  return false;
}

int e2a_read_game(const char *root, e2a_game *g) {
  char path[1100];
  memset(g, 0, sizeof *g);
  snprintf(path, sizeof path, "%s/sce_sys/param.json", root);
  char *doc = NULL;
  if (e2a_read_text(path, &doc, NULL, 1 << 20) != 0) return -1;
  json_str(doc, "titleId", g->title_id, sizeof g->title_id);
  json_str(doc, "contentVersion", g->version, sizeof g->version);
  if (!g->version[0]) json_str(doc, "masterVersion", g->version, sizeof g->version);
  if (json_str(doc, "titleName", g->title, sizeof g->title) != 0) json_str(doc, "title", g->title, sizeof g->title);
  free(doc);
  /* IDs and versions end up in JSON and in folder names: keep only safe characters */
  char *fields[] = {g->title_id, g->version};
  for (int f = 0; f < 2; f++) {
    char *w = fields[f];
    for (const char *r = fields[f]; *r; r++)
      if ((*r >= 'A' && *r <= 'Z') || (*r >= 'a' && *r <= 'z') || (*r >= '0' && *r <= '9') || *r == '.' || *r == '-' || *r == '_')
        *w++ = *r;
    *w = 0;
  }
  snprintf(path, sizeof path, "%s/eboot.bin", root);
  if (!g->title_id[0] || !e2a_is_file(path)) return -1;
  return 0;
}
