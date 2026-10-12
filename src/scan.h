/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE. */
#ifndef E2A_SCAN_H
#define E2A_SCAN_H

#include "common.h"

/* per-file action: loose = copied as is, compress = LZ4 blocks, store = RAW blocks (seekable, no LZ4) */
enum { E2A_LOOSE = 0, E2A_COMPRESS = 1, E2A_STORE = 2 };
/* access pattern of a packed file (ampr_pack layout): mixed is the safe default */
enum { E2A_LAYOUT_MIXED = 0, E2A_LAYOUT_RANDOM = 1, E2A_LAYOUT_STREAMING = 2 };

typedef struct {
  char *rel;        /* path relative to the game root, forward slashes, no leading slash */
  char *key;        /* "/app0/" + rel, ASCII-folded (sort + hash key) */
  uint64_t size;
  int64_t mtime;    /* unix seconds */
  uint8_t action;   /* E2A_LOOSE / E2A_COMPRESS / E2A_STORE */
  uint8_t block_shift; /* log2(block size), 0 = default 64 KiB */
  uint8_t layout;   /* E2A_LAYOUT_* */
  bool hot;         /* read early / often: random layout, never auto-loosed */
  bool synthetic;   /* not present in the source tree (e.g. fakelib/libSceAmpr.sprx) */
} e2a_entry;

typedef struct {
  e2a_entry *v;
  size_t n, cap;
  uint64_t total_bytes;
} e2a_list;

int  e2a_scan_tree(const char *root, e2a_list *out, bool clean_leftovers);
int  e2a_list_add(e2a_list *l, const char *rel, uint64_t size, int64_t mtime, bool synthetic);
void e2a_list_sort(e2a_list *l);          /* by folded key, as build_ampr_index.py */
void e2a_list_free(e2a_list *l);
int  e2a_classify(const char *rel);       /* blind profile: compress or loose */
bool e2a_is_leftover(const char *rel);    /* _DUPLEX_, Unreal Engine/Saved, <project>/Saved */
void e2a_blind_tune(e2a_list *l);         /* default geometry without a trace profile */
/* Big games: over `budget` estimated blocks, the biggest packed files that nothing marked hot or random get
 * bigger blocks (128 KiB, 256 KiB, then 512 KiB) so the emulator's manifest and caches stay within its memory.
 * Returns the estimate after the change; note tells what was changed (empty when nothing was). */
uint64_t e2a_plan_budget(e2a_list *l, uint64_t budget, char *note, size_t cap);

/* glob with *, ?, ** (crosses '/'), case-insensitive; a pattern without '/' also matches the basename */
bool e2a_glob_match(const char *pattern, const char *rel);

/* PS5 game metadata (sce_sys/param.json) */
typedef struct { char title_id[16]; char title[128]; char version[32]; } e2a_game;
int e2a_read_game(const char *root, e2a_game *g);
/* a title ID (PPSA12345, CUSA12345...) written in a file or folder name; false when there is none */
bool e2a_title_from_name(const char *name, char out[16]);

/* Does the game use AMPR? The AMPR emulator only serves reads made through libSceAmpr: a game that
 * reads its files any other way cannot load packed files. Looks for the libSceAmpr import in
 * eboot.bin and the .prx / .sprx modules of sce_module. 1 = uses AMPR, 0 = does not, -1 = cannot tell (encrypted). */
int e2a_ampr_compat(const char *root, char *detail, size_t cap);
/* the same test for any system library (e.g. "libScePlayGo"): 1 = imported, 0 = not, -1 = cannot tell */
int e2a_game_imports(const char *root, const char *lib);

#endif
