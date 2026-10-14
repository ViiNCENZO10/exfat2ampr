/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * The app's memory. On the console, the system allocator refuses medium blocks (seen: 768 KB and 1 MB refused
 * while 4 MB and 32 MB were given at the same moment), which left covers undecoded and once the engine's ELF
 * unread. Blocks from 32 KB to 16 MB are cut from 32 MB arenas the system does give; the others go to it.
 * Included last in every file that allocates: malloc, calloc, realloc and free then go through here. */
#ifndef E2A_MEM_H
#define E2A_MEM_H

#include <stddef.h>
#include <stdlib.h>

void *mem_alloc(size_t n);
void *mem_calloc(size_t count, size_t size);
void *mem_realloc(void *p, size_t n);
void mem_free(void *p);
/* one line for the log: arenas held and refusals seen */
int mem_stats(char *out, size_t cap);

#ifndef E2A_MEM_IMPL
#define malloc(n) mem_alloc(n)
#define calloc(c, s) mem_calloc(c, s)
#define realloc(p, n) mem_realloc(p, n)
#define free(p) mem_free(p)
#endif

#endif
