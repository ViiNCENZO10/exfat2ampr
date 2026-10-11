/* SPDX-License-Identifier: GPL-3.0-or-later
 * Host unit tests (CI): destination detection on simulated PS5 mount tables, AMPR compatibility.
 * Build: see .github/workflows/ps5.yml (unit tests step). */
#include "common.h"
#include "scan.h"
#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_fail;
#define CHECK(cond, ...)                                    \
  do {                                                      \
    if (!(cond)) {                                          \
      g_fail++;                                             \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);           \
      printf(__VA_ARGS__);                                  \
      printf("\n");                                         \
    }                                                       \
  } while (0)

#define GB 1000000000ull
/* sizes go through 32 KiB blocks: compare in whole GB, rounded */
#define ROUND_GB(x) (((x) + GB / 2) / GB)

static e2a_mnt M(const char *on, const char *from, const char *type, uint64_t total, uint64_t avail, int fsid, bool ro) {
  e2a_mnt m;
  memset(&m, 0, sizeof m);
  snprintf(m.on, sizeof m.on, "%s", on);
  snprintf(m.from, sizeof m.from, "%s", from);
  snprintf(m.type, sizeof m.type, "%s", type);
  m.bsize = 32768;
  m.blocks = total / m.bsize;
  m.bfree = avail / m.bsize;
  m.bavail = avail / m.bsize;
  m.fsid[0] = fsid;
  m.fsid[1] = fsid ? 7 : 0;
  m.rdonly = ro;
  return m;
}

/* what the console always has, none of it a destination */
static int system_mounts(e2a_mnt *v) {
  int n = 0;
  v[n++] = M("/", "/dev/ssd0.system", "ufs", 8 * GB, 1 * GB, 1, true);
  v[n++] = M("/dev", "devfs", "devfs", 1, 0, 2, false);
  v[n++] = M("/user", "/dev/ssd0.user", "ufs", 800 * GB, 300 * GB, 3, false);
  v[n++] = M("/mnt/sandbox/NPXS40028_000/app0", "/system/vsh/app/NPXS40028", "nullfs", 8 * GB, 1 * GB, 4, true);
  v[n++] = M("/mnt/shadowmnt/PPSA02530_1234", "/dev/lvd2", "exfatfs", 50 * GB, 1 * GB, 5, true);
  v[n++] = M("/system_tmp", "tmpfs", "tmpfs", 1 * GB, 1 * GB, 6, false);
  return n;
}

static const e2a_storage *find(const e2a_storage *s, int n, const char *mount) {
  for (int i = 0; i < n; i++)
    if (!strcmp(s[i].mount, mount)) return &s[i];
  return NULL;
}

static void test_no_usb(void) {
  e2a_mnt v[16], internal = M("/data", "/dev/ssd0.user", "ufs", 628 * GB, 199 * GB, 3, false);
  int n = system_mounts(v);
  e2a_storage s[E2A_MAX_STORAGES];
  int c = storage_build(v, n, &internal, s, E2A_MAX_STORAGES, false);
  CHECK(c == 1, "no USB: expected only the internal storage, got %d", c);
  CHECK(c >= 1 && !strcmp(s[0].kind, "internal") && !strcmp(s[0].base, E2A_HOMEBREW_DIR), "internal storage first, base %s", s[0].base);
  CHECK(c >= 1 && ROUND_GB(s[0].total) == 628 && ROUND_GB(s[0].avail) == 199, "internal sizes come from the filesystem (%llu/%llu GB)",
        (unsigned long long)ROUND_GB(s[0].avail), (unsigned long long)ROUND_GB(s[0].total));
}

