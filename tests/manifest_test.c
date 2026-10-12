/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * The manifest checks: empty and one-entry layouts, truncation, extreme counts and offsets, strings out of
 * bounds or cut short (run with ASan/UBSan on the CI). */
#include "manifest_validate.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
  ampr_pak_header h;
  memset(&h, 0, sizeof h);
  h.endian = AMPR_ENDIAN_MARK;
  h.file_record_size = 48;
  h.chunk_record_size = 12;
  h.pack_record_size = 32;
  h.files_offset = h.chunks_offset = h.packs_offset = h.strings_offset = 128;
  assert(e2a_manifest_layout_valid(&h, 128));
  assert(!e2a_manifest_layout_valid(&h, 127));
  h.file_count = UINT64_MAX;
  assert(!e2a_manifest_layout_valid(&h, 128));
  h.file_count = 1;
  h.chunks_offset = h.packs_offset = h.strings_offset = 176;
  assert(e2a_manifest_layout_valid(&h, 176));
  h.chunks_offset = UINT64_MAX;
  assert(!e2a_manifest_layout_valid(&h, 176));
  assert(e2a_manifest_string_valid("abc", 4, 0, 3));
  assert(!e2a_manifest_string_valid("abc", 4, 0, 4));
  assert(!e2a_manifest_string_valid("abc", 4, UINT32_MAX, 1));
  assert(!e2a_manifest_string_valid("a\0b", 4, 0, 3));
  puts("manifest bounds: OK");
  return 0;
}
