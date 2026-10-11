/* SPDX-License-Identifier: GPL-3.0-or-later
 * AMPRIDX3 writer (port of drakmor/ampr_emu tools/build_ampr_index.py). */
#include "ampr_pack.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int e2a_write_index(const char *path, const e2a_list *l) {
  if (l->n == 0) { e2a_log("index: no entries"); return -1; }
  size_t n = l->n;
  ampr_idx_entry *recs = calloc(n, sizeof *recs);
  size_t blob_cap = 0;
  for (size_t i = 0; i < n; i++) blob_cap += 6 + strlen(l->v[i].rel) + 1;
  char *blob = malloc(blob_cap);
  if (!recs || !blob) { free(recs); free(blob); return -1; }

  size_t at = 0;
  for (size_t i = 0; i < n; i++) {
    const e2a_entry *e = &l->v[i];
    size_t plen = 6 + strlen(e->rel);
    memcpy(blob + at, "/app0/", 6);
    memcpy(blob + at + 6, e->rel, strlen(e->rel));
    blob[at + plen] = 0;
    recs[i].path_at = (uint32_t)at;
    recs[i].path_len = (uint32_t)plen;
    recs[i].size = e->size;
    recs[i].mtime = e->mtime;
    at += plen + 1;
  }

  uint32_t slots = 2;
  while ((uint64_t)slots < (uint64_t)n * 2) slots *= 2;
  ampr_idx_slot *table = calloc(slots, sizeof *table);
  if (!table) { free(recs); free(blob); return -1; }
  uint32_t mask = slots - 1;
  for (size_t i = 0; i < n; i++) {
    uint64_t h = e2a_fnv1a64_fold(l->v[i].key, strlen(l->v[i].key));
    uint32_t pos = (uint32_t)(h & mask);
    uint32_t dup = 0;
    while (table[pos].index_plus_one != 0) {
      if (table[pos].hash == h) { dup = 1; table[pos].flags |= 1; }
      pos = (pos + 1) & mask;
    }
    table[pos].hash = h;
    table[pos].index_plus_one = (uint32_t)(i + 1);
    table[pos].flags = dup;
  }

  uint64_t path_end = sizeof(ampr_idx_header) + n * sizeof(ampr_idx_entry) + at;
  uint64_t hash_offset = e2a_align_up(path_end, 16);

  ampr_idx_header h;
  memset(&h, 0, sizeof h);
  memcpy(h.magic, AMPR_MAGIC_IDX, 8);
  h.version = 3; h.entry_size = 24; h.entry_count = n; h.path_bytes = at;
  h.hash_offset = hash_offset; h.hash_slot_size = 16; h.hash_slot_count = slots;

  char tmp[1100];
  snprintf(tmp, sizeof tmp, "%s.e2a-tmp", path);
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  int rc = -1;
  if (fd >= 0) {
    static const uint8_t zeros[16] = {0};
    rc = 0;
    if (e2a_write_all(fd, &h, sizeof h) || e2a_write_all(fd, recs, n * sizeof *recs) ||
        e2a_write_all(fd, blob, at) || e2a_write_all(fd, zeros, (size_t)(hash_offset - path_end)) ||
        e2a_write_all(fd, table, (size_t)slots * sizeof *table))
      rc = -1;
    if (close(fd) != 0) rc = -1;
    if (rc == 0 && rename(tmp, path) != 0) rc = -1;
    if (rc != 0) unlink(tmp);
  }
  free(recs); free(blob); free(table);
  if (rc == 0) e2a_log("index: %zu entries, %u slots -> %s", n, slots, path);
  return rc;
}
