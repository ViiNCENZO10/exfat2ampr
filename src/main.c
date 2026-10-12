/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * exfat2ampr payload entry point.
 *
 *   exfat2ampr.elf            start the web UI on port 6972
 *   exfat2ampr.elf --auto     also queue every game found under /mnt/usb*\/homebrew
 *                             that is not yet in /data/homebrew
 */
#include "common.h"
#include "job.h"
#include "pair.h"
#include "activity.h"
#include "storage.h"
#include "profile.h"
#include "scan.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

int httpd_listen(int port);
int httpd_serve(int srv, int port);

/* Earlier versions could create <mount point>/homebrew on the system partition when no drive was
 * mounted there; remove such empty leftovers (only on unmounted mount points, only when empty). */
static void clean_stray_mount_dirs(void) {
  e2a_storage st[E2A_MAX_STORAGES];
  int n = storage_scan(st, E2A_MAX_STORAGES, false);
  static const char *const kinds[] = {"usb", "ext"};
  for (int k = 0; k < 2; k++)
    for (int i = 0; i < 8; i++) {
      char mnt[32], hb[48];
      snprintf(mnt, sizeof mnt, "/mnt/%s%d", kinds[k], i);
      bool mounted = false;
      for (int s = 0; s < n; s++) mounted |= !strcmp(st[s].mount, mnt);
      snprintf(hb, sizeof hb, "%s/homebrew", mnt);
      if (!mounted && e2a_is_dir(hb) && rmdir(hb) == 0) e2a_log("removed stray %s (no drive mounted there)", hb);
    }
}

static int count_resumable(void) {
  char *json = malloc(64 * 1024);
  if (!json) return 0;
  int n = 0;
  if (job_list_resumable(json, 64 * 1024) == 0) for (const char *p = json; (p = strstr(p, "\"dest\"")) != NULL; p++) n++;
  free(json);
  return n;
}

static void auto_queue(void) {
  static const char *roots[] = {"/mnt/usb0/homebrew", "/mnt/usb1/homebrew", "/mnt/usb2/homebrew", "/mnt/usb3/homebrew", NULL};
  int queued = 0;
  for (int r = 0; roots[r]; r++) {
    DIR *d = opendir(roots[r]);
    if (!d) continue;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
      if (de->d_name[0] == '.') continue;
      char path[600], dest[600];
      snprintf(path, sizeof path, "%s/%s", roots[r], de->d_name);
      e2a_game g;
      if (!e2a_is_dir(path) || e2a_read_game(path, &g) != 0) continue;
      snprintf(dest, sizeof dest, "%s/%s", E2A_HOMEBREW_DIR, g.title_id);
      if (e2a_is_dir(dest)) continue;
      job_request req;
      memset(&req, 0, sizeof req);
      snprintf(req.source, sizeof req.source, "%s", path);
      snprintf(req.dest_base, sizeof req.dest_base, "%s", E2A_HOMEBREW_DIR);
      req.level = 9; req.workers = 6; req.clean_leftovers = true; req.verify = true; req.dedup = true;
      if (job_enqueue(&req) >= 0) queued++;
    }
    closedir(d);
  }
  if (queued) e2a_notify("exfat2ampr: %d game(s) queued (auto mode)", queued);
}

int main(int argc, char **argv) {
  bool auto_mode = false;
  for (int i = 1; i < argc; i++) if (strcmp(argv[i], "--auto") == 0) auto_mode = true;

  e2a_mkdir_p(E2A_STATE_DIR);
  e2a_mkdir_p(E2A_HOMEBREW_DIR);
  e2a_log("exfat2ampr %s starting%s", E2A_VERSION, auto_mode ? " (auto mode)" : "");

  /* own the port first: a second launch must not start a scheduler or queue anything */
  int srv = httpd_listen(E2A_HTTP_PORT);
  if (srv < 0) {
    e2a_notify("exfat2ampr: port %d busy, already running?", E2A_HTTP_PORT);
    return 1;
  }

  /* leave the system UI responsive while compressing */
  setpriority(PRIO_PROCESS, 0, 10);

  if (job_init() != 0) { e2a_notify("exfat2ampr: cannot start the job scheduler"); return 1; }
  clean_stray_mount_dirs();
  e2a_copy_hook = activity_throttle;   /* loose-file copies are paced too while a game runs */
  activity_init();
  e2a_profile_import_usb();

  pair_init();
  char ip[64];
  pair_local_ip(ip, sizeof ip);
  int resumable = count_resumable();
  /* the code pairs a PC or phone with the web page (the home-screen app shows it too) */
  if (resumable)
    e2a_notify("exfat2ampr %s\nhttp://%s:%d/  code %s\n%d interrupted conversion(s) can be resumed", E2A_VERSION, ip, E2A_HTTP_PORT,
               pair_pin(), resumable);
  else e2a_notify("exfat2ampr %s\nhttp://%s:%d/  code %s", E2A_VERSION, ip, E2A_HTTP_PORT, pair_pin());

  if (auto_mode) auto_queue();

  httpd_serve(srv, E2A_HTTP_PORT);
  /* the server loop only ends on a fatal accept error: never exit while a conversion is running */
  while (job_busy()) sleep(1);
  return 0;
}
