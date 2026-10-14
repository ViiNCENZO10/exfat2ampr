/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE. */
#pragma once
/* The covers' cache: every icon0.png / key art found once (folder, console metadata, mounted image,
 * ShadowMountPlus) is kept in E2A_STATE_DIR/covers/<TITLE>[_bg].png, so it is served at once afterwards even
 * when its source is slow (ShadowMountPlus can take a minute for an image it has not opened yet) or gone (the
 * image was converted and removed). A background pass fetches the missing ones with a patient timeout. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool covers_png_ok(const void *data, size_t len);   /* PNG signature and a sane IHDR (1..8192 px) */
/* the cached picture of a title (malloc'd); false when there is none */
bool covers_get(const char *title_id, bool bg, uint8_t **png, size_t *len);
void covers_put(const char *title_id, bool bg, const void *png, size_t len);   /* checked, written atomically */
void covers_put_from_dir(const char *title_id, const char *game_root);           /* icon0 + key art + snd0 of a game folder */
/* the title's home-screen music (sce_sys/snd0.at9, a RIFF/WAVE holding ATRAC9), kept the same way */
bool covers_sound_ok(const void *data, size_t len);
bool covers_get_sound(const char *title_id, uint8_t **at9, size_t *len);
void covers_put_sound(const char *title_id, const void *at9, size_t len);
void covers_want(const char *title_id);   /* a title whose cover could not be found quickly: the background pass tries */
void covers_start(void);                  /* the background pass: at start, then every 10 minutes and on demand */
int covers_status_json(char *json, size_t cap);
