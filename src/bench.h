/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * Speed of a destination storage, measured on demand (the app: Destinations > square; the page):
 *  - read: random 256 KiB reads spread over the largest file kept there (a game image or pack of at least
 *    256 MiB), which is what a game does while it streams; the reads land on cold data, the cache does
 *    not flatter the result. Without such a file the read speed stays unknown (-1);
 *  - write: up to 96 MiB in 1 MiB writes (2.5 s at most), synced, into <base>/.e2a-speedtest.tmp, then
 *    deleted.
 * Results are kept per destination (memory and E2A_STATE_DIR/speed.txt) and listed with the destinations. */
#ifndef E2A_BENCH_H
#define E2A_BENCH_H

#include "common.h"

typedef struct {
  int read_mbps, write_mbps;   /* MB/s (10^6 bytes), -1 = unknown */
  int64_t at;                  /* unix seconds of the measure */
} e2a_speed;

/* measure base (a destination's <mount>/homebrew); 0 = done, out filled; -1 = err explains */
int bench_run(const char *base, e2a_speed *out, char *err, size_t errcap);
/* the last measure of base; false when it was never measured */
bool bench_get(const char *base, e2a_speed *out);

#endif
