/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * What happens around the engine, checked every 2 s by a monitor thread:
 *  - games running on the console (process list + sceKernelGetAppInfo, as ShadowMountPlus does):
 *    game mode slows the conversion down so the game keeps its CPU and disk, and a game is never
 *    converted while it is being played (its conversion is paused, then resumed when it closes);
 *  - drives plugged in or removed (storage revision for the app and the page; a conversion whose
 *    drive disappears is paused cleanly, resumable when the drive is back);
 *  - the home-screen app (heartbeat), so it is not mistaken for a game. */
#ifndef E2A_ACTIVITY_H
#define E2A_ACTIVITY_H

#include "common.h"

#define E2A_APP_TITLE_ID "PPSA99272"

void activity_init(void);
void activity_app_seen(int pid);
bool activity_app_alive(void);

/* title ids of the games running now (never the home-screen app); returns count */
int activity_running_titles(char titles[][16], int max);
bool activity_title_running(const char *title_id);

bool activity_game_mode_enabled(void);
void activity_set_game_mode_enabled(bool on);
bool activity_game_mode(void);                 /* enabled and a game is running */
int activity_workers(int requested);           /* compression threads to use right now */
void activity_throttle(uint64_t bytes);        /* paces disk traffic in game mode */

uint64_t activity_storage_rev(void);

#endif
