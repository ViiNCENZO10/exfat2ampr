/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * Trace-driven pack profiles, a C port of drakmor/ampr_emu tools/ampr_pack_profile.py:
 *  1. trace mode: the debug AMPR emulator is placed in <game>/fakelib2 (ShadowMountPlus mounts
 *     fakelib2 exclusively, so its emulator updater cannot replace it); the game then writes
 *     /app0/ampr_commands.bin (AMPRCMD1 records) while it is played;
 *  2. collect: each session's journal is copied with the matching ampr_emu.index into
 *     E2A_STATE_DIR/traces/<TITLE_ID>/<n>/ (the emulator truncates the journal at every launch);
 *  3. generate: all collected traces are merged and turned into a TOML profile
 *     (block size, layout, hot flag per file, [runtime] cache and worker settings). */
#ifndef E2A_TRACE_H
#define E2A_TRACE_H

#include "common.h"

int trace_enable(const char *game_dir, char *err, size_t errcap);
int trace_disable(const char *game_dir);
bool trace_enabled(const char *game_dir);
/* copies a new <game>/ampr_commands.bin; returns 1 when copied, 0 when nothing new, -1 on error */
int trace_collect(const char *game_dir, const char *title_id, char *err, size_t errcap);
int trace_count(const char *title_id);
/* merges every collected trace of the title into <E2A_PROFILE_DIR>/<TITLE_ID>.toml */
int trace_generate(const char *title_id, char *summary, size_t cap);
/* JSON array of the game folders under the homebrew folders (loose or converted) with their trace state */
int trace_list_json(char *json, size_t cap);
/* true when path is a direct child of one of the homebrew roots listed by trace_list_json */
bool trace_game_allowed(const char *path);

/* core, exposed for the host build / CI: parse (journal, index) pairs and write the TOML */
int trace_build_profile(const char *const *journals, const char *const *indexes, int pairs, const char *out_toml,
                        char *summary, size_t cap);

#endif
