/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE. */
#ifndef E2A_APP_IMG_H
#define E2A_APP_IMG_H

#include "gfx.h"

/* whole picture, at most max_side pixels wide or high, with its half-size levels */
int img_decode_png(const uint8_t *data, size_t len, int max_side, image_t *out);
/* exactly tw x th: the centred part with those proportions, scaled (no levels) */
int img_decode_fill(const uint8_t *data, size_t len, int tw, int th, image_t *out);
/* a part of src (with its half-size levels) */
int img_crop(const image_t *src, int x, int y, int w, int h, image_t *out);
void img_free(image_t *img);
/* zlib data inflated (malloc'd), expect = its size when known; NULL on error */
void *img_inflate(const uint8_t *z, size_t len, size_t expect, size_t *out);

#endif
