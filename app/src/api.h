/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * Talks to the exfat2ampr engine (the privileged payload) on 127.0.0.1:6972, and starts it
 * through the local ELF loader (port 9021) when it is not running. */
#ifndef E2A_APP_API_H
#define E2A_APP_API_H

#include "json.h"

#include <stdint.h>

#define ENGINE_PORT 6972
#define LOADER_PORT 9021

/* GET (body == NULL) or POST form; fills *doc with the JSON answer. Returns the HTTP status, < 0 if unreachable. */
int api_call(const char *path, const char *form, jdoc *doc);
/* quiet: the engine's silence is not logged (while the app starts it) */
void api_set_quiet(bool quiet);
/* POST of raw bytes (the app's update, to the engine); JSON answer in *doc */
int api_post_bytes(const char *path, const void *data, size_t len, jdoc *doc);
/* raw GET (icons); *data malloc'd */
int api_get_raw(const char *path, uint8_t **data, size_t *len);
/* the same, returning the HTTP status (-1: no answer); *data only on 200 */
int api_get_status(const char *path, uint8_t **data, size_t *len);

/* form builder: appends key=value (URL-encoded) to buf */
void form_add(char *buf, size_t cap, const char *key, const char *value);
void form_add_int(char *buf, size_t cap, const char *key, long value);

typedef enum { ENGINE_OK, ENGINE_STARTED, ENGINE_UPDATED, ENGINE_NO_LOADER, ENGINE_NO_ELF, ENGINE_TIMEOUT, ENGINE_BUSY_OLD } engine_result;
/* make sure the engine answers, starting (or replacing an older idle one) when needed */
engine_result engine_ensure(const char *want_version, char *running_version, size_t cap);

#endif
