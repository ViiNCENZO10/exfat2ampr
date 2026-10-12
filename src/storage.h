/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * Destination storages, from the kernel's real mount table (getfsstat):
 *  - the internal storage is /data/homebrew, the homebrew data area (never the system partitions);
 *  - an external drive is listed only when /mnt/usbN or /mnt/extN is a real mount point (an empty
 *    mount-point directory reports its parent filesystem: it is never listed and never written);
 *  - one entry per device (same source or same filesystem id = same drive);
 *  - capacity and free space come from that filesystem, write access is proven with a probe file.
 * The pure filtering step (storage_build) is unit-tested on the host with fake mount tables. */
#ifndef E2A_STORAGE_H
#define E2A_STORAGE_H

#include "common.h"

#define E2A_MAX_STORAGES 24

typedef struct {
  char on[256];          /* mount point */
  char from[256];        /* device or source */
  char type[32];         /* filesystem type */
  uint64_t bsize, blocks, bfree, bavail;
  int32_t fsid[2];
  bool rdonly;
} e2a_mnt;

typedef struct {
  char base[300];        /* where games go: <mount>/homebrew */
  char mount[256];
  char device[256];
  char kind[16];         /* internal | usb | extended */
  int index;             /* N of /mnt/usbN, /mnt/extN; -1 for internal */
  char label[64];        /* English fallback; clients localize kind + index */
  char fstype[16];       /* exFAT, UFS, FAT32, ... */
  uint64_t total, avail;
  bool writable;
  char reason[24];       /* why not writable: read_only, no_write, full */
} e2a_storage;

/* recs: mount table; internal: statfs of the internal area (NULL when unavailable).
 * probe_write: create <mount>/homebrew and a probe file on real, read-write mounts only. */
int storage_build(const e2a_mnt *recs, int n, const e2a_mnt *internal, e2a_storage *out, int max, bool probe_write);

/* current destinations (mount table read now) */
int storage_scan(e2a_storage *out, int max, bool probe_write);
/* changes whenever a drive is plugged in or removed */
uint64_t storage_signature(void);
/* the storage that holds path (longest mount prefix), or -1; out filled */
int storage_find(const char *path, e2a_storage *out);
/* dest_base must be exactly one of the current, writable destinations */
bool storage_dest_allowed(const char *dest_base);
/* JSON array for /api/destinations */
int storage_list_json(char *json, size_t cap);

#endif
