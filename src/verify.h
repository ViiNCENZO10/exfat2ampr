/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * Reads a finished game folder back: manifest, volumes and loose files are checked
 * against the AMPRCRC1 sidecar (every block is decoded and CRC-compared). */
#ifndef E2A_VERIFY_H
#define E2A_VERIFY_H

#include "ampr_pack.h"

typedef struct {
  uint64_t files_packed, files_loose, chunks, bytes;
  uint64_t errors;
  char first_error[256];
} e2a_verify_stats;

/* Returns E2A_PACK_OK (no error), E2A_PACK_ERROR (errors found / unreadable) or E2A_PACK_PAUSED. */
int e2a_verify(const char *out_dir, const e2a_pack_opts *o, e2a_verify_stats *vs);

/* Sets entries[i].action from the file records of <out_dir>/ampr_assets.index
 * (used when a run resumes after the pack phase already completed). */
int e2a_manifest_actions(const char *out_dir, e2a_list *l);

#endif
