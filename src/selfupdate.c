/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE. */
#include "selfupdate.h"

#include "activity.h"
#include "smp.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define UPDATE_DIR E2A_STATE_DIR "/update"
#define UPDATE_PART UPDATE_DIR "/" E2A_APP_TITLE_ID ".ffpkg.part"
#define UPDATE_MAX (2ULL << 30)   /* a package is ~150 MB; never more than 2 GiB */

static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_install_mx = PTHREAD_MUTEX_INITIALIZER;   /* a new download never truncates a package being installed */
static int g_fd = -1;
static uint64_t g_size, g_got;
static char g_sha[65];

int selfupdate_begin(uint64_t size, const char *sha256_hex, char *err, size_t errcap) {
  bool hex = sha256_hex && strlen(sha256_hex) == 64;
  for (int i = 0; hex && i < 64; i++) hex = (sha256_hex[i] >= '0' && sha256_hex[i] <= '9') || (sha256_hex[i] >= 'a' && sha256_hex[i] <= 'f');
  if (!size || size > UPDATE_MAX || !hex) { snprintf(err, errcap, "bad size or sha256"); return -1; }
  pthread_mutex_lock(&g_install_mx);
  pthread_mutex_lock(&g_mx);
  if (g_fd >= 0) close(g_fd);
  mkdir(E2A_STATE_DIR, 0777);
  mkdir(UPDATE_DIR, 0777);
  g_fd = open(UPDATE_PART, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  int rc = 0;
  if (g_fd < 0) { snprintf(err, errcap, "cannot write %s: %s", UPDATE_PART, strerror(errno)); rc = -1; }
  g_size = size;
  g_got = 0;
  snprintf(g_sha, sizeof g_sha, "%s", sha256_hex);
  pthread_mutex_unlock(&g_mx);
  pthread_mutex_unlock(&g_install_mx);
  if (rc == 0) e2a_log("app update: receiving %llu bytes", (unsigned long long)size);
  return rc;
}

int selfupdate_chunk(uint64_t off, const void *data, size_t len, char *err, size_t errcap) {
  pthread_mutex_lock(&g_mx);
  int rc = 0;
  if (g_fd < 0) { snprintf(err, errcap, "no update in progress"); rc = -1; }
  else if (off != g_got || off > g_size || len > g_size - off) { snprintf(err, errcap, "piece out of order (%llu, expected %llu)", (unsigned long long)off, (unsigned long long)g_got); rc = -1; }
  else if (e2a_write_all(g_fd, data, len) != 0) { snprintf(err, errcap, "write failed: %s", strerror(errno)); rc = -1; }
  else g_got += len;
  pthread_mutex_unlock(&g_mx);
  return rc;
}

/* where ShadowMountPlus keeps the app's package: its own entry, else a homebrew folder, else the internal one */
static void app_package(char *out, size_t cap) {
  smp_game *games = calloc(256, sizeof *games);
  int n = games ? smp_list_games(games, 256) : -1;
  for (int i = 0; i < n; i++)
    if (!strcmp(games[i].title_id, E2A_APP_TITLE_ID) && !strcmp(games[i].source_type, "image") && e2a_is_file(games[i].path)) {
      snprintf(out, cap, "%s", games[i].path);
      free(games);
      return;
    }
  free(games);
  static const char *roots[] = {E2A_HOMEBREW_DIR, "/mnt/usb0/homebrew", "/mnt/usb1/homebrew", "/mnt/usb2/homebrew", "/mnt/usb3/homebrew",
                                "/mnt/ext0/homebrew", "/mnt/ext1/homebrew", NULL};
  for (int r = 0; roots[r]; r++) {
    snprintf(out, cap, "%s/%s.ffpkg", roots[r], E2A_APP_TITLE_ID);
    if (e2a_is_file(out)) return;
  }
  snprintf(out, cap, "%s/%s.ffpkg", E2A_HOMEBREW_DIR, E2A_APP_TITLE_ID);
}

static int install_package(char *where, size_t wcap, char *err, size_t errcap) {
  pthread_mutex_lock(&g_mx);
  int fd = g_fd;
  g_fd = -1;
  uint64_t size = g_size, got = g_got;
  char sha[65];
  memcpy(sha, g_sha, sizeof sha);
  pthread_mutex_unlock(&g_mx);
  if (fd < 0) { snprintf(err, errcap, "no update in progress"); return -1; }
  bool synced = fsync(fd) == 0;
  close(fd);
  char hex[65];
  if (!synced || got != size || (uint64_t)e2a_file_size(UPDATE_PART) != size) {
    snprintf(err, errcap, "incomplete download (%llu of %llu bytes)", (unsigned long long)got, (unsigned long long)size);
    unlink(UPDATE_PART);
    return -1;
  }
  if (e2a_sha256_file(UPDATE_PART, hex) != 0 || strcasecmp(hex, sha) != 0) {
    snprintf(err, errcap, "the download does not match the published SHA-256");
    unlink(UPDATE_PART);
    return -1;
  }
  char target[600], staged[640], old[640] = "";
  app_package(target, sizeof target);
  /* next to the target first (rename across file systems is a copy), then the swap */
  snprintf(staged, sizeof staged, "%s.new", target);
  if (rename(UPDATE_PART, staged) != 0) {
    if (e2a_copy_file(UPDATE_PART, staged, 0) != 0 || (uint64_t)e2a_file_size(staged) != size) {
      snprintf(err, errcap, "cannot write next to %s: %s", target, strerror(errno));
      unlink(staged);
      return -1;
    }
    unlink(UPDATE_PART);
  }
  /* checked again where it lies now: a copy to another drive can go wrong */
  if ((uint64_t)e2a_file_size(staged) != size || e2a_sha256_file(staged, hex) != 0 || strcasecmp(hex, sha) != 0) {
    snprintf(err, errcap, "the package next to %s does not match the published SHA-256", target);
    unlink(staged);
    return -1;
  }
  if (e2a_is_file(target)) {   /* the running version stays, renamed: nothing is deleted */
    snprintf(old, sizeof old, "%s.old", target);
    for (int k = 2; e2a_is_file(old) && k < 100; k++) snprintf(old, sizeof old, "%s.old%d", target, k);
    if (e2a_is_file(old)) {   /* every name taken: never overwrite a kept version */
      snprintf(err, errcap, "too many kept versions next to %s: move some .old files away", target);
      unlink(staged);
      return -1;
    }
    if (rename(target, old) != 0) {
      snprintf(err, errcap, "cannot set %s aside: %s", target, strerror(errno));
      unlink(staged);
      return -1;
    }
  }
  if (rename(staged, target) != 0) {
    int saved = errno;   /* the app must never be left without its package: the previous one goes back */
    if (old[0] && rename(old, target) != 0)
      snprintf(err, errcap, "install failed (%s); put %s back by hand", strerror(saved), old);
    else snprintf(err, errcap, "cannot put the new package in place: %s; the previous one is back", strerror(saved));
    return -1;
  }
  snprintf(where, wcap, "%s", target);
  e2a_log("app update installed: %s (%llu bytes); restart the console to run it", target, (unsigned long long)size);
  return 0;
}

int selfupdate_install(char *where, size_t wcap, char *err, size_t errcap) {
  pthread_mutex_lock(&g_install_mx);
  int rc = install_package(where, wcap, err, errcap);
  pthread_mutex_unlock(&g_install_mx);
  return rc;
}
