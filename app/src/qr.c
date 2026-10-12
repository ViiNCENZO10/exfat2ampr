/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * Pairing QR code, drawn with qrcodegen (Project Nayuki, MIT). */
#define NDEBUG   /* the app runtime has no __assert */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#include "third_party/qrcodegen.inc"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "gfx.h"
#include "qr.h"

#include <string.h>

int qr_draw(const char *text, int x, int y, int max_size) {
  static uint8_t qr[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
  static char last[256];
  static bool ok;
  /* encoded again only when the text changes: the screen is redrawn at every animation frame */
  if (strncmp(last, text, sizeof last - 1) != 0) {
    size_t n = strlen(text) < sizeof last - 1 ? strlen(text) : sizeof last - 1;
    memcpy(last, text, n);
    last[n] = 0;
    ok = qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true);
  }
  if (!ok) return 0;
  int n = qrcodegen_getSize(qr), quiet = 4, cell = max_size / (n + 2 * quiet);
  if (cell < 1) return 0;
  int side = cell * (n + 2 * quiet);
  gfx_round_rect(x, y, side, side, cell * 2, RGB(255, 255, 255));
  for (int r = 0; r < n; r++)
    for (int c = 0; c < n; c++)
      if (qrcodegen_getModule(qr, c, r)) gfx_rect(x + (quiet + c) * cell, y + (quiet + r) * cell, cell, cell, RGB(0, 0, 0));
  return side;
}
