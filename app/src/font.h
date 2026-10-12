/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * Anti-aliased bitmap fonts generated at build time (app/tools/make_font.py). */
#ifndef E2A_APP_FONT_H
#define E2A_APP_FONT_H

#include <stdint.h>

typedef struct {
  uint32_t cp;      /* Unicode code point (table sorted) */
  uint32_t off;     /* first byte in the 8-bit coverage table */
  uint8_t w, h;     /* bitmap size */
  int8_t x, y;      /* bitmap offset from the pen (y from the top of the line) */
  uint8_t adv;      /* horizontal advance */
} font_glyph;

typedef struct {
  int size, ascent, line;
  int count;
  const font_glyph *glyphs;
  const uint8_t *px;
  int nibbles;      /* 1: 4-bit coverage, two pixels a byte (the tables loaded at start) */
} font_t;

extern const font_t FONT_SMALL, FONT_BODY, FONT_HEAD, FONT_TITLE;
extern const font_t FONT_HUGE;   /* digits and a few signs only: the progress percentage */

#endif
