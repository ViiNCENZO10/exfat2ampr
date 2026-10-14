/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE. */
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
  atomic_int *cancel;     /* abort, the caller discards the destination */
  atomic_int *pause;      /* checkpoint and return E2A_PACK_PAUSED */
  /* bytes processed so far in the current phase + the file being handled */
  void (*progress)(void *ctx, uint64_t done_bytes, const char *msg);
  void (*skip)(void *ctx, uint64_t bytes);   /* counted done without being read now (the speed ignores them) */
  void *ctx;
  /* 1024 bits, one per volume, owned by the caller: the pack sets the bit of every volume it read back and
   * checked while the next one filled; the final verification then skips those (NULL: not used) */
  uint64_t *verified_bits;
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

/* what each stage of the packer sustains right now (a few seconds' window), zeros when nothing packs:
 * shown by the app and the web page so a slow source is told apart from a slow destination */
typedef struct {
  double read_mbps, compress_mbps, write_mbps;
  int read_threads, workers;
  uint64_t in_bytes, out_bytes, autoloose_bytes;   /* packed so far, their size in the volumes, files left loose */
} e2a_pack_rates;
void e2a_pack_rates_get(e2a_pack_rates *out);

/* true when state_dir holds a checkpoint that e2a_pack can resume from */
bool e2a_pack_has_checkpoint(const char *state_dir);
void e2a_pack_clear_checkpoint(const char *state_dir);

/* AMPRIDX3 for the entry list (sorted). */
int e2a_write_index(const char *path, const e2a_list *l);

/* A folder converted by an earlier version: rebuilds ampr_emu.index from the pack manifest when it does not
 * describe it (old slot hash, or replaced by the emulator) and gives the loose executables and the fakelib
 * modules an execute bit. 0 = nothing to do or not a converted folder, >0 = fixes made, -1 = error (report). */
int e2a_repair_converted(const char *game_dir, char *report, size_t cap);

/* Private integrity sidecar; the emulator does not consume it. Records follow fileId order. */
typedef struct __attribute__((packed)) {
  char magic[8];
  uint8_t build_id[16];
  uint64_t count;
  uint32_t payload_crc, reserved;
} e2a_loose_header;
typedef struct __attribute__((packed)) { uint64_t path_hash; char sha[64]; } e2a_loose_record;

/* Copies every E2A_LOOSE entry from src_root into out_dir (already copied files are skipped). */
int e2a_copy_loose(const e2a_list *l, const e2a_pack_opts *o);

#endif
