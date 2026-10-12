/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE. */
#pragma once
/* The app's own log. Every line is kept for the Journal screen (Settings) and shipped to the engine
 * (POST /api/applog) by a thread of its own, so the developer reads it from a PC or the web page, and it
 * survives the app closing or crashing: the engine keeps the lines and writes them to its state folder. */
#include <stddef.h>

#define APP_LOG_LEN 200

void app_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void app_log_start(void);                                       /* starts the shipping thread (once) */
/* the newest lines, oldest first, skipping `skip_newest` of them (scrolling); returns how many were copied */
int app_log_copy(char (*out)[APP_LOG_LEN], int max, int skip_newest);
int app_log_count(void);
unsigned app_log_rev(void);                                     /* changes with every line: the screen redraws */
const char *app_log_host(const char *url, char *out, size_t cap);   /* "https://host/path" -> "host" */
