/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * Per-game profiles: a subset of the ampr_pack TOML (rules with action / include / exclude).
 * Searched in <E2A_PROFILE_DIR>/<TITLE_ID>.toml, <usb>/homebrew/profiles/<TITLE_ID>.toml and
 * <source>/exfat2ampr.toml. Rules are applied after the blind profile; the last match wins. */
#ifndef E2A_PROFILE_H
#define E2A_PROFILE_H

#include "scan.h"

/* action = "keep": the rule only sets block size / layout / hot of files that are packed anyway */
#define E2A_ACTION_KEEP 0xFF

typedef struct {
  uint8_t action;           /* E2A_LOOSE / E2A_COMPRESS / E2A_STORE / E2A_ACTION_KEEP */
  uint8_t block_shift;      /* block_size = "16KiB".."1MiB"; 0 = [pack] default */
  int8_t layout;            /* E2A_LAYOUT_*; -1 = auto (random when hot, else mixed) */
  bool hot;
  char **include; size_t ninc;
  char **exclude; size_t nexc;
} e2a_rule;

typedef struct {
  e2a_rule *rules; size_t n;
  int level;                /* [pack] compression_level, -1 when absent */
  uint8_t default_shift;    /* [pack] default_block_size, 0 = 64 KiB */
  bool default_loose;       /* [pack] default_action = "loose": files no rule matches stay loose */
  /* [runtime]: 0 = not set */
  uint64_t rt_decoded, rt_physical;
  uint32_t rt_workers, rt_reserve;
  bool has_runtime;
  char path[320];
} e2a_profile;

int  e2a_profile_load(const char *path, e2a_profile *p);
uint64_t e2a_parse_size(const char *v);   /* "64KiB", "1MiB", "128MiB", "4096", "1.5GiB" */
int  e2a_profile_find(const char *title_id, const char *source_root, char *out, size_t cap);
void e2a_profile_apply(const e2a_profile *p, e2a_list *l, size_t *changed);
void e2a_profile_free(e2a_profile *p);
int  e2a_profile_save(const char *title_id, const char *toml, size_t len);   /* into E2A_PROFILE_DIR */
int  e2a_profile_import_usb(void);                                           /* copy the .toml files found in <usb>/homebrew/profiles */

#endif
