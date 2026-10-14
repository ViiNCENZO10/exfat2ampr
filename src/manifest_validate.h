/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE. */
#ifndef E2A_MANIFEST_VALIDATE_H
#define E2A_MANIFEST_VALIDATE_H
/* A manifest read from disk is checked before any pointer is formed from it: record sizes, section offsets
 * in order and within the file (divisions, never multiplications that could overflow), strings inside the
 * table and NUL-terminated. */
#include "ampr_format.h"
#include "common.h"
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
/* Shared by verification, repair and resume: no mutation trusts a weaker parser. */
static inline bool e2a_manifest_valid(const void *data, size_t len) {
  if (len < sizeof(ampr_pak_header)) return false;
  ampr_pak_header h;
  memcpy(&h, data, sizeof h);
  uint32_t crc = h.header_crc; h.header_crc = 0;
  const uint8_t *b = data;
  if (memcmp(h.magic, AMPR_MAGIC_PAK, 8) || h.version != 4 || h.header_size != sizeof h ||
      !e2a_manifest_layout_valid(&h, len) || e2a_crc32(0, &h, sizeof h) != crc ||
      e2a_crc32(0, b + sizeof h, len - sizeof h) != h.payload_crc) return false;
  const char *strings = (const char *)b + h.strings_offset;
  const ampr_file_record *files = (const ampr_file_record *)(b + h.files_offset);
  for (uint64_t i = 0; i < h.file_count; i++) {
    const ampr_file_record *f = &files[i];
    if (!e2a_manifest_string_valid(strings, h.strings_size, f->path_offset, f->path_length)) return false;
    const char *p = strings + f->path_offset;
    if (strncmp(p, "/app0/", 6) || !e2a_safe_relative(p + 6) ||
        f->path_hash != e2a_fnv1a64_fold(p, f->path_length) ||
        f->first_chunk > h.chunk_count || f->chunk_count > h.chunk_count - f->first_chunk) return false;
    if (f->flags & AMPR_FILE_PACKED) {
      if (f->block_shift < 14 || f->block_shift > 20) return false;
      uint64_t bs = 1ull << f->block_shift;
      if (f->chunk_count != f->logical_size / bs + (f->logical_size % bs != 0)) return false;
    }
  }
  const ampr_pack_record *packs = (const ampr_pack_record *)(b + h.packs_offset);
  for (uint32_t i = 0; i < h.pack_count; i++) {
    const ampr_pack_record *p = &packs[i];
    if (!e2a_manifest_string_valid(strings, h.strings_size, p->name_offset, p->name_length) ||
        !e2a_safe_relative(strings + p->name_offset) || strchr(strings + p->name_offset, '/')) return false;
  }
  return true;
}
#endif
