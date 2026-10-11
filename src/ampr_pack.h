/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef E2A_AMPR_PACK_H
#define E2A_AMPR_PACK_H

#include "common.h"
#include "scan.h"
#include "ampr_format.h"

enum { E2A_PACK_OK = 0, E2A_PACK_ERROR = -1, E2A_PACK_PAUSED = 2 };

typedef struct {
  const char *src_root;     /* game root (read only) */
  const char *out_dir;      /* destination game folder */
  const char *state_dir;    /* checkpoint folder (<out_dir>/.e2a-state) */
  bool resume;              /* continue from the checkpoint in state_dir */
  bool dedup;               /* share identical blocks (lane scope) */
  int level;                /* LZ4: 1-4 fast, 5-12 HC */
  int workers;              /* compression threads */
  uint64_t max_pack_size;   /* volume rollover */
  uint64_t test_pause_after;/* test hook: request a pause once this many bytes were packed (0 = off) */
  /* emulator runtime settings written to ampr_assets.index.runtime (0 = defaults 128/32 MiB, 4/1) */
  uint64_t rt_decoded, rt_physical;
  uint32_t rt_workers, rt_reserve;
  volatile int *cancel;     /* abort, the caller discards the destination */
  volatile int *pause;      /* checkpoint and return E2A_PACK_PAUSED */
  /* bytes processed so far in the current phase + the file being handled */
  void (*progress)(void *ctx, uint64_t done_bytes, const char *msg);
  void *ctx;
} e2a_pack_opts;

typedef struct {
  uint64_t files_packed, files_loose, chunks, chunks_lz4, chunks_raw, chunks_shared;
  uint64_t logical_bytes, stored_bytes, shared_bytes, packs;
} e2a_pack_stats;

/* Writes ampr_assets.index (+ .crc, .runtime) and ampr_assets-NNN.pak into out_dir.
 * Entries flagged E2A_COMPRESS / E2A_STORE are packed, the others are left for e2a_copy_loose().
 * On E2A_PACK_PAUSED a checkpoint was saved in state_dir (the caller keeps out_dir). */
int e2a_pack(e2a_list *entries, const e2a_pack_opts *o, e2a_pack_stats *st);

/* Message explaining the last pause that the packer triggered itself (e.g. disk full). */
const char *e2a_pack_pause_reason(void);
/* reason shown for a pause requested from outside (game launched, drive removed); "" clears it */
void e2a_pack_set_pause_reason(const char *why);

/* true when state_dir holds a checkpoint that e2a_pack can resume from */
bool e2a_pack_has_checkpoint(const char *state_dir);
void e2a_pack_clear_checkpoint(const char *state_dir);

/* AMPRIDX3 for the entry list (sorted). */
int e2a_write_index(const char *path, const e2a_list *l);

/* Copies every E2A_LOOSE entry from src_root into out_dir (already copied files are skipped). */
int e2a_copy_loose(const e2a_list *l, const e2a_pack_opts *o);

#endif
