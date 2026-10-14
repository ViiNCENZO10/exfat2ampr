/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * Pictures loaded on a background thread, so the interface never waits for them:
 *  - game covers (icon0.png through the engine);
 *  - full-screen wallpapers made from a game's key art (sce_sys/pic0.png) or, without one, from
 *    its blurred cover; darkened so the text stays readable, cross-faded when they change. */
#ifndef E2A_APP_ASSETS_H
#define E2A_APP_ASSETS_H

#include "gfx.h"

#include <stdbool.h>
#include <stdint.h>

void assets_init(void);

/* cover for key (what identifies the game), fetched with GET /api/icon?<query>. NULL until it is
 * loaded (or when there is none); *loaded_at = when it arrived, for a fade-in. */
const image_t *assets_cover(const char *key, const char *query, uint64_t *loaded_at);

/* the wallpaper to show: key "" = the default one. art_query: /api/icon query for the key art
 * ("" = none), cover_query: for the cover. Asking again for the shown key costs nothing. */
void assets_wallpaper(const char *key, const char *art_query, const char *cover_query);
/* the focused game's own music: fetched (/api/sound) and decoded on the loader thread, then handed to the
 * sound module; an empty key fades it out; games without one are remembered and not asked again */
void assets_music(const char *key, const char *query);
void assets_music_reset(void);   /* the next assets_music() asks again (music switched back on) */

/* once per loop, before drawing: takes in what the thread finished; true = something new to show */
bool assets_update(uint64_t now);
/* paints the wallpaper over the whole frame (keeps frames coming while it cross-fades) */
void assets_draw_wallpaper(void);

#endif
