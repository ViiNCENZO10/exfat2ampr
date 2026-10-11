/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "storage.h"
#include "bench.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef E2A_HOST
#include <sys/statvfs.h>
#else
#include <sys/param.h>
#include <sys/mount.h>
#endif

#define MIN_USEFUL_BYTES (256ull << 20)

/* "/mnt/usbN" or "/mnt/extN" (host tests: "$E2A_FAKE_ROOT/mnt/usbN") */
static bool mount_slot(const char *on, char kind[16], int *index) {
  const char *root = "";
#ifdef E2A_HOST
  root = getenv("E2A_FAKE_ROOT");
  if (!root) return false;
#endif
  size_t rl = strlen(root);
  if (strncmp(on, root, rl) != 0) return false;
  const char *p = on + rl;
  const char *k = NULL;
  if (!strncmp(p, "/mnt/usb", 8)) k = "usb";
  else if (!strncmp(p, "/mnt/ext", 8)) k = "extended";
  if (!k) return false;
  p += 8;
  if (*p < '0' || *p > '9') return false;
  int n = 0, digits = 0;
  while (*p >= '0' && *p <= '9' && digits < 3) { n = n * 10 + (*p++ - '0'); digits++; }
  if (*p) return false;   /* exactly the mount root, never a sub-folder or a sandbox alias */
  snprintf(kind, 16, "%s", k);
  *index = n;
  return true;
}

static bool virtual_fs(const char *type) {
  static const char *const v[] = {"devfs", "fdescfs", "procfs", "linprocfs", "tmpfs", "nsfs", NULL};
  for (int i = 0; v[i]; i++)
    if (!strcmp(type, v[i])) return true;
  return false;
}

static void pretty_fs(const char *type, char out[16]) {
  const char *p = type;
  if (!strcasecmp(type, "exfatfs") || !strcasecmp(type, "exfat")) p = "exFAT";
  else if (!strcasecmp(type, "msdosfs") || !strcasecmp(type, "vfat")) p = "FAT32";
  else if (!strcasecmp(type, "ufs")) p = "UFS";
  else if (!strncasecmp(type, "ntfs", 4)) p = "NTFS";
  snprintf(out, 16, "%s", p);
}

static void sizes(const e2a_mnt *m, uint64_t *total, uint64_t *avail) {
  uint64_t blocks = m->blocks, bfree = m->bfree > blocks ? blocks : m->bfree, bavail = m->bavail > bfree ? bfree : m->bavail;
  *total = blocks * m->bsize;
  *avail = bavail * m->bsize;
}

/* proves write access: create the homebrew folder and a probe file, then remove the probe */
static bool probe(const char *base) {
  if (e2a_mkdir_p(base) != 0) return false;
  char p[400];
  static unsigned counter;
  snprintf(p, sizeof p, "%s/.e2a-probe-%d-%u", base, (int)getpid(), __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED));
  int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0) return false;
  bool ok = write(fd, "ok", 2) == 2;
  if (close(fd) != 0) ok = false;
  unlink(p);
  return ok;
}

static int order(const e2a_storage *s) {
  return (!strcmp(s->kind, "internal") ? 0 : !strcmp(s->kind, "usb") ? 1000 : 2000) + (s->index < 0 ? 0 : s->index);
}

static int cmp_storage(const void *a, const void *b) { return order(a) - order(b); }

