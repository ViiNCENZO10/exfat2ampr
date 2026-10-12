/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * Game plugins: drakmor's replacement modules, kept up to date in the games' fakelib (ShadowMountPlus mounts a
 * game's fakelib over the system's common/lib when the game starts):
 *   libScePlayGo.sprx                                   pgo_stub  every PlayGo chunk reported installed
 *   libSceAppContent.sprx, libSceGameUpdate.sprx,
 *   libSceNpEntitlementAccess.sprx                      dlc_emu   the DLC listed in the game's dlc_emu.ini
 *   libSceAmpr.sprx                                     ampr_emu  the AMPR emulator (job.c installs it)
 * The releases pinned by the Makefile are embedded; the SHA-256 of every known release tells, for each file of a
 * game, whether it is current, an older release (replaced, the old copy kept as .old), or a build we do not know
 * (left alone, reported). Done at every conversion, at start-up for the games of the known destinations, and on
 * request (/api/plugins/refresh). ShadowMountPlus' own emulator folder (update_emulators=1 replaces matching
 * files of every folder/image game at launch) gets the same files, so games we did not convert follow too. */
#ifndef E2A_PLUGINS_H
#define E2A_PLUGINS_H

#include "common.h"

/* the state of one game's plugins, summed up for a listing: "ok" (every needed file current), "update" (an older
 * release present: a refresh replaces it), "missing" (a needed file absent: a refresh adds it), "unknown" (a build
 * we do not know, left alone), "none" (this game needs none of them) */
void plugins_state(const char *game_dir, char out[16]);
/* a real game folder under one of the roots the engine scans (what /api/plugins may touch) */
bool plugins_path_allowed(const char *path);
/* the same in detail: {"state":"...","files":[{"name":..,"state":..,"version":..,"needed":..}]} */
int plugins_status_json(const char *game_dir, char *json, size_t cap);

/* add what the game needs and replace the older releases in <game_dir>/fakelib; returns the number of files
 * written (0 = nothing to do), -1 on error (report explains, in English, for the log) */
int plugins_apply(const char *game_dir, char *report, size_t cap);
/* at a conversion: the game is read where it is (game_src), the files go to <dest>/fakelib; written gets bit i
 * set for every plugins_name(i) written, so the conversion lists them with their real size */
#define PLUGINS_N 4
int plugins_apply_to(const char *game_src, const char *dest, unsigned *written, char *report, size_t cap);
const char *plugins_name(int i);
/* every game folder of the known destinations that is not running; returns the number of games touched */
int plugins_refresh_all(char *report, size_t cap);
/* ShadowMountPlus' emulator folder: the embedded files added when absent, older releases replaced */
int plugins_sync_emus(char *report, size_t cap);
/* {"embedded":[{name,project,version}],"emus":{...},"last_refresh":{...}} for /api/plugins */
int plugins_info_json(char *json, size_t cap);
/* the start-up pass (emus, then every game) on its own thread */
void plugins_start_background(void);

#endif
