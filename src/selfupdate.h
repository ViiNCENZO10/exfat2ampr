/* SPDX-License-Identifier: GPL-3.0-or-later
 * The app's own update, installed by the engine. The home-screen app downloads the new PPSA99272.ffpkg
 * over HTTPS (the console's sceHttp: the engine has no TLS) and hands it over 1 MiB at a time, from the
 * console only (/api/update/...). The engine checks its size and SHA-256 against the published manifest,
 * keeps the running package aside (.old, .old2, ...: nothing is deleted) and puts the new one where
 * ShadowMountPlus finds the app; a restart of the console runs it. */
#ifndef E2A_SELFUPDATE_H
#define E2A_SELFUPDATE_H

#include "common.h"

/* a new upload: its size and lowercase hex SHA-256 */
int selfupdate_begin(uint64_t size, const char *sha256_hex, char *err, size_t errcap);
/* the next piece, at offset off (pieces come in order) */
int selfupdate_chunk(uint64_t off, const void *data, size_t len, char *err, size_t errcap);
/* checks the whole file, then swaps it in; where = the package's path */
int selfupdate_install(char *where, size_t wcap, char *err, size_t errcap);

#endif
