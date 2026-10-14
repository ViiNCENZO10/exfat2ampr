/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * AMPRIDX3 writer (port of drakmor/ampr_emu tools/build_ampr_index.py), and the repair of folders converted
 * before the index used ampr_emu's hash. */
#include "ampr_pack.h"
#include "manifest_validate.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
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
    uint64_t h = e2a_ampridx_hash(l->v[i].key, strlen(l->v[i].key));   /* not the pack's basis: see util.c */
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
    if (fsync(fd) != 0) rc = -1;
    if (close(fd) != 0) rc = -1;
    if (rc == 0 && (rename(tmp, path) != 0 || e2a_sync_parent(path) != 0)) rc = -1;
    if (rc != 0) unlink(tmp);
  }
  free(recs); free(blob); free(table);
  if (rc == 0) e2a_log("index: %zu entries, %u slots -> %s", n, slots, path);
  return rc;
}

/* ---------------------------------------------------------------- repair -- */
/* The pack manifest's file records in fileId order, as entries (rel without "/app0/"); packed[i] = 1 for the
 * files that live in the volumes. */
static int manifest_entries(const char *manifest, e2a_list *l, uint8_t **packed_out) {
  char *doc = NULL;
  size_t len = 0;
  int rc = -1;
  uint8_t *packed = NULL;
  if (e2a_read_text(manifest, &doc, &len, 1u << 30) != 0 || !e2a_manifest_valid(doc, len)) goto out;
  ampr_pak_header h;
  memcpy(&h, doc, sizeof h);
  if (!h.file_count || h.file_count > (16u << 20)) goto out;
  const ampr_file_record *files = (const ampr_file_record *)(doc + h.files_offset);
  const char *strings = doc + h.strings_offset;
  packed = calloc(h.file_count, 1);
  if (!packed) goto out;
  for (uint64_t i = 0; i < h.file_count; i++) {
    const ampr_file_record *fr = &files[i];
    if (e2a_list_add(l, strings + fr->path_offset + 6, fr->logical_size, fr->mtime, true) != 0) goto out;
    packed[i] = (fr->flags & AMPR_FILE_PACKED) ? 1 : 0;
  }
  rc = 0;
out:
  free(doc);
  if (rc == 0) *packed_out = packed;
  else free(packed);
  return rc;
}

/* ampr_emu.index describes exactly the manifest's files, in the same order, with ampr_emu's slot hash */
static bool index_matches(const char *index, const e2a_list *l) {
  char *doc = NULL;
  size_t len = 0;
  if (e2a_read_text(index, &doc, &len, 64u << 20) != 0) return false;
  const uint8_t *b = (const uint8_t *)doc;
  bool ok = false;
  ampr_idx_header h;
  uint32_t slots = 2;
  while ((uint64_t)slots < (uint64_t)l->n * 2) slots *= 2;
  if (len < sizeof h) goto out;
  memcpy(&h, b, sizeof h);
  if (memcmp(h.magic, AMPR_MAGIC_IDX, 8) != 0 || h.version != 3 || h.entry_size != sizeof(ampr_idx_entry) ||
      h.entry_count != l->n || h.hash_slot_size != sizeof(ampr_idx_slot) || h.hash_slot_count != slots)
    goto out;
  uint64_t blob = sizeof h + h.entry_count * sizeof(ampr_idx_entry);
  if (blob + h.path_bytes > len || h.hash_offset < blob + h.path_bytes ||
      h.hash_offset + (uint64_t)slots * sizeof(ampr_idx_slot) > len)
    goto out;
  for (size_t i = 0; i < l->n; i++) {
    ampr_idx_entry e;
    memcpy(&e, b + sizeof h + i * sizeof e, sizeof e);
    size_t rl = strlen(l->v[i].rel);
    if (e.size != l->v[i].size || e.mtime != l->v[i].mtime || e.path_len != 6 + rl ||
        (uint64_t)e.path_at + e.path_len > h.path_bytes || strncasecmp((const char *)b + blob + e.path_at, "/app0/", 6) != 0 ||
        strncasecmp((const char *)b + blob + e.path_at + 6, l->v[i].rel, rl) != 0)
      goto out;
  }
  for (uint32_t s = 0; s < slots; s++) {
    ampr_idx_slot sl;
    memcpy(&sl, b + h.hash_offset + (uint64_t)s * sizeof sl, sizeof sl);
    if (sl.index_plus_one == 0) continue;
    if (sl.index_plus_one > l->n) goto out;
    const char *k = l->v[sl.index_plus_one - 1].key;
    if (sl.hash != e2a_ampridx_hash(k, strlen(k))) goto out;
  }
  ok = true;
out:
  free(doc);
  return ok;
}