int storage_build(const e2a_mnt *recs, int n, const e2a_mnt *internal, e2a_storage *out, int max, bool probe_write) {
  int count = 0;
  /* identity of each accepted filesystem, to drop a drive seen through a second path */
  struct { char from[256]; int32_t fsid[2]; uint64_t blocks, bsize; char type[32]; } seen[E2A_MAX_STORAGES + 1];
  int nseen = 0;

  if (internal && internal->blocks && internal->bsize && count < max) {
    e2a_storage *s = &out[count++];
    memset(s, 0, sizeof *s);
    snprintf(s->base, sizeof s->base, "%s", E2A_HOMEBREW_DIR);   /* /data/homebrew: the homebrew data area */
    snprintf(s->mount, sizeof s->mount, "%s", internal->on);
    snprintf(s->device, sizeof s->device, "%s", internal->from);
    snprintf(s->kind, sizeof s->kind, "internal");
    s->index = -1;
    snprintf(s->label, sizeof s->label, "PS5 internal storage");
    pretty_fs(internal->type, s->fstype);
    sizes(internal, &s->total, &s->avail);
    s->writable = !internal->rdonly && (!probe_write || probe(s->base));
    if (!s->writable) snprintf(s->reason, sizeof s->reason, "%s", internal->rdonly ? "read_only" : "no_write");
    else if (s->avail < MIN_USEFUL_BYTES) snprintf(s->reason, sizeof s->reason, "full");
    snprintf(seen[nseen].from, sizeof seen[nseen].from, "%s", internal->from);
    memcpy(seen[nseen].fsid, internal->fsid, sizeof seen[nseen].fsid);
    seen[nseen].blocks = internal->blocks; seen[nseen].bsize = internal->bsize;
    snprintf(seen[nseen].type, sizeof seen[nseen].type, "%s", internal->type);
    nseen++;
  }

  for (int i = 0; i < n && count < max; i++) {
    const e2a_mnt *m = &recs[i];
    char kind[16];
    int index;
    if (!mount_slot(m->on, kind, &index)) continue;
    if (virtual_fs(m->type) || !m->blocks || !m->bsize) continue;
    bool dup = false;
    for (int k = 0; k < nseen && !dup; k++) {
      if (m->from[0] && !strcmp(m->from, seen[k].from)) dup = true;                         /* same device */
      else if ((m->fsid[0] || m->fsid[1]) && !memcmp(m->fsid, seen[k].fsid, sizeof m->fsid)) dup = true;   /* same fs */
      else if (!strcmp(m->type, "nullfs") && m->blocks == seen[k].blocks && m->bsize == seen[k].bsize) dup = true;
    }
    /* a nullfs alias whose source is another storage (or a folder of it) is not a second drive */
    if (!dup && !strcmp(m->type, "nullfs")) {
      for (int k = 0; k < count && !dup; k++) {
        size_t ml = strlen(out[k].mount);
        if (ml && !strncmp(m->from, out[k].mount, ml) && (m->from[ml] == '/' || !m->from[ml])) dup = true;
      }
      if (!strncmp(m->from, "/data", 5) || !strncmp(m->from, "/user", 5)) dup = true;
    }
    if (dup) continue;
    e2a_storage *s = &out[count++];
    memset(s, 0, sizeof *s);
    snprintf(s->mount, sizeof s->mount, "%s", m->on);
    snprintf(s->base, sizeof s->base, "%s/homebrew", m->on);
    snprintf(s->device, sizeof s->device, "%s", m->from);
    snprintf(s->kind, sizeof s->kind, "%s", kind);
    s->index = index;
    snprintf(s->label, sizeof s->label, "%s %d", !strcmp(kind, "usb") ? "USB" : "Extended storage", index);
    pretty_fs(m->type, s->fstype);
    sizes(m, &s->total, &s->avail);
    s->writable = !m->rdonly && (!probe_write || probe(s->base));
    if (!s->writable) snprintf(s->reason, sizeof s->reason, "%s", m->rdonly ? "read_only" : "no_write");
    else if (s->avail < MIN_USEFUL_BYTES) snprintf(s->reason, sizeof s->reason, "full");
    if (nseen < E2A_MAX_STORAGES) {
      snprintf(seen[nseen].from, sizeof seen[nseen].from, "%s", m->from);
      memcpy(seen[nseen].fsid, m->fsid, sizeof seen[nseen].fsid);
      seen[nseen].blocks = m->blocks; seen[nseen].bsize = m->bsize;
      snprintf(seen[nseen].type, sizeof seen[nseen].type, "%s", m->type);
      nseen++;
    }
  }
  qsort(out, (size_t)count, sizeof *out, cmp_storage);
  return count;
}

