/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "verify.h"
#include "activity.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lz4.h"

#define MAX_BLOCK (1u << 20)   /* block_size = 16 KiB .. 1 MiB per file */

static void fail(e2a_verify_stats *vs, const char *fmt, const char *a, unsigned long long b) {
  vs->errors++;
  if (!vs->first_error[0]) snprintf(vs->first_error, sizeof vs->first_error, fmt, a, b);
  if (vs->errors <= 20) e2a_log(fmt, a, b);
}

static int read_file(const char *path, uint8_t **out, size_t *len) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size > (1ll << 31)) { close(fd); return -1; }
  uint8_t *buf = malloc((size_t)st.st_size + 1);
  size_t got = 0;
  if (!buf || e2a_read_full(fd, buf, (size_t)st.st_size, &got) != 0 || got != (size_t)st.st_size) { free(buf); close(fd); return -1; }
  close(fd);
  *out = buf; *len = got;
  return 0;
}

int e2a_verify(const char *out_dir, const e2a_pack_opts *o, e2a_verify_stats *vs) {
  memset(vs, 0, sizeof *vs);
  char path[1100];
  uint8_t *idx = NULL, *crcf = NULL;
  size_t idx_len = 0, crc_len = 0;
  int rc = E2A_PACK_ERROR;
  int *pack_fd = NULL;
  uint8_t *stored = NULL, *raw = NULL;
  ampr_pak_header h;
  memset(&h, 0, sizeof h);

  snprintf(path, sizeof path, "%s/ampr_assets.index", out_dir);
  if (read_file(path, &idx, &idx_len) != 0 || idx_len < sizeof(ampr_pak_header)) { fail(vs, "cannot read %s (%llu)", path, 0); goto out; }
  memcpy(&h, idx, sizeof h);
  {
    ampr_pak_header hc = h;
    hc.header_crc = 0;
    if (memcmp(h.magic, AMPR_MAGIC_PAK, 8) != 0 || h.version != 4 || h.header_size != 128 ||
        e2a_crc32(0, &hc, sizeof hc) != h.header_crc) { fail(vs, "manifest header invalid %s (%llu)", "", 0); goto out; }
    if (idx_len != 128 + h.file_count * 48 + h.chunk_count * 12 + (uint64_t)h.pack_count * 32 + h.strings_size ||
        e2a_crc32(0, idx + 128, idx_len - 128) != h.payload_crc) { fail(vs, "manifest payload crc mismatch %s (%llu)", "", 0); goto out; }
  }
  const ampr_file_record *files = (const ampr_file_record *)(idx + h.files_offset);
  const ampr_chunk_record *chunks = (const ampr_chunk_record *)(idx + h.chunks_offset);
  const ampr_pack_record *packs = (const ampr_pack_record *)(idx + h.packs_offset);
  const char *strings = (const char *)(idx + h.strings_offset);

  snprintf(path, sizeof path, "%s/ampr_assets.index.crc", out_dir);
  if (read_file(path, &crcf, &crc_len) != 0 || crc_len < sizeof(ampr_crc_header)) { fail(vs, "cannot read %s (%llu)", path, 0); goto out; }
  ampr_crc_header ch;
  memcpy(&ch, crcf, sizeof ch);
  {
    ampr_crc_header cc = ch;
    cc.header_crc = 0;
    if (memcmp(ch.magic, AMPR_MAGIC_CRC, 8) != 0 || e2a_crc32(0, &cc, sizeof cc) != ch.header_crc ||
        memcmp(ch.build_id, h.build_id, 16) != 0 || ch.chunk_count != h.chunk_count ||
        crc_len != 48 + 4 * ch.chunk_count || e2a_crc32(0, crcf + 48, crc_len - 48) != ch.payload_crc) {
      fail(vs, "crc sidecar invalid or not matching the manifest %s (%llu)", "", 0); goto out;
    }
  }
  const uint32_t *crcs = (const uint32_t *)(crcf + 48);

  /* volumes */
  pack_fd = malloc(sizeof(int) * (h.pack_count ? h.pack_count : 1));
  if (!pack_fd) goto out;
  for (uint32_t p = 0; p < h.pack_count; p++) pack_fd[p] = -1;
  for (uint32_t p = 0; p < h.pack_count; p++) {
    snprintf(path, sizeof path, "%s/%s", out_dir, strings + packs[p].name_offset);
    pack_fd[p] = open(path, O_RDONLY);
    ampr_dat_header dh;
    if (pack_fd[p] < 0 || e2a_pread_all(pack_fd[p], &dh, sizeof dh, 0) != 0) { fail(vs, "cannot open volume %s (%llu)", path, 0); continue; }
    ampr_dat_header dc = dh;
    dc.header_crc = 0;
    struct stat st;
    if (memcmp(dh.magic, AMPR_MAGIC_DAT, 8) != 0 || e2a_crc32(0, &dc, sizeof dc) != dh.header_crc || dh.pack_id != p ||
        memcmp(dh.build_id, h.build_id, 16) != 0 || fstat(pack_fd[p], &st) != 0 || (uint64_t)st.st_size != packs[p].file_size ||
        dh.payload_offset + dh.payload_bytes != packs[p].file_size)
      fail(vs, "volume header invalid %s (%llu)", path, p);
  }
  if (vs->errors) goto out;

  uint64_t total = 0;
  for (uint64_t f = 0; f < h.file_count; f++) total += files[f].logical_size;
  stored = malloc(1u << 20);
  raw = malloc(MAX_BLOCK);
  if (!stored || !raw) goto out;
  uint64_t done = 0;
  double last_report = 0;

  for (uint64_t f = 0; f < h.file_count; f++) {
    const ampr_file_record *fr = &files[f];
    const char *rel = strings + fr->path_offset;
    if (strncmp(rel, "/app0/", 6) == 0) rel += 6;
    if (o->cancel && *o->cancel) goto out;
    if (o->pause && *o->pause) { rc = E2A_PACK_PAUSED; goto out; }
    if (!(fr->flags & AMPR_FILE_PACKED)) {
      snprintf(path, sizeof path, "%s/%s", out_dir, rel);
      struct stat st;
      if (stat(path, &st) != 0 || (uint64_t)st.st_size != fr->logical_size) fail(vs, "loose file missing or wrong size: %s (%llu)", rel, fr->logical_size);
      vs->files_loose++;
      done += fr->logical_size;
      continue;
    }
    vs->files_packed++;
    if (fr->block_shift < 14 || fr->block_shift > 20) { fail(vs, "bad block size: %s (shift %llu)", rel, (unsigned long long)fr->block_shift); continue; }
    uint32_t bs = 1u << fr->block_shift;
    uint64_t remaining = fr->logical_size;
    for (uint32_t c = 0; c < fr->chunk_count; c++) {
      uint64_t ci = fr->first_chunk + c;
      if (ci >= h.chunk_count) { fail(vs, "chunk index out of range: %s (%llu)", rel, ci); break; }
      const ampr_chunk_record *cr = &chunks[ci];
      uint32_t pack_id = (uint32_t)(cr->location >> 48);
      uint64_t off = cr->location & 0xFFFFFFFFFFFFull;
      uint32_t stored_size = (cr->descriptor & 0xFFFFF) + 1;
      uint32_t codec = (cr->descriptor >> 20) & 3;
      uint32_t raw_size = (uint32_t)(remaining < bs ? remaining : bs);
      if (pack_id >= h.pack_count || pack_fd[pack_id] < 0 || off + stored_size > packs[pack_id].file_size) { fail(vs, "chunk location invalid: %s (%llu)", rel, ci); break; }
      if (e2a_pread_all(pack_fd[pack_id], stored, stored_size, off) != 0) { fail(vs, "chunk read failed: %s (%llu)", rel, ci); break; }
      const uint8_t *data = stored;
      if (codec == AMPR_CODEC_LZ4) {
        int n = LZ4_decompress_safe((const char *)stored, (char *)raw, (int)stored_size, (int)raw_size);
        if (n < 0 || (uint32_t)n != raw_size) { fail(vs, "lz4 decode failed: %s (%llu)", rel, ci); break; }
        data = raw;
      } else if (stored_size != raw_size) { fail(vs, "raw chunk size mismatch: %s (%llu)", rel, ci); break; }
      if (e2a_crc32(0, data, raw_size) != crcs[ci]) { fail(vs, "crc mismatch: %s (%llu)", rel, ci); break; }
      remaining -= raw_size;
      done += raw_size;
      vs->chunks++;
      vs->bytes += raw_size;
      activity_throttle(stored_size);
    }
    if (remaining != 0 && vs->errors == 0) fail(vs, "chunk coverage incomplete: %s (%llu)", rel, remaining);
    double now = e2a_now();
    if (o->progress && now - last_report > 0.5) { last_report = now; o->progress(o->ctx, done, rel); }
  }
  if (o->progress) o->progress(o->ctx, total, "");
  rc = vs->errors ? E2A_PACK_ERROR : E2A_PACK_OK;
  e2a_log("verify: %llu packed, %llu loose, %llu chunks, %llu errors", (unsigned long long)vs->files_packed,
          (unsigned long long)vs->files_loose, (unsigned long long)vs->chunks, (unsigned long long)vs->errors);

out:
  if (pack_fd) { for (uint32_t p = 0; p < h.pack_count; p++) if (pack_fd[p] >= 0) close(pack_fd[p]); free(pack_fd); }
  free(stored); free(raw); free(idx); free(crcf);
  return rc;
}

