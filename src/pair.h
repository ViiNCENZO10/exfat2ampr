/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * Device pairing for the web page: the console shows a 6-digit code (home-screen app, start
 * notification); a PC or phone that types it receives a random token, kept in
 * E2A_STATE_DIR/paired.txt, and sends it with every request. The app itself talks over
 * 127.0.0.1 and never needs one. */
#ifndef E2A_PAIR_H
#define E2A_PAIR_H

#include "common.h"

void pair_init(void);
const char *pair_pin(void);                 /* current code */
bool pair_token_ok(const char *token);
/* 0 = paired (token written, 33 bytes), -1 = wrong code, -2 = too many attempts (wait) */
int pair_register(const char *pin, char token[65]);
void pair_reset(void);                      /* forget every device, new code */
int pair_devices(void);
void pair_local_ip(char *out, size_t cap);

#endif
