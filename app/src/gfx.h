/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * CPU drawing into a linear 1920x1080 frame (0xAABBGGRR), shown by plat_present(). */
#ifndef E2A_APP_GFX_H
#define E2A_APP_GFX_H

#include "font.h"
#include "platform.h"

#define RGBA(r, g, b, a) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(g) << 8 | (uint32_t)(r))
#define RGB(r, g, b) RGBA(r, g, b, 255)
#define WITH_ALPHA(c, a) (((c) & 0xffffffu) | (uint32_t)(a) << 24)

/* same pixel format as the frame; half = the same picture at half size (mipmaps for small draws) */
typedef struct image_t { int w, h; uint32_t *px; struct image_t *half; } image_t;

int gfx_init(void);
uint32_t *gfx_frame(void);
void gfx_present(void);

/* Drawing state, nested: gfx_push moves everything drawn next by (dx, dy) and multiplies its
 * opacity (0..255); gfx_clip restricts it to a rectangle; gfx_pop restores the previous state. */
void gfx_push(int dx, int dy, int opacity);
void gfx_clip(int x, int y, int w, int h);
void gfx_pop(void);

void gfx_clear(uint32_t c);
void gfx_gradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void gfx_rect(int x, int y, int w, int h, uint32_t c);                 /* alpha-blended */
void gfx_round_rect(int x, int y, int w, int h, int r, uint32_t c);
void gfx_round_frame(int x, int y, int w, int h, int r, int t, uint32_t c);
void gfx_round_gradient(int x, int y, int w, int h, int r, uint32_t top, uint32_t bottom);
void gfx_glow(int x, int y, int w, int h, int r, uint32_t c, int size);   /* soft halo around a rounded box */
void gfx_circle(int cx, int cy, int r, uint32_t c);
void gfx_line(int x0, int y0, int x1, int y1, int t, uint32_t c);   /* round caps */
void gfx_image(const image_t *img, int x, int y, int w, int h, int radius);   /* scaled, rounded corners */
void gfx_image_a(const image_t *img, int x, int y, int w, int h, int radius, int alpha);
void gfx_spinner(int cx, int cy, int r, int dot, float phase, uint32_t c);   /* 12 dots, phase 0..1 */

/* full-screen backgrounds (SCREEN_W x SCREEN_H): copy, or cross-fade a -> b with t = 0..256 */
void gfx_blit_full(const uint32_t *src);
void gfx_blend_full(const uint32_t *a, const uint32_t *b, unsigned t);

/* UTF-8 text; returns the advance in pixels. max_w > 0 clips with an ellipsis. */
int gfx_text(const font_t *f, int x, int y, const char *s, uint32_t c, int max_w);
int gfx_text_width(const font_t *f, const char *s);
/* wrapped text (at spaces, between CJK characters, inside Thai phrases); the last line ends with an
 * ellipsis when text is left over. Returns the height used. */
int gfx_text_wrap(const font_t *f, int x, int y, const char *s, uint32_t c, int max_w, int max_lines);
int gfx_text_right(const font_t *f, int right, int y, const char *s, uint32_t c);
int gfx_text_center(const font_t *f, int cx, int y, const char *s, uint32_t c);
/* the extra glyph tables (CJK game titles, app/tools/make_font.py); once, from any thread. The built-in
 * tables answer until they are there. gfx_extra_arrived: true once afterwards (redraw). */
int gfx_load_extra(const char *path);
bool gfx_extra_arrived(void);

/* PlayStation face buttons and D-pad drawn as icons */
enum { ICON_CROSS, ICON_CIRCLE, ICON_SQUARE, ICON_TRIANGLE, ICON_OPTIONS, ICON_L1, ICON_R1, ICON_DPAD };
int gfx_button(int icon, int x, int y, int size);   /* returns the width used */

#endif
