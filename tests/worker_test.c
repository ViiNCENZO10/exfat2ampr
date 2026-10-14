/* SPDX-License-Identifier: GPL-3.0-or-later
 * Actual internal worker pool: every block must be processed when game mode changes worker count. */
#include "../src/ampr_pack.c"
#include <assert.h>

void activity_throttle(uint64_t bytes) { (void)bytes; }
static int active = 2;
int activity_workers(int requested) { return requested < active ? requested : active; }

int main(void) {
  packer *P = calloc(1, sizeof *P);
  assert(P);
  e2a_pack_opts options = {.workers = 6, .level = 1};
  P->o = &options; P->block = 16384; P->bound = LZ4_compressBound(P->block);
  enum { N = 19 };
  uint8_t *raw = malloc(N * P->block);
  uint32_t sizes[N];
  assert(raw);
  for (int i = 0; i < N; i++) { memset(raw + i * P->block, i + 1, P->block); sizes[i] = P->block; }
  P->cur_raw = raw; P->cur_len = sizes;
  P->comp = malloc((size_t)N * P->bound);
  P->comp_len = calloc(N, sizeof(int)); P->raw_crc = calloc(N, sizeof(uint32_t));
  assert(P->comp && P->comp_len && P->raw_crc);
  pool_init(P);
  for (int pass = 0; pass < 30; pass++) {
    active = (pass % 3 == 0) ? 2 : (pass % 3 == 1) ? 6 : 1;
    memset(P->raw_crc, 0, N * sizeof(uint32_t));
    assert(compress_batch(P, N) == 0);
    for (int i = 0; i < N; i++) assert(P->raw_crc[i] == e2a_crc32(0, raw + i * P->block, P->block));
  }
  assert(P->workers_started == 6);
  pool_destroy(P);
  free(raw); free(P->comp); free(P->comp_len); free(P->raw_crc); free(P);
  puts("persistent workers and reduced-worker coverage: OK");
  return 0;
}
