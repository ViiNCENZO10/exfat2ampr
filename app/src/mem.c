/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE. */
#define E2A_MEM_IMPL
#include "mem.h"
#include "platform.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HDR 16                              /* every block starts with one, payloads stay 16-byte aligned */
#define ARENA_MIN ((size_t)32 << 10)        /* from 32 KB: cut from the arenas */
#define DIRECT_MIN ((size_t)256 << 20)      /* from 256 MB: straight to the system */
#define ARENA_SIZE ((size_t)32 << 20)       /* a size the system was seen giving */
#define MAX_ARENAS 24
#define USED ((size_t)1)
#define SYS_MAGIC ((size_t)0xE2A0A110C5A9E1D5ull)

/* an arena block: its whole size (header included, | USED) and the previous block's, for merging both ways */
typedef struct { size_t size, prev; } blk;
/* a system block: its payload size, and a mark telling it from memory the system handed out directly */
typedef struct { size_t size, magic; } sys_hdr;

static uint8_t *g_arena[MAX_ARENAS];
static size_t g_arena_size[MAX_ARENAS];
static int g_narena;
static unsigned g_arena_fails, g_sys_fails;
static size_t g_last_fail;
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;

/* the platform's large blocks (platform_ps5.c: direct memory); none elsewhere (host build, unit tests) */
__attribute__((weak)) void *plat_big_alloc(size_t len) {
  (void)len;
  return NULL;
}

static size_t round16(size_t n) { return (n + 15) & ~(size_t)15; }
static blk *next_blk(blk *b) { return (blk *)((uint8_t *)b + (b->size & ~USED)); }

static int arena_of(const void *p) {
  for (int i = 0; i < g_narena; i++)
    if ((const uint8_t *)p >= g_arena[i] && (const uint8_t *)p < g_arena[i] + g_arena_size[i]) return i;
  return -1;
}

/* a new arena: the largest size the system gives now, by malloc, else aligned allocation */
static bool arena_add(size_t need) {
  if (g_narena >= MAX_ARENAS) return false;
  static const size_t SIZES[] = {(size_t)32 << 20, (size_t)16 << 20, (size_t)8 << 20, (size_t)4 << 20};
  uint8_t *a = NULL;
  size_t size = (size_t)64 << 20;
  while (size < need + 2 * HDR) size *= 2;
  a = plat_big_alloc(size);   /* the console's direct memory first: the system heap is small and soon full */
  for (int k = 0; k < 8 && !a; k++) {
    size = SIZES[k % 4];
    if (size < need + 2 * HDR) continue;
    if (k < 4) a = malloc(size);   /* the system's own malloc here */
    else {
      void *p = NULL;
      if (posix_memalign(&p, 4096, size) == 0) a = p;
    }
  }
  if (!a) { g_arena_fails++; return false; }
  blk *first = (blk *)a, *end = (blk *)(a + size - HDR);
  first->size = size - HDR;            /* free */
  first->prev = 0;
  end->size = USED;                    /* the end mark: size 0, used */
  end->prev = first->size;
  g_arena_size[g_narena] = size;
  g_arena[g_narena++] = a;
  return true;
}

int mem_stats(char *out, size_t cap) {
  pthread_mutex_lock(&g_mx);
  size_t total = 0;
  for (int i = 0; i < g_narena; i++) total += g_arena_size[i];
  int n = snprintf(out, cap, "%d arena(s), %zu MB; arena refused %u time(s), system refused %u time(s) (last %zu bytes)",
                   g_narena, total >> 20, g_arena_fails, g_sys_fails, g_last_fail);
  pthread_mutex_unlock(&g_mx);
  return n;
}

static void *arena_alloc(size_t n) {
  size_t need = round16(n) + HDR;
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < g_narena; i++)
      for (blk *b = (blk *)g_arena[i]; (b->size & ~USED) != 0; b = next_blk(b)) {
        size_t sz = b->size & ~USED;
        if ((b->size & USED) || sz < need) continue;
        if (sz - need >= 64) {   /* split: the rest stays free */
          blk *r = (blk *)((uint8_t *)b + need);
          r->size = sz - need;
          r->prev = need;
          next_blk(r)->prev = r->size;
          sz = need;
        }
        b->size = sz | USED;
        return (uint8_t *)b + HDR;
      }
    if (pass == 0 && !arena_add(need)) return NULL;
  }
  return NULL;
}

static void arena_free(void *p) {
  blk *b = (blk *)((uint8_t *)p - HDR);
  size_t sz = b->size & ~USED;
  b->size = sz;
  blk *n = next_blk(b);
  if (!(n->size & USED)) {   /* merge with the next free block */
    sz += n->size;
    b->size = sz;
    next_blk(b)->prev = sz;
  }
  if (b->prev) {             /* and with the previous one */
    blk *pv = (blk *)((uint8_t *)b - b->prev);
    if (!(pv->size & USED)) {
      size_t s2 = pv->size + sz;
      pv->size = s2;
      next_blk(pv)->prev = s2;
    }
  }
}

static void *sys_alloc(size_t n) {
  sys_hdr *h = malloc(n + HDR);
  if (!h) { g_sys_fails++; g_last_fail = n; return NULL; }
  h->size = n;
  h->magic = SYS_MAGIC;
  return (uint8_t *)h + HDR;
}

void *mem_alloc(size_t n) {
  if (n == 0) n = 1;
  if (n >= ARENA_MIN && n < DIRECT_MIN) {
    pthread_mutex_lock(&g_mx);
    void *p = arena_alloc(n);
    pthread_mutex_unlock(&g_mx);
    if (p) return p;
  }
  return sys_alloc(n);
}

void *mem_calloc(size_t count, size_t size) {
  if (size && count > (size_t)-1 / size) return NULL;
  void *p = mem_alloc(count * size);
  if (p) memset(p, 0, count * size);
  return p;
}

/* the usable size of a block from here; 0 for memory the system handed out directly */
static size_t block_size(void *p) {
  pthread_mutex_lock(&g_mx);
  int a = arena_of(p);
  size_t sz = a >= 0 ? (((blk *)((uint8_t *)p - HDR))->size & ~USED) - HDR : 0;
  pthread_mutex_unlock(&g_mx);
  if (a >= 0) return sz;
  sys_hdr *h = (sys_hdr *)((uint8_t *)p - HDR);
  return h->magic == SYS_MAGIC ? h->size : 0;
}

void mem_free(void *p) {
  if (!p) return;
  pthread_mutex_lock(&g_mx);
  if (arena_of(p) >= 0) {
    arena_free(p);
    pthread_mutex_unlock(&g_mx);
    return;
  }
  pthread_mutex_unlock(&g_mx);
  sys_hdr *h = (sys_hdr *)((uint8_t *)p - HDR);
  if (h->magic == SYS_MAGIC) {
    h->magic = 0;
    free(h);
  } else free(p);   /* handed out by the system itself (strdup in the host build) */
}

void *mem_realloc(void *p, size_t n) {
  if (!p) return mem_alloc(n);
  if (n == 0) { mem_free(p); return NULL; }
  size_t old = block_size(p);
  if (old == 0) return realloc(p, n);   /* not ours: the system's own */
  if (old >= n) return p;
  void *q = mem_alloc(n);   /* never the system's realloc: it refuses growing blocks with memory to spare */
  if (!q) return NULL;
  memcpy(q, p, old);
  mem_free(p);
  return q;
}