static void test_one_usb(void) {
  e2a_mnt v[16], internal = M("/data", "/dev/ssd0.user", "ufs", 628 * GB, 199 * GB, 3, false);
  int n = system_mounts(v);
  v[n++] = M("/mnt/usb0", "/dev/da0s1", "exfatfs", 931 * GB, 119 * GB, 20, false);
  e2a_storage s[E2A_MAX_STORAGES];
  int c = storage_build(v, n, &internal, s, E2A_MAX_STORAGES, false);
  CHECK(c == 2, "one USB: expected 2 storages, got %d", c);
  const e2a_storage *u = find(s, c, "/mnt/usb0");
  CHECK(u != NULL, "USB 0 listed");
  if (u) {
    CHECK(!strcmp(u->kind, "usb") && u->index == 0, "kind usb index 0 (%s %d)", u->kind, u->index);
    CHECK(!strcmp(u->base, "/mnt/usb0/homebrew"), "base %s", u->base);
    CHECK(!strcmp(u->fstype, "exFAT"), "fstype %s", u->fstype);
    CHECK(ROUND_GB(u->total) == 931 && ROUND_GB(u->avail) == 119, "real sizes %llu/%llu", (unsigned long long)ROUND_GB(u->avail),
          (unsigned long long)ROUND_GB(u->total));
    CHECK(u->writable && !u->reason[0], "writable");
  }
}

static void test_several_usb(void) {
  e2a_mnt v[32], internal = M("/data", "/dev/ssd0.user", "ufs", 628 * GB, 199 * GB, 3, false);
  int n = system_mounts(v);
  v[n++] = M("/mnt/usb1", "/dev/da1s1", "exfatfs", 2000 * GB, 1500 * GB, 21, false);
  v[n++] = M("/mnt/usb0", "/dev/da0s1", "exfatfs", 931 * GB, 119 * GB, 20, false);
  v[n++] = M("/mnt/usb2", "/dev/da0s1", "exfatfs", 931 * GB, 119 * GB, 22, false);          /* same device again */
  v[n++] = M("/mnt/usb3", "/mnt/usb1", "nullfs", 2000 * GB, 1500 * GB, 23, false);        /* alias of usb1 */
  v[n++] = M("/mnt/usb4", "/dev/da2s1", "msdosfs", 64 * GB, 10 * GB, 24, true);           /* read-only stick */
  v[n++] = M("/mnt/usb5", "tmpfs", "tmpfs", 2 * 1000000ull, 2 * 1000000ull, 25, false);   /* "2 MB" fake */
  v[n++] = M("/mnt/usb1/games", "/dev/da1s1", "exfatfs", 2000 * GB, 1500 * GB, 26, false); /* not a root */
  v[n++] = M("/mnt/ext0", "/dev/nvme1", "ufs", 1000 * GB, 400 * GB, 27, false);
  v[n++] = M("/mnt/usb6", "/dev/da3s1", "exfatfs", 500 * GB, 200 * GB, 21, false);        /* same fsid as usb1 */
  e2a_storage s[E2A_MAX_STORAGES];
  int c = storage_build(v, n, &internal, s, E2A_MAX_STORAGES, false);
  CHECK(c == 5, "several USB: expected internal + usb0 + usb1 + usb4 + ext0 = 5, got %d", c);
  for (int i = 0; i < c; i++) printf("  [%d] %-9s %-12s %-6s %4llu/%4llu GB %s %s\n", i, s[i].kind, s[i].mount, s[i].fstype,
                                     (unsigned long long)(s[i].avail / GB), (unsigned long long)(s[i].total / GB),
                                     s[i].writable ? "rw" : "ro", s[i].reason);
  CHECK(c >= 5 && !strcmp(s[0].kind, "internal") && !strcmp(s[1].mount, "/mnt/usb0") && !strcmp(s[2].mount, "/mnt/usb1") &&
            !strcmp(s[3].mount, "/mnt/usb4") && !strcmp(s[4].mount, "/mnt/ext0"),
        "order: internal, usb0, usb1, usb4, ext0");
  CHECK(!find(s, c, "/mnt/usb2"), "duplicate device hidden");
  CHECK(!find(s, c, "/mnt/usb3"), "nullfs alias hidden");
  CHECK(!find(s, c, "/mnt/usb5"), "tmpfs (fake 2 MB) hidden");
  CHECK(!find(s, c, "/mnt/usb1/games"), "sub-folder mount hidden");
  CHECK(!find(s, c, "/mnt/usb6"), "same filesystem id hidden");
  const e2a_storage *ro = find(s, c, "/mnt/usb4");
  CHECK(ro && !ro->writable && !strcmp(ro->reason, "read_only") && !strcmp(ro->fstype, "FAT32"), "read-only stick shown, not writable");
  const e2a_storage *ext = find(s, c, "/mnt/ext0");
  CHECK(ext && !strcmp(ext->kind, "extended") && ext->index == 0, "M.2 / extended storage");
}

