/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef E2A_APP_QR_H
#define E2A_APP_QR_H

/* draws text as a QR code (white quiet zone included) in at most max_size pixels; returns the side used */
int qr_draw(const char *text, int x, int y, int max_size);

#endif