int e2a_manifest_actions(const char *out_dir, e2a_list *l) {
  char path[1100];
  uint8_t *idx = NULL;
  size_t idx_len = 0;
  snprintf(path, sizeof path, "%s/ampr_assets.index", out_dir);
  if (read_file(path, &idx, &idx_len) != 0 || idx_len < sizeof(ampr_pak_header)) { free(idx); return -1; }
  ampr_pak_header h;
  memcpy(&h, idx, sizeof h);
  if (memcmp(h.magic, AMPR_MAGIC_PAK, 8) != 0 || h.file_count != l->n || idx_len < 128 + h.file_count * 48) { free(idx); return -1; }
  const ampr_file_record *files = (const ampr_file_record *)(idx + h.files_offset);
  for (size_t i = 0; i < l->n; i++) {
    char canon[1100];
    snprintf(canon, sizeof canon, "/app0/%s", l->v[i].rel);
    if (files[i].path_hash != e2a_fnv1a64_fold(canon, strlen(canon))) { free(idx); return -1; }
    if (!(files[i].flags & AMPR_FILE_PACKED)) l->v[i].action = E2A_LOOSE;
    else l->v[i].action = (files[i].flags & AMPR_FILE_STORE_ONLY) ? E2A_STORE : E2A_COMPRESS;
  }
  free(idx);
  return 0;
}
