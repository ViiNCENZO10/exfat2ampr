#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
# Downloads the app's build inputs (pinned) and generates app/src/font_data.c:
#  - stb_image.h (public domain / MIT) for the game covers
#  - qrcodegen (Project Nayuki, MIT) for the pairing QR code
#  - DejaVu Sans 2.37 (Bitstream Vera / DejaVu license) rasterized by make_font.py, with Noto Sans Thai and
#    Noto Sans CJK (SIL OFL 1.1) from the system (Debian/Ubuntu: fonts-noto-core fonts-noto-cjk); the common CJK
#    characters of game titles go to .deps/glyphs_cjk.bin, packaged with the app (assets/glyphs_cjk.bin)
set -euo pipefail
cd "$(dirname "$0")/../.."
STB_COMMIT=2c980bb59875b0d32144a71867fbdebb2f77cd20
mkdir -p app/src/third_party .deps
curl -fsSL -o app/src/third_party/stb_image.h "https://raw.githubusercontent.com/nothings/stb/$STB_COMMIT/stb_image.h"
# QR codes for the pairing screen (MIT). Stored as .inc: every .c below app/src is compiled on its own,
# this one is included by qr.c with NDEBUG (the app runtime has no __assert)
QR_COMMIT=3c6d0b3cefb4e049dc337e82237c9644399716a8
curl -fsSL -o app/src/third_party/qrcodegen.h "https://raw.githubusercontent.com/nayuki/QR-Code-generator/$QR_COMMIT/c/qrcodegen.h"
curl -fsSL -o app/src/third_party/qrcodegen.inc "https://raw.githubusercontent.com/nayuki/QR-Code-generator/$QR_COMMIT/c/qrcodegen.c"
if [ ! -f .deps/dejavu/ttf/DejaVuSans.ttf ]; then
  curl -fsSL -o .deps/dejavu.zip https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.zip
  rm -rf .deps/dejavu && mkdir -p .deps/dejavu
  unzip -q .deps/dejavu.zip -d .deps/dejavu
  mv .deps/dejavu/dejavu-fonts-ttf-2.37/* .deps/dejavu/
fi
"${PYTHON:-python3}" app/tools/make_font.py .deps/dejavu/ttf/DejaVuSans.ttf .deps/dejavu/ttf/DejaVuSans-Bold.ttf \
  app/src/font_data.c .deps/glyphs_cjk.bin