/* --------------------------------------------------------- mount table -- */
#ifdef E2A_HOST
/* host tests: E2A_FAKE_MOUNTS = file of "on|from|type|bsize|blocks|bfree|bavail|fsid0|fsid1|ro" lines */
static int read_mounts(e2a_mnt **out) {
  *out = NULL;
  const char *path = getenv("E2A_FAKE_MOUNTS");
  if (!path) return 0;
  char *doc = NULL;
  if (e2a_read_text(path, &doc, NULL, 1 << 20) != 0) return -1;
  int cap = 64, n = 0;
  e2a_mnt *v = calloc((size_t)cap, sizeof *v);
  for (char *line = strtok(doc, "\n"); line && v && n < cap; line = strtok(NULL, "\n")) {
    if (!line[0] || line[0] == '#') continue;
    e2a_mnt *m = &v[n];
    char *f[10] = {0};
    int k = 0;
    for (char *p = line; k < 10; k++) {
      f[k] = p;
      char *bar = strchr(p, '|');
      if (!bar) { k++; break; }
      *bar = 0;
      p = bar + 1;
    }
    if (k < 10) continue;
    snprintf(m->on, sizeof m->on, "%s", f[0]);
    snprintf(m->from, sizeof m->from, "%s", f[1]);
    snprintf(m->type, sizeof m->type, "%s", f[2]);
    m->bsize = strtoull(f[3], NULL, 10); m->blocks = strtoull(f[4], NULL, 10);
    m->bfree = strtoull(f[5], NULL, 10); m->bavail = strtoull(f[6], NULL, 10);
    m->fsid[0] = (int32_t)atoi(f[7]); m->fsid[1] = (int32_t)atoi(f[8]);
    m->rdonly = f[9][0] == '1';
    n++;
  }
  free(doc);
  *out = v;
  return v ? n : -1;
}

static bool read_internal(e2a_mnt *m) {
  struct statvfs sv;
  memset(m, 0, sizeof *m);
  if (statvfs(".", &sv) != 0) return false;
  snprintf(m->on, sizeof m->on, ".");
  snprintf(m->from, sizeof m->from, "host");
  snprintf(m->type, sizeof m->type, "host");
  m->bsize = sv.f_frsize; m->blocks = sv.f_blocks; m->bfree = sv.f_bfree; m->bavail = sv.f_bavail;
  m->rdonly = (sv.f_flag & ST_RDONLY) != 0;
  return true;
}
#else
static void from_statfs(const struct statfs *sf, e2a_mnt *m) {
  memset(m, 0, sizeof *m);
  snprintf(m->on, sizeof m->on, "%s", sf->f_mntonname);
  snprintf(m->from, sizeof m->from, "%s", sf->f_mntfromname);
  snprintf(m->type, sizeof m->type, "%s", sf->f_fstypename);
  m->bsize = (uint64_t)sf->f_bsize;
  m->blocks = (uint64_t)sf->f_blocks;
  m->bfree = (uint64_t)sf->f_bfree;
  m->bavail = sf->f_bavail > 0 ? (uint64_t)sf->f_bavail : 0;
  memcpy(m->fsid, &sf->f_fsid, sizeof m->fsid);
  m->rdonly = (sf->f_flags & MNT_RDONLY) != 0;
}

/* snapshot of the kernel mount table (grows the buffer if mounts appear meanwhile) */
static int read_mounts(e2a_mnt **out) {
  *out = NULL;
  int count = getfsstat(NULL, 0, MNT_NOWAIT);
  if (count < 0) return -1;
  size_t capacity = (size_t)count + 16u;
  for (int attempt = 0; attempt < 3; attempt++) {
    struct statfs *sf = malloc(capacity * sizeof *sf);
    if (!sf) return -1;
    count = getfsstat(sf, (long)(capacity * sizeof *sf), MNT_NOWAIT);
    if (count >= 0 && (size_t)count < capacity) {
      e2a_mnt *v = calloc((size_t)count + 1, sizeof *v);
      if (!v) { free(sf); return -1; }
      for (int i = 0; i < count; i++) from_statfs(&sf[i], &v[i]);
      free(sf);
      *out = v;
      return count;
    }
    free(sf);
    if (count < 0) return -1;
    capacity *= 2;
  }
  return -1;
}

