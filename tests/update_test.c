/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * The app's self-update: a corrupt package is refused and the old one kept, an extreme offset is refused,
 * a good package is installed and the previous one kept as .old. ShadowMountPlus is simulated. */
#include "activity.h"
#include "selfupdate.h"
#include "smp.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int smp_list_games(smp_game *out, int max) { (void)out; (void)max; return 0; }

int main(void) {
  char err[1000], where[700], sha[65], actual[65];
  const char *target = E2A_HOMEBREW_DIR "/" E2A_APP_TITLE_ID ".ffpkg";
  assert(e2a_mkdir_p(E2A_HOMEBREW_DIR) == 0);
  assert(e2a_mkdir_p(E2A_STATE_DIR) == 0);
  assert(e2a_write_file_atomic("payload", "new", 3) == 0);
  assert(e2a_sha256_file("payload", sha) == 0);
  assert(e2a_write_file_atomic(target, "old", 3) == 0);
  assert(selfupdate_begin(3, sha, err, sizeof err) == 0);
  assert(selfupdate_chunk(UINT64_MAX, "new", 3, err, sizeof err) == -1);
  assert(selfupdate_chunk(0, "bad", 3, err, sizeof err) == 0);
  assert(selfupdate_install(where, sizeof where, err, sizeof err) == -1);
  assert(e2a_sha256_file(target, actual) == 0 && strcmp(actual, sha) != 0);   /* the old one is still there */
  assert(selfupdate_begin(3, sha, err, sizeof err) == 0);
  assert(selfupdate_chunk(0, "new", 3, err, sizeof err) == 0);
  assert(selfupdate_install(where, sizeof where, err, sizeof err) == 0);
  assert(strcmp(where, target) == 0);
  assert(e2a_sha256_file(target, actual) == 0 && strcmp(actual, sha) == 0);
  assert(e2a_is_file(E2A_HOMEBREW_DIR "/" E2A_APP_TITLE_ID ".ffpkg.old"));
  puts("update integrity: OK");
  return 0;
}
