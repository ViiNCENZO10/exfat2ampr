/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * Host (Linux) command line around the same job engine, used by the CI to validate the
 * produced packs with drakmor/ampr_emu's Python tools and to exercise pause / resume.
 *
 *   exfat2ampr-host [--level N] [--workers N] [--no-verify] [--no-dedup] [--pause-after-mb N] SRC DEST_BASE [NAME]
 *   exfat2ampr-host --resume DEST
 *   exfat2ampr-host --trace-profile OUT.toml JOURNAL INDEX [JOURNAL INDEX ...]
 *   exfat2ampr-host --repair GAME_DIR   (the engine's repair of a converted folder; exit 0 nothing to do or fixed)
 *
 * exit codes: 0 done, 1 error, 3 paused
 */
#include "common.h"
#include "job.h"
#include "ampr_pack.h"
#include "trace.h"
#include "verify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int wait_job(void) {
  job_status s;
  int last_pct = -1;
  char last_phase[64] = "";
  for (;;) {
    usleep(200 * 1000);
    job_get_status(&s);
    if (s.state == JOB_RUNNING || (s.state == JOB_IDLE && s.queue_len > 0)) {
      if (s.percent != last_pct || strcmp(s.phase, last_phase) != 0) {
        last_pct = s.percent;
        snprintf(last_phase, sizeof last_phase, "%s", s.phase);
        fprintf(stderr, "[%s] %3d%% %s\n", s.phase, s.percent, s.message);
      }
      continue;
    }
    if (s.state == JOB_IDLE) continue;
    break;
  }
  printf("{\"state\":%d,\"phase\":\"%s\",\"message\":\"%s\",\"dest\":\"%s\",\"source_bytes\":%llu,\"packed_bytes\":%llu,\"shared_bytes\":%llu}\n",
         s.state, s.phase, s.message, s.dest, (unsigned long long)s.source_bytes, (unsigned long long)s.packed_bytes,
         (unsigned long long)s.shared_bytes);
  if (s.state == JOB_DONE) return 0;
  if (s.state == JOB_PAUSED) return 3;
  return 1;
}

int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "--verify") == 0) {
    e2a_pack_opts o = {0};
    e2a_verify_stats v;
    o.workers = 4;
    return e2a_verify(argv[2], &o, &v) == E2A_PACK_OK ? 0 : 1;
  }
  if (argc == 3 && strcmp(argv[1], "--repair") == 0) {
    char report[400];
    int rc = e2a_repair_converted(argv[2], report, sizeof report);
    printf("repair: %d %s\n", rc, report);
    return rc < 0 ? 1 : 0;
  }
  if (argc >= 5 && strcmp(argv[1], "--trace-profile") == 0 && (argc - 3) % 2 == 0) {
    int pairs = (argc - 3) / 2;
    const char *j[64], *x[64];
    if (pairs > 64) return 2;
    for (int i = 0; i < pairs; i++) { j[i] = argv[3 + 2 * i]; x[i] = argv[4 + 2 * i]; }
    char summary[1024];
    int rc = trace_build_profile(j, x, pairs, argv[2], summary, sizeof summary);
    printf("%s\n", summary);
    return rc == 0 ? 0 : 1;
  }
  job_request r;
  memset(&r, 0, sizeof r);
  r.level = 9; r.workers = 4; r.clean_leftovers = true; r.verify = true; r.dedup = true;
  const char *resume = NULL, *src = NULL, *base = NULL, *name = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--level") == 0 && i + 1 < argc) r.level = atoi(argv[++i]);
    else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) r.workers = atoi(argv[++i]);
    else if (strcmp(argv[i], "--remove-source") == 0) r.remove_source = true;
    else if (strcmp(argv[i], "--no-verify") == 0) r.verify = false;
    else if (strcmp(argv[i], "--no-dedup") == 0) r.dedup = false;
    else if (strcmp(argv[i], "--overwrite") == 0) r.overwrite = true;
    else if (strcmp(argv[i], "--pause-after-mb") == 0 && i + 1 < argc) r.test_pause_after = (uint64_t)atoll(argv[++i]) << 20;
    else if (strcmp(argv[i], "--resume") == 0 && i + 1 < argc) resume = argv[++i];
    else if (!src) src = argv[i];
    else if (!base) base = argv[i];
    else if (!name) name = argv[i];
    else { fprintf(stderr, "unexpected argument %s\n", argv[i]); return 2; }
  }
  if (job_init() != 0) { fprintf(stderr, "cannot start scheduler\n"); return 1; }
  if (resume) {
    if (job_resume(resume) != 0) { fprintf(stderr, "cannot resume %s\n", resume); return 1; }
    return wait_job();
  }
  if (!src || !base) { fprintf(stderr, "usage: exfat2ampr-host [options] SRC DEST_BASE [NAME] | --resume DEST\n"); return 2; }
  snprintf(r.source, sizeof r.source, "%s", src);
  snprintf(r.dest_base, sizeof r.dest_base, "%s", base);
  if (name) snprintf(r.name, sizeof r.name, "%s", name);
  if (job_enqueue(&r) < 0) { fprintf(stderr, "queue full\n"); return 1; }
  return wait_job();
}