static bool read_internal(e2a_mnt *m) {
  struct statfs sf;
  if (statfs("/data", &sf) != 0) return false;
  from_statfs(&sf, m);
  snprintf(m->on, sizeof m->on, "/data");   /* /data may be an alias of /user/data: name it as used */
  return true;
}
#endif

int storage_scan(e2a_storage *out, int max, bool probe_write) {
  e2a_mnt *recs = NULL, internal;
  int n = read_mounts(&recs);
  if (n < 0) n = 0;
  bool have_internal = read_internal(&internal);
  int count = storage_build(recs, n, have_internal ? &internal : NULL, out, max, probe_write);
  free(recs);
  return count;
}

uint64_t storage_signature(void) {
  e2a_storage s[E2A_MAX_STORAGES];
  int n = storage_scan(s, E2A_MAX_STORAGES, false);
  uint64_t h = 1469598103934665603ull;
  for (int i = 0; i < n; i++) {
    for (const char *p = s[i].mount; *p; p++) { h ^= (uint8_t)*p; h *= 1099511628211ull; }
    for (const char *p = s[i].device; *p; p++) { h ^= (uint8_t)*p; h *= 1099511628211ull; }
    h ^= s[i].total; h *= 1099511628211ull;
  }
  return h ^ (uint64_t)n;
}

int storage_find(const char *path, e2a_storage *out) {
  e2a_storage s[E2A_MAX_STORAGES];
  int n = storage_scan(s, E2A_MAX_STORAGES, false), best = -1;
  size_t best_len = 0;
  for (int i = 0; i < n; i++) {
    const char *root = !strcmp(s[i].kind, "internal") ? s[i].base : s[i].mount;
    size_t l = strlen(root);
    if (!strncmp(path, root, l) && (path[l] == '/' || !path[l]) && l >= best_len) { best = i; best_len = l; }
  }
  if (best >= 0 && out) *out = s[best];
  return best;
}

bool storage_dest_allowed(const char *dest_base) {
  e2a_storage s[E2A_MAX_STORAGES];
  int n = storage_scan(s, E2A_MAX_STORAGES, true);
  for (int i = 0; i < n; i++)
    if (s[i].writable && !strcmp(s[i].base, dest_base)) return true;
  return false;
}

int storage_list_json(char *json, size_t cap) {
  e2a_storage s[E2A_MAX_STORAGES];
  int n = storage_scan(s, E2A_MAX_STORAGES, true);
  size_t pos = 0;
  if (cap < 3) return -1;
  json[pos++] = '[';
  for (int i = 0; i < n; i++) {
    char base[700], mount[600], dev[600];
    e2a_json_escape(s[i].base, base, sizeof base);
    e2a_json_escape(s[i].mount, mount, sizeof mount);
    e2a_json_escape(s[i].device, dev, sizeof dev);
    e2a_speed sp = {-1, -1, 0};   /* measured on demand (/api/speedtest) */
    bench_get(s[i].base, &sp);
    int w = snprintf(json + pos, cap - pos,
                     "%s{\"path\":\"%s\",\"mount\":\"%s\",\"device\":\"%s\",\"kind\":\"%s\",\"index\":%d,\"label\":\"%s\","
                     "\"fstype\":\"%s\",\"total\":%llu,\"free\":%llu,\"writable\":%s,\"reason\":\"%s\","
                     "\"read_mbps\":%d,\"write_mbps\":%d}",
                     i ? "," : "", base, mount, dev, s[i].kind, s[i].index, s[i].label, s[i].fstype,
                     (unsigned long long)s[i].total, (unsigned long long)s[i].avail, s[i].writable ? "true" : "false", s[i].reason,
                     sp.read_mbps, sp.write_mbps);
    if (w < 0 || (size_t)w >= cap - pos - 2) return -1;
    pos += (size_t)w;
  }
  json[pos++] = ']';
  json[pos] = 0;
  return 0;
}