static void test_full_and_probe(void) {
  /* real directories: the probe creates <mount>/homebrew and proves write access */
  char root[256];
  snprintf(root, sizeof root, "%s/e2a-unit-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
  char mnt[300];
  snprintf(mnt, sizeof mnt, "%s/mnt/usb0", root);
  e2a_mkdir_p(mnt);
  setenv("E2A_FAKE_ROOT", root, 1);
  e2a_mnt v[2];
  v[0] = M(mnt, "/dev/da0s1", "exfatfs", 32 * GB, 100 * 1000000ull, 30, false);   /* 100 MB left */
  e2a_storage s[4];
  int c = storage_build(v, 1, NULL, s, 4, true);
  CHECK(c == 1, "probe: one storage, got %d", c);
  char hb[400];
  snprintf(hb, sizeof hb, "%s/homebrew", mnt);
  CHECK(c == 1 && s[0].writable, "probe: writable");
  CHECK(c == 1 && !strcmp(s[0].reason, "full"), "probe: nearly full drive flagged (%s)", c ? s[0].reason : "");
  CHECK(e2a_is_dir(hb), "probe: homebrew folder created on the real mount");
  /* an unmounted mount point (not in the table) is never touched */
  char other[300];
  snprintf(other, sizeof other, "%s/mnt/usb1", root);
  e2a_mkdir_p(other);
  c = storage_build(v, 0, NULL, s, 4, true);
  snprintf(hb, sizeof hb, "%s/homebrew", other);
  CHECK(c == 0 && !e2a_is_dir(hb), "unmounted mount point: not listed, nothing written");
  setenv("E2A_FAKE_ROOT", "", 1);
}

static void write_file(const char *path, const char *a, const char *b) {
  FILE *f = fopen(path, "wb");
  if (!f) return;
  for (int i = 0; i < 300000; i++) fputc((i * 7) & 0x7f, f);   /* filler, no import names */
  if (a) fputs(a, f);
  for (int i = 0; i < 1000; i++) fputc(0, f);
  if (b) fputs(b, f);
  fclose(f);
}

static void test_ampr_compat(void) {
  char root[256], p[400], why[200];
  snprintf(root, sizeof root, "%s/e2a-compat-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
  snprintf(p, sizeof p, "%s/sce_module", root);
  e2a_mkdir_p(p);
  snprintf(p, sizeof p, "%s/eboot.bin", root);
  write_file(p, "libkernel", "libSceAmpr");
  CHECK(e2a_ampr_compat(root, why, sizeof why) == 1, "eboot importing libSceAmpr: compatible (%s)", why);
  write_file(p, "libkernel", "libSceGnmDriver");
  CHECK(e2a_ampr_compat(root, why, sizeof why) == 0, "plain eboot without libSceAmpr: not compatible (%s)", why);
  snprintf(p, sizeof p, "%s/sce_module/libgame.prx", root);
  write_file(p, "libSceAmpr", NULL);
  CHECK(e2a_ampr_compat(root, why, sizeof why) == 1, "game module importing libSceAmpr: compatible (%s)", why);
  unlink(p);
  snprintf(p, sizeof p, "%s/eboot.bin", root);
  write_file(p, NULL, NULL);
  CHECK(e2a_ampr_compat(root, why, sizeof why) == -1, "encrypted eboot: unknown (%s)", why);
}

int main(void) {
  setenv("E2A_FAKE_ROOT", "", 1);
  test_no_usb();
  test_one_usb();
  test_several_usb();
  test_full_and_probe();
  test_ampr_compat();
  if (g_fail) { printf("%d check(s) failed\n", g_fail); return 1; }
  printf("all unit tests passed\n");
  return 0;
}
