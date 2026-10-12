/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * On-disk structures of the AMPR emulator asset packs (drakmor/ampr_emu tools/ampr_pack_format.py).
 * All little-endian, packed. */
#ifndef E2A_AMPR_FORMAT_H
#define E2A_AMPR_FORMAT_H

#include <stdint.h>

#define AMPR_MAGIC_IDX  "AMPRIDX3"
#define AMPR_MAGIC_PAK  "AMPRPAK4"
#define AMPR_MAGIC_DAT  "AMPRDAT3"
#define AMPR_MAGIC_CRC  "AMPRCRC1"
#define AMPR_MAGIC_CFG  "AMPRCFG1"
#define AMPR_ENDIAN_MARK 0x01020304u

/* ---- AMPRIDX3: path -> fileId index (fileId = record index + 1) ---- */
typedef struct __attribute__((packed)) {
  char magic[8];            /* "AMPRIDX3" */
  uint32_t version;         /* 3 */
  uint32_t entry_size;      /* 24 */
  uint64_t entry_count;
  uint64_t path_bytes;      /* size of the NUL-terminated path blob */
  uint64_t hash_offset;     /* 16-byte aligned, >= end of path blob */
  uint32_t hash_slot_size;  /* 16 */
  uint32_t hash_slot_count; /* power of two >= 2 * entry_count, min 2 */
} ampr_idx_header;          /* 48 bytes */

typedef struct __attribute__((packed)) {
  uint32_t path_at;         /* offset in the path blob */
  uint32_t path_len;        /* bytes, without NUL */
  uint64_t size;
  int64_t mtime;            /* unix seconds */
} ampr_idx_entry;           /* 24 bytes */

typedef struct __attribute__((packed)) {
  uint64_t hash;            /* FNV-1a 64 of the folded "/app0/..." path */
  uint32_t index_plus_one;  /* 0 = empty */
  uint32_t flags;           /* 1 = duplicate hash */
} ampr_idx_slot;            /* 16 bytes */

/* ---- AMPRPAK4: runtime storage manifest ---- */
typedef struct __attribute__((packed)) {
  char magic[8];            /* "AMPRPAK4" */
  uint32_t version;         /* 4 */
  uint32_t header_size;     /* 128 */
  uint32_t flags;           /* 0 */
  uint32_t endian;          /* 0x01020304 */
  uint8_t build_id[16];
  uint64_t file_count;
  uint64_t chunk_count;
  uint32_t pack_count;
  uint32_t file_record_size;  /* 48 */
  uint32_t chunk_record_size; /* 12 */
  uint32_t pack_record_size;  /* 32 */
  uint64_t files_offset;      /* 128 */
  uint64_t chunks_offset;
  uint64_t packs_offset;
  uint64_t strings_offset;
  uint64_t strings_size;
  uint32_t payload_crc;       /* crc32 of everything after the header */
  uint32_t header_crc;        /* crc32 of the header with this field zeroed */
  uint64_t reserved;
} ampr_pak_header;            /* 128 bytes */

enum { AMPR_FILE_PACKED = 1u << 0, AMPR_FILE_STORE_ONLY = 1u << 1, AMPR_FILE_STREAMING = 1u << 2,
       AMPR_FILE_HOT = 1u << 3, AMPR_FILE_RANDOM_ACCESS = 1u << 4 };
enum { AMPR_CODEC_RAW = 0, AMPR_CODEC_LZ4 = 1 };
enum { AMPR_CHUNK_SHARED = 1u << 0, AMPR_CHUNK_STREAMING = 1u << 1, AMPR_CHUNK_PAGE_CONTAINED = 1u << 2,
       AMPR_CHUNK_PAGE_ALIGNED = 1u << 3 };
enum { AMPR_PACK_STRIPED = 1u << 0, AMPR_PACK_IO_PAGE_LAYOUT = 1u << 1 };

typedef struct __attribute__((packed)) {
  uint64_t path_hash;
  uint64_t logical_size;
  int64_t mtime;
  uint32_t first_chunk;
  uint32_t chunk_count;
  uint32_t path_offset;     /* in string table */
  uint32_t path_length;     /* bytes, without NUL */
  uint32_t flags;           /* AMPR_FILE_* */
  uint8_t block_shift;      /* log2(block size), 0 for loose */
  uint8_t packing_class;    /* group index */
  uint16_t reserved;
} ampr_file_record;          /* 48 bytes */

typedef struct __attribute__((packed)) {
  uint64_t location;        /* bits 0-47 offset in pack, bits 48-63 pack id */
  uint32_t descriptor;      /* bits 0-19 stored_size-1, 20-21 codec, 22-29 flags */
} ampr_chunk_record;         /* 12 bytes */

typedef struct __attribute__((packed)) {
  uint64_t payload_bytes;
  uint64_t file_size;
  uint32_t name_offset;
  uint32_t name_length;
  uint32_t flags;           /* AMPR_PACK_* */
  uint32_t io_page_size;
} ampr_pack_record;          /* 32 bytes */

/* ---- AMPRDAT3: data volume header ---- */
typedef struct __attribute__((packed)) {
  char magic[8];            /* "AMPRDAT3" */
  uint32_t version;         /* 3 */
  uint32_t header_size;     /* 64 */
  uint32_t pack_id;
  uint32_t flags;           /* subset of the pack record flags */
  uint8_t build_id[16];
  uint64_t payload_offset;  /* multiple of io_page_size, >= 64 */
  uint64_t payload_bytes;   /* file_size - payload_offset */
  uint32_t header_crc;      /* crc32 of the header with this field zeroed */
  uint32_t reserved;
} ampr_dat_header;           /* 64 bytes */

/* ---- AMPRCRC1: offline decoded-block CRC sidecar ---- */
typedef struct __attribute__((packed)) {
  char magic[8];            /* "AMPRCRC1" */
  uint32_t version;         /* 1 */
  uint32_t header_size;     /* 48 */
  uint8_t build_id[16];
  uint64_t chunk_count;
  uint32_t payload_crc;     /* crc32 of the u32 CRC array */
  uint32_t header_crc;      /* crc32 of the header with this field zeroed */
} ampr_crc_header;           /* 48 bytes */

/* ---- AMPRCFG1: runtime settings ---- */
typedef struct __attribute__((packed)) {
  char magic[8];            /* "AMPRCFG1" */
  uint32_t version;         /* 1 */
  uint32_t size;            /* 64 */
  uint8_t build_id[16];
  uint64_t decoded_cache_bytes;
  uint64_t physical_cache_bytes;
  uint32_t workers;         /* 1..16 */
  uint32_t latency_reserve_workers;
  uint32_t checksum;        /* crc32 of the record with this field zeroed */
  uint32_t flags;           /* 0 */
} ampr_cfg_record;           /* 64 bytes */

_Static_assert(sizeof(ampr_idx_header) == 48, "idx header");
_Static_assert(sizeof(ampr_idx_entry) == 24, "idx entry");
_Static_assert(sizeof(ampr_idx_slot) == 16, "idx slot");
_Static_assert(sizeof(ampr_pak_header) == 128, "pak header");
_Static_assert(sizeof(ampr_file_record) == 48, "file record");
_Static_assert(sizeof(ampr_chunk_record) == 12, "chunk record");
_Static_assert(sizeof(ampr_pack_record) == 32, "pack record");
_Static_assert(sizeof(ampr_dat_header) == 64, "dat header");
_Static_assert(sizeof(ampr_crc_header) == 48, "crc header");
_Static_assert(sizeof(ampr_cfg_record) == 64, "cfg record");

#endif
