/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * A game's home-screen music (sce_sys/snd0.at9: RIFF/WAVE holding ATRAC9 superframes) decoded to PCM with
 * LibAtrac9 (Alex Barney, MIT; third_party/atrac9). */
#ifndef E2A_ATRAC9_H
#define E2A_ATRAC9_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  int16_t *pcm;           /* interleaved, malloc'd; the caller frees it */
  int frames;             /* per channel */
  int channels, rate;
  int loop_start, loop_end;   /* frames; loop_end 0: no loop point in the file (loop the whole track) */
} at9_track;

/* 0 on success; -1 when the file is not an ATRAC9 WAVE or cannot be decoded (the track is left empty) */
int at9_decode(const uint8_t *file, size_t len, at9_track *out);

#endif
