/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * Client for the ShadowMountPlus local HTTP API (127.0.0.1:10101 by default).
 * Used to mount an image by title ID for the duration of a conversion. */
#ifndef E2A_SMP_H
#define E2A_SMP_H

#include "common.h"

typedef struct {
  char title_id[16];
  char title[128];
  char version[32];
  char path[600];          /* outer image file (or folder) */
  char runtime_path[600];  /* game root while mounted */
  char source_type[16];    /* image | folder | pkg */
  char image_type[16];     /* ufs | exfatfs | pfs | pfsc */
  char fakelib_mode[16];   /* resolved policy (1.7beta5+): full | emulators | disabled, "" before */
  bool mounted, managed, source_available, fakelib_enabled;
} smp_game;

/* mount outcome; on SMP_BUSY, reason holds ShadowMountPlus' stable error_reason code
 * (game_active, runtime_prepared, rest_mode, scanner_busy, ...; "" from older versions) */
typedef enum { SMP_OK = 0, SMP_BUSY, SMP_UNREACHABLE, SMP_NOT_MANAGED, SMP_NO_PATH, SMP_FAILED } smp_result;

/* true when the API answers /api/v1/version */
bool smp_available(char *version, size_t cap);

/* list SMP-managed games (image and folder sources); returns count or -1 */
int smp_list_games(smp_game *out, int max);

/* mount read-only and return the game root; reason and errmsg (English) filled on failure */
smp_result smp_mount(const char *title_id, char *runtime_path, size_t cap, char *reason, size_t rcap, char *errmsg, size_t errcap);
/* the image already mounted by ShadowMountPlus (persistent_image_mounts=1): its mount point, no mount request */
bool smp_image_mount_point(const char *image_path, char *mount_point, size_t cap);
/* the same from a list kept 15 s (pictures: asked for every game on screen) */
bool smp_image_mount_point_cached(const char *image_path, char *mount_point, size_t cap);
/* per-title fakelib policy (full | emulators | disabled | default); 0 when saved */
int smp_set_fakelib(const char *title_id, const char *mode);
int smp_unmount(const char *title_id, char *errmsg, size_t errcap);
int smp_game_info(const char *title_id, smp_game *g);

/* GET /api/v1/games/icon?title_id=...: PNG bytes (caller frees) */
int smp_icon(const char *title_id, uint8_t **png, size_t *len);
int smp_icon_wait(const char *title_id, uint8_t **png, size_t *len, int timeout_s);   /* the background pass: patient */

#endif
