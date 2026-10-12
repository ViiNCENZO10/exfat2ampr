/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE. */
#ifndef E2A_MANIFEST_VALIDATE_H
#define E2A_MANIFEST_VALIDATE_H
/* A manifest read from disk is checked before any pointer is formed from it: record sizes, section offsets
 * in order and within the file (divisions, never multiplications that could overflow), strings inside the
 * table and NUL-terminated. */
#include "ampr_format.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline bool e2a_manifest_layout_valid(const ampr_pak_header *h, size_t len) {
  uint64_t end = sizeof *h;
  if (len < end || h->endian != AMPR_ENDIAN_MARK || h->file_record_size != 48 || h->chunk_record_size != 12 ||
      h->pack_record_size != 32 || h->pack_count > 1024 || h->files_offset != end || h->file_count > (len - end) / 48)
    return false;
  end += (uint64_t)h->file_count * 48;
  if (h->chunks_offset != end || h->chunk_count > (len - end) / 12) return false;
  end += (uint64_t)h->chunk_count * 12;
  if (h->packs_offset != end || h->pack_count > (len - end) / 32) return false;
  end += (uint64_t)h->pack_count * 32;
  return h->strings_offset == end && h->strings_size == len - end;
}

static inline bool e2a_manifest_string_valid(const char *strings, uint64_t size, uint32_t off, uint32_t len) {
  return off < size && len < size - off && strings[off + (uint64_t)len] == 0 && memchr(strings + off, 0, len) == NULL;
}
#endif