/* 1 = execute bit added, 0 = already there (or not a regular file), -1 = chmod failed */
static int exec_bit(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || (st.st_mode & 0111)) return 0;
  return chmod(path, 0755) == 0 ? 1 : -1;
}

int e2a_repair_converted(const char *game_dir, char *report, size_t cap) {
  char manifest[1100], index[1100], old[1120];
  if (cap) report[0] = 0;
  snprintf(manifest, sizeof manifest, "%s/ampr_assets.index", game_dir);
  if (!e2a_is_file(manifest)) return 0;   /* not a converted folder */
  e2a_list l;
  memset(&l, 0, sizeof l);
  uint8_t *packed = NULL;
  if (manifest_entries(manifest, &l, &packed) != 0) {
    snprintf(report, cap, "unreadable pack manifest");
    e2a_list_free(&l);
    return -1;
  }
  /* Resolve every existing loose path before changing either the index or permissions. */
  for (size_t i = 0; i < l.n; i++) {
    if (packed[i]) continue;
    char p[1400];
    int n = snprintf(p, sizeof p, "%s/%s", game_dir, l.v[i].rel);
    if (n < 0 || (size_t)n >= sizeof p || !e2a_path_inside(game_dir, p)) {
      snprintf(report, cap, "missing or unsafe loose path: %.150s", l.v[i].rel);
      e2a_list_free(&l); free(packed); return -1;
    }
  }
  int fixes = 0, failed = 0;
  size_t at = 0;
  snprintf(index, sizeof index, "%s/ampr_emu.index", game_dir);
  if (!index_matches(index, &l)) {
    /* the previous one stays next to it (the emulator never opens it); the new one replaces it atomically */
    snprintf(old, sizeof old, "%s.e2a-old", index);
    struct stat st;
    if (stat(index, &st) == 0 && e2a_copy_file(index, old, (int64_t)st.st_mtime) != 0) {
      snprintf(report, cap, "cannot back up previous index");
      e2a_list_free(&l); free(packed); return -1;
    }
    if (e2a_write_index(index, &l) == 0) fixes++;
    else failed++;
    snprintf(report, cap, "%s", failed ? "cannot rewrite ampr_emu.index" : "ampr_emu.index rebuilt from the pack manifest");
    at = strlen(report);
  }
  /* the loose executables, then whatever the fakelib folders carry (plugins added after the conversion) */
  unsigned modes = 0;
  for (size_t i = 0; i < l.n; i++) {
    if (packed[i] || !e2a_is_executable_name(l.v[i].rel)) continue;
    char p[1400];
    snprintf(p, sizeof p, "%s/%s", game_dir, l.v[i].rel);
    int r = exec_bit(p);
    if (r > 0) modes++;
    else if (r < 0) failed++;
  }
  static const char *const LIBS[] = {"fakelib", "fakelib2", NULL};
  for (int k = 0; LIBS[k]; k++) {
    char dir[1200];
    snprintf(dir, sizeof dir, "%s/%s", game_dir, LIBS[k]);
    DIR *d = opendir(dir);
    if (!d) continue;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
      if (de->d_name[0] == '.' || !e2a_is_executable_name(de->d_name)) continue;
      char p[1500];
      snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
      int r = e2a_path_inside(game_dir, p) ? exec_bit(p) : -1;
      if (r > 0) modes++;
      else if (r < 0) failed++;
    }
    closedir(d);
  }
  if (modes) {
    fixes++;
    if (at < cap) snprintf(report + at, cap - at, "%sexecute bit set on %u file(s)", at ? "; " : "", modes);
  }
  if (failed && !report[0]) snprintf(report, cap, "cannot set the execute bit");
  e2a_list_free(&l);
  free(packed);
  return failed ? -1 : fixes;
}
