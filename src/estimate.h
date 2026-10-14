/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE. */
#pragma once
/* Size forecasts. Before a conversion, the size of the result is predicted from the game itself: the files left
 * loose count as they are, the others are sampled (blocks read across each big file and compressed the way
 * the packer does), plus the layout's padding. When the game cannot be read yet (an image not mounted), the
 * sizes learned on a previous run of the same title are used, else the average ratio of past conversions.
 * After each conversion the real sizes are remembered (E2A_STATE_DIR/sizes.txt). */
#include <stdbool.h>
#include <stdint.h>

#include "scan.h"

typedef struct {
  uint64_t content;    /* bytes of the game's files */
  uint64_t loose;      /* of which left as they are */
  uint64_t predicted;  /* the converted game on disk */
  double ratio;        /* predicted / content */
  const char *method;  /* "sampled", "history", "average" or "rough" */
} e2a_estimate;

/* from the game's files (a folder, or an image while it is mounted) */
int est_from_tree(const char *root, int level, e2a_estimate *out);
/* from a scanned and planned list (the job): the same sampling, without walking again */
int est_from_list(const char *root, const e2a_list *l, int level, e2a_estimate *out);
/* nothing readable: what an earlier run of this title taught, else the average of all runs */
void est_from_history(const char *title_id, uint64_t image_bytes, bool compressed_image, e2a_estimate *out);
void est_remember(const char *title_id, uint64_t content, uint64_t image_bytes, uint64_t final_bytes);
bool est_known_content(const char *title_id, uint64_t *content, uint64_t *final_bytes);
/* the last measured verification speed (bytes/s), 0 when never measured */
double est_verify_rate(void);
void est_set_verify_rate(double bps);
/* the packing speed at an LZ4 level (bytes of game per second): measured on this console, else a guess */
double est_pack_rate(int level);
void est_set_pack_rate(int level, double bps);
/* how long a conversion of <content> bytes should take (seconds), verification included or not; -1 unknown */
int64_t est_duration(uint64_t content, int level, bool verify);
