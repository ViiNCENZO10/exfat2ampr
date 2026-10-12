/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE. */
#ifndef E2A_APP_QR_H
#define E2A_APP_QR_H

/* draws text as a QR code (white quiet zone included) in at most max_size pixels; returns the side used */
int qr_draw(const char *text, int x, int y, int max_size);

#endif
