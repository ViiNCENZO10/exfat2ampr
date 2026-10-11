/* SPDX-License-Identifier: GPL-3.0-or-later
 * Trace-driven profiles: port of drakmor/ampr_emu tools/ampr_pack_profile.py (generate, exact
 * pattern mode) and of the AMPRCMD1 decoder of tools/parse_ampr_command_log.py.
 * Not ported: the index-size budget pass, lanes/groups (single group here), content sampling. */
#include "trace.h"
#include "ampr_format.h"
#include "scan.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern unsigned char libSceAmpr_debug_sprx[];
extern unsigned int libSceAmpr_debug_sprx_len;

#define KIB 1024ull
#define MIB (1024ull * 1024)
#define NBS 7
static const uint32_t BS[NBS] = {16 * 1024, 32 * 1024, 64 * 1024, 128 * 1024, 256 * 1024, 512 * 1024, 1024 * 1024};
enum { L_MIXED = 0, L_RANDOM = 1, L_STREAMING = 2 };
enum { A_COMPRESS = 0, A_STORE = 1, A_LOOSE = 2 };

/* ------------------------------------------------------------------ data -- */
typedef struct { uint64_t off; uint32_t len; uint16_t trace; } ev_t;

typedef struct {
  char *rel;                 /* path relative to /app0 */
  uint64_t size;             /* max over traces */
  ev_t *ev; size_t n, cap;
  /* metrics */
  uint64_t requested;
  double p50, p90, tiny4, small16, small64, large256;
  double exact, near, random, repeat64;
  double score[NBS];
  /* recommendation */
  int action, layout, block_idx;
  bool hot, recommended;
  uint32_t ordinal;
} file_t;

typedef struct { uint32_t file; uint64_t off; uint32_t len; } gev_t;   /* global ordered event */

typedef struct {
  file_t *f; size_t n, cap;
  uint32_t *tab; size_t tcap;        /* rel -> 1-based file index */
  gev_t *g; size_t gn, gcap;
  uint64_t records, decode_errors, dropped, gaps, oom;
  bool sim_oom;
  int traces;
} model_t;

static uint64_t hstr(const char *s) {
  uint64_t h = 1469598103934665603ull;
  for (; *s; s++) { h ^= (uint8_t)*s; h *= 1099511628211ull; }
  return h;
}

static int grow(void **p, size_t *cap, size_t need, size_t elem) {
  if (need <= *cap) return 0;
  size_t nc = *cap ? *cap * 2 : 256;
  while (nc < need) nc *= 2;
  void *np = realloc(*p, nc * elem);
  if (!np) return -1;
  *p = np; *cap = nc;
  return 0;
}

static int rehash(model_t *m, size_t cap) {
  uint32_t *t = calloc(cap, sizeof *t);
  if (!t) return -1;
  for (size_t i = 0; i < m->n; i++) {
    size_t pos = hstr(m->f[i].rel) & (cap - 1);
    while (t[pos]) pos = (pos + 1) & (cap - 1);
    t[pos] = (uint32_t)(i + 1);
  }
  free(m->tab);
  m->tab = t; m->tcap = cap;
  return 0;
}

static file_t *file_get(model_t *m, const char *rel, uint64_t size) {
  if (!m->tab && rehash(m, 4096)) return NULL;
  size_t pos = hstr(rel) & (m->tcap - 1);
  while (m->tab[pos]) {
    file_t *f = &m->f[m->tab[pos] - 1];
    if (strcmp(f->rel, rel) == 0) { if (size > f->size) f->size = size; return f; }
    pos = (pos + 1) & (m->tcap - 1);
  }
  if ((m->n + 1) * 2 > m->tcap) { if (rehash(m, m->tcap * 2)) return NULL; return file_get(m, rel, size); }
  if (grow((void **)&m->f, &m->cap, m->n + 1, sizeof *m->f)) return NULL;
  file_t *f = &m->f[m->n];
  memset(f, 0, sizeof *f);
  f->rel = strdup(rel);
  if (!f->rel) return NULL;
  f->size = size;
  f->ordinal = (uint32_t)m->n;
  m->tab[pos] = (uint32_t)(m->n + 1);
  m->n++;
  return f;
}

/* ----------------------------------------------------------------- index -- */
typedef struct { char **path; uint64_t *size; uint64_t n; } idx_t;

static void idx_free(idx_t *x) {
  for (uint64_t i = 0; i < x->n; i++) free(x->path[i]);
  free(x->path); free(x->size);
  memset(x, 0, sizeof *x);
}

static int idx_load(const char *path, idx_t *x) {
  memset(x, 0, sizeof *x);
  char *buf = NULL;
  size_t len = 0;
  int64_t fsz = e2a_file_size(path);
  if (fsz < (int64_t)sizeof(ampr_idx_header) || e2a_read_text(path, &buf, &len, (size_t)fsz) != 0) return -1;
  ampr_idx_header h;
  memcpy(&h, buf, sizeof h);
  if (memcmp(h.magic, AMPR_MAGIC_IDX, 8) != 0 || h.version != 3 || h.entry_size != 24 || h.entry_count > len / 24 ||
      h.path_bytes > len || 48 + h.entry_count * 24 + h.path_bytes > len) { free(buf); return -1; }
  x->path = calloc((size_t)h.entry_count + 1, sizeof *x->path);
  x->size = calloc((size_t)h.entry_count + 1, sizeof *x->size);
  if (!x->path || !x->size) { free(buf); idx_free(x); return -1; }
  const ampr_idx_entry *e = (const ampr_idx_entry *)(buf + 48);
  const char *blob = buf + 48 + h.entry_count * 24;
  for (uint64_t i = 0; i < h.entry_count; i++) {
    if ((uint64_t)e[i].path_at + e[i].path_len >= h.path_bytes) continue;
    const char *p = blob + e[i].path_at;
    if (e[i].path_len > 6 && strncasecmp(p, "/app0/", 6) == 0) p += 6;
    else if (e[i].path_len > 0 && p[0] == '/') p++;
    x->path[i] = strndup(p, e[i].path_len - (size_t)(p - (blob + e[i].path_at)));
    x->size[i] = e[i].size;
  }
  x->n = h.entry_count;
  free(buf);
  return 0;
}

/* -------------------------------------------------------- AMPRCMD1 decode -- */
typedef struct __attribute__((packed)) {
  char magic[8];
  uint16_t version, header_bytes;
  uint32_t record_bytes;
  uint64_t sequence, utc_ns, monotonic_ns, cookie, source_address, payload_hash;
  uint32_t payload_bytes, source_capacity, command_count, priority, domain, submit_mode, submit_type, flags;
} cmd_hdr;
_Static_assert(sizeof(cmd_hdr) == 96, "AMPRCMD1 header");

typedef struct { uint32_t prio; uint32_t file_id; uint64_t next; bool used; } gs_state;

static void emit(model_t *m, const idx_t *x, int trace, uint32_t file_id, uint64_t off, uint64_t len) {
  if (file_id == 0 || file_id > x->n || !x->path[file_id - 1]) { m->dropped++; return; }
  uint64_t size = x->size[file_id - 1];
  if (len == 0 || off >= size) { m->dropped++; return; }
  if (off + len > size) len = size - off;
  file_t *f = file_get(m, x->path[file_id - 1], size);
  if (!f || grow((void **)&f->ev, &f->cap, f->n + 1, sizeof *f->ev)) { m->oom++; return; }
  f->ev[f->n].off = off; f->ev[f->n].len = (uint32_t)(len > 0xFFFFFFFFu ? 0xFFFFFFFFu : len); f->ev[f->n].trace = (uint16_t)trace;
  f->n++;
  if (grow((void **)&m->g, &m->gcap, m->gn + 1, sizeof *m->g)) { m->oom++; return; }
  m->g[m->gn].file = f->ordinal; m->g[m->gn].off = off; m->g[m->gn].len = f->ev[f->n - 1].len;
  m->gn++;
}

static gs_state *gs_find(gs_state *st, int *nst, uint32_t prio, bool create) {
  for (int i = 0; i < *nst; i++) if (st[i].used && st[i].prio == prio) return &st[i];
  if (!create) return NULL;
  for (int i = 0; i < *nst; i++)
    if (!st[i].used) { st[i].prio = prio; st[i].used = true; return &st[i]; }
  if (*nst >= 64) return NULL;
  st[*nst].prio = prio; st[*nst].used = true;
  return &st[(*nst)++];
}

static int cmd_dwords(uint32_t w0) {
  uint32_t b = w0 & 0xFF, b12 = w0 & 0xFFF;
  if ((w0 & 0xFFFF000F) == 0x5452000F) return w0 == 0x5452300F ? 1 : (int)((w0 >> 8) & 0xF) + 1;
  if (b12 == 1032 || b12 == 1144) return 5;
  if (b12 == 0x22A) return 3;
  if (b12 == 557) return 3;
  if (b12 == 813) return 4;
  switch (b12) {  /* AMM */
    case 0x221: case 0x222: case 0x228: return 3;
    case 0x321: case 0x325: case 0x323: case 0x324: case 0x327: case 0x326: return 4;
    case 0x425: case 0x423: case 0x424: case 0x426: return 5;
    default: break;
  }
  if (w0 == 46 || w0 == 47) return 1;
  if (b == 1 || b == 2) return (int)((w0 >> 8) & 0xF) + 1;
  if (b == 5 || b == 117) return (int)((w0 >> 8) & 3) + 1;
  if (b == 6 || b == 118) return (int)((w0 >> 8) & 0xF) + 1;
  if (b == 40 || b == 43) return (int)((w0 >> 8) & 7) + 1;
  if (b == 41) return (int)((w0 >> 8) & 3) + 1;
  return (int)((w0 >> 8) & 0xF) + 1;
}

static int load_trace(model_t *m, const char *journal, const char *index, int trace) {
  idx_t x;
  if (idx_load(index, &x) != 0) { e2a_log("trace: bad index %s", index); return -1; }
  int fd = open(journal, O_RDONLY);
  if (fd < 0) { idx_free(&x); return -1; }
  gs_state st[64];
  int nst = 0;
  memset(st, 0, sizeof st);
  uint32_t *payload = NULL;
  size_t pcap = 0;
  uint64_t expect = 1;
  for (;;) {
    cmd_hdr h;
    size_t got = 0;
    if (e2a_read_full(fd, &h, sizeof h, &got) != 0 || got != sizeof h) break;
    if (memcmp(h.magic, "AMPRCMD1", 8) != 0 || h.version != 1 || h.header_bytes < 96 ||
        h.record_bytes != (uint32_t)h.header_bytes + h.payload_bytes) { e2a_log("trace: corrupt record in %s", journal); break; }
    if (h.header_bytes > 96 && lseek(fd, h.header_bytes - 96, SEEK_CUR) < 0) break;
    if (h.payload_bytes > (64u << 20)) break;
    if (grow((void **)&payload, &pcap, h.payload_bytes / 4 + 1, 4)) break;
    if (e2a_read_full(fd, payload, h.payload_bytes, &got) != 0 || got != h.payload_bytes) break;  /* truncated: stop */
    if (h.sequence != expect) m->gaps++;
    expect = h.sequence + 1;
    m->records++;
    if (h.domain != 1) continue;   /* APR only */
    size_t nw = h.payload_bytes / 4, i = 0;
    uint32_t cmds = 0;
    while (i < nw) {
      const uint32_t *w = payload + i;
      uint32_t w0 = w[0];
      int dw = cmd_dwords(w0);
      if (dw < 1 || i + (size_t)dw > nw) { m->decode_errors++; break; }
      uint32_t b = w0 & 0xFF, b12 = w0 & 0xFFF;
      if (b == 40 && dw >= 5 && dw <= 6) {                      /* AprReadFile */
        uint64_t len = (uint64_t)w[1] + 1, off = ((w0 >> 12) & 0x3FFFF) | (w[4] & 0xFFFC0000u);
        if (dw == 6) off |= (uint64_t)(w[5] & 0xFF) << 32;
        uint32_t fid = w[2] & 0x7FFFFFFF;
        gs_state *s = gs_find(st, &nst, h.priority, true);
        if (s) { s->file_id = fid; s->next = off + len; }
        emit(m, &x, trace, fid, off, len);
      } else if (b == 41 && dw >= 2 && dw <= 3) {               /* AprReadGather */
        uint64_t len = (uint64_t)w[1] + 1, off = (w0 >> 12) & 0x3FFFF;
        if (dw == 3) off |= (uint64_t)(w[2] & 0x3FFFFF) << 18;
        gs_state *s = gs_find(st, &nst, h.priority, false);
        if (s) { emit(m, &x, trace, s->file_id, off, len); s->next = off + len; }
      } else if (b12 == 0x22A) {                                 /* AprReadScatter */
        uint64_t len = (uint64_t)w[1] + 1;
        gs_state *s = gs_find(st, &nst, h.priority, false);
        if (s) { emit(m, &x, trace, s->file_id, s->next, len); s->next += len; }
      } else if (b == 43 && dw >= 4 && dw <= 5) {               /* AprReadGatherScatter */
        uint64_t len = (uint64_t)w[1] + 1, off = ((w0 >> 12) & 0x3FFFF) | (w[3] & 0xFFFC0000u);
        if (dw == 5) off |= (uint64_t)(w[4] & 0xFF) << 32;
        gs_state *s = gs_find(st, &nst, h.priority, false);
        if (s) { emit(m, &x, trace, s->file_id, off, len); s->next = off + len; }
      } else if (w0 == 47) {                                     /* AprResetGatherScatterState */
        gs_state *s = gs_find(st, &nst, h.priority, false);
        if (s) s->used = false;
      }
      i += (size_t)dw;
      cmds++;
    }
    if (h.command_count && cmds != h.command_count) m->decode_errors++;
  }
  free(payload);
  close(fd);
  idx_free(&x);
  m->traces++;
  return 0;
}

/* --------------------------------------------------------------- metrics -- */
static int cmp_u32(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }
static int cmp_u64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }

static double pct_sorted_u32(const uint32_t *v, size_t n, double p) {
  if (!n) return 0;
  if (n == 1) return v[0];
  double pos = (double)(n - 1) * p / 100.0;
  size_t lo = (size_t)floor(pos), hi = (size_t)ceil(pos);
  if (lo == hi) return v[lo];
  return v[lo] * (1.0 - (pos - (double)lo)) + v[hi] * (pos - (double)lo);
}
static double pct_sorted_u64(const uint64_t *v, size_t n, double p) {
  if (!n) return 0;
  if (n == 1) return (double)v[0];
  double pos = (double)(n - 1) * p / 100.0;
  size_t lo = (size_t)floor(pos), hi = (size_t)ceil(pos);
  if (lo == hi) return (double)v[lo];
  return (double)v[lo] * (1.0 - (pos - (double)lo)) + (double)v[hi] * (pos - (double)lo);
}

typedef struct { uint64_t a, b; } span;
static int cmp_span(const void *x, const void *y) { const span *p = x, *q = y; return p->a < q->a ? -1 : p->a > q->a; }
static uint64_t union_len(span *s, size_t n) {
  if (!n) return 0;
  qsort(s, n, sizeof *s, cmp_span);
  uint64_t tot = 0, a = s[0].a, b = s[0].b;
  for (size_t i = 1; i < n; i++) {
    if (s[i].a <= b) { if (s[i].b > b) b = s[i].b; }
    else { tot += b - a; a = s[i].a; b = s[i].b; }
  }
  return tot + (b - a);
}

static int classify(const file_t *f) {
  size_t n = f->n;
  double seq = f->exact + f->near;
  if (f->p50 <= 256 && n >= 64 && (f->repeat64 >= 0.10 || seq >= 0.70)) return L_RANDOM;
  if (f->p50 >= 64 * 1024 && seq >= 0.76 && f->random <= 0.20 && f->repeat64 <= 0.18) return L_STREAMING;
  if (f->p90 >= 256 * 1024 && f->large256 >= 0.65 && seq >= 0.86 && f->repeat64 <= 0.12) return L_STREAMING;
  if (f->tiny4 >= 0.50 || f->small16 >= 0.70 || (f->small64 >= 0.82 && f->random >= 0.30) || (f->repeat64 >= 0.28 && f->p50 <= 64 * 1024))
    return L_RANDOM;
  return L_MIXED;
}

static double block_score(const file_t *f, uint32_t B, int layout, double *repeat_out) {
  span *sp = malloc(f->n * sizeof *sp);
  if (!sp) return 1e9;
  uint64_t touched = 0, touches = 0;
  for (size_t i = 0; i < f->n; i++) {
    uint64_t first = f->ev[i].off / B, lastx = (f->ev[i].off + f->ev[i].len + B - 1) / B;
    uint64_t start = first * B, end = lastx * B < f->size ? lastx * B : f->size;
    touched += end > start ? end - start : 0;
    touches += lastx - first;
    sp[i].a = first; sp[i].b = lastx;
  }
  uint64_t unique = union_len(sp, f->n);
  free(sp);
  double amp = f->requested ? (double)touched / (double)f->requested : 1.0;
  double avg = f->n ? (double)touches / (double)f->n : 0.0;
  double repeat = touches ? fmax(0.0, 1.0 - (double)unique / (double)touches) : 0.0;
  if (repeat_out) *repeat_out = repeat;
  double meta = ceil((double)f->size / B) * 12.0;
  double la = log2(fmax(1.0, amp)), lo = log2(fmax(1.0, avg)), sl = log2((double)B / 65536.0), s;
  if (layout == L_RANDOM) {
    s = 3.8 * la + 0.12 * lo;
    if (B > 65536) s += 0.55 * log2((double)B / 65536.0);
    if (B < 16384) s += 10.0;
  } else if (layout == L_STREAMING) {
    s = 0.65 * la + 0.95 * lo;
    if (B < 131072) s += 0.35 * log2(131072.0 / B);
    if (B > 524288) s += 0.15 * log2((double)B / 524288.0);
  } else {
    s = 2.15 * la + 0.38 * lo + fabs(sl) * 0.06;
    if (B > 131072) s += 0.28 * log2((double)B / 131072.0);
    if (B > 65536) s += f->random * 0.42 * log2((double)B / 65536.0);
    if (B < 32768) s += 0.20 * log2(32768.0 / B);
  }
  return s + meta / fmax((double)f->size, 1.0) * 3.0;
}

static void file_metrics(file_t *f) {
  size_t n = f->n;
  uint32_t *lens = malloc((n ? n : 1) * sizeof *lens);
  if (!lens) return;
  uint64_t req = 0, t4 = 0, s16 = 0, s64 = 0, l256 = 0;
  for (size_t i = 0; i < n; i++) {
    uint32_t L = f->ev[i].len;
    lens[i] = L; req += L;
    if (L <= 4096) t4 += L;
    if (L <= 16384) s16 += L;
    if (L <= 65536) s64 += L;
    if (L >= 262144) l256 += L;
  }
  qsort(lens, n, sizeof *lens, cmp_u32);
  f->p50 = pct_sorted_u32(lens, n, 50); f->p90 = pct_sorted_u32(lens, n, 90);
  free(lens);
  f->requested = req;
  f->tiny4 = req ? (double)t4 / req : 0; f->small16 = req ? (double)s16 / req : 0;
  f->small64 = req ? (double)s64 / req : 0; f->large256 = req ? (double)l256 / req : 0;
  /* transitions between consecutive reads of this file within the same trace */
  uint64_t tb = 0, ex = 0, nr = 0, rnd = 0;
  for (size_t i = 1; i < n; i++) {
    const ev_t *p = &f->ev[i - 1], *c = &f->ev[i];
    if (p->trace != c->trace) continue;
    uint64_t pend = p->off + p->len, w = c->len;
    tb += w;
    if (c->off == pend) ex += w;
    else if (c->off >= pend && c->off - pend <= (uint64_t)fmax(65536.0, fmin((double)p->len, 524288.0))) nr += w;
    else if (c->off >= p->off && c->off < pend) { /* overlap */ }
    else rnd += w;   /* backward or far */
  }
  f->exact = tb ? (double)ex / tb : 0; f->near = tb ? (double)nr / tb : 0; f->random = tb ? (double)rnd / tb : 0;
  double rep = 0;
  (void)block_score(f, 65536, L_MIXED, &rep);
  f->repeat64 = rep;
}

static const char *STORE_EXT[] = {".bik", ".bk2", ".mp4", ".m4v", ".webm", ".m2v", ".mpg", ".mpeg", ".avi", ".wem", ".opus",
                                  ".ogg", ".mp3", ".aac", ".flac", NULL};
static int known_action(const char *rel) {
  const char *base = strrchr(rel, '/');
  base = base ? base + 1 : rel;
  const char *dot = strrchr(base, '.');
  if (strcasecmp(base, "eboot.bin") == 0 || strncasecmp(base, "ampr_emu.index", 14) == 0 ||
      strncasecmp(base, "ampr_assets.index", 17) == 0 || strcasecmp(base, "param.sfo") == 0 || strcasecmp(base, "nptitle.dat") == 0)
    return A_LOOSE;
  if (strncasecmp(rel, "sce_", 4) == 0 || strncasecmp(rel, "fakelib", 7) == 0) return A_LOOSE;
  if (dot && (strcasecmp(dot, ".prx") == 0 || strcasecmp(dot, ".sprx") == 0 || strcasecmp(dot, ".self") == 0 || strcasecmp(dot, ".elf") == 0))
    return A_LOOSE;
  for (int i = 0; dot && STORE_EXT[i]; i++) if (strcasecmp(dot, STORE_EXT[i]) == 0) return A_STORE;
  return A_COMPRESS;
}

static void recommend(model_t *m) {
  size_t k = 0;
  uint32_t *rc = malloc((m->n ? m->n : 1) * sizeof *rc);
  uint64_t *rq = malloc((m->n ? m->n : 1) * sizeof *rq);
  if (!rc || !rq) { free(rc); free(rq); return; }
  for (size_t i = 0; i < m->n; i++) { rc[k] = (uint32_t)m->f[i].n; rq[k] = m->f[i].requested; k++; }
  qsort(rc, k, sizeof *rc, cmp_u32);
  qsort(rq, k, sizeof *rq, cmp_u64);
  double read_hot = fmax(32.0, floor(pct_sorted_u32(rc, k, 90)));
  double byte_hot = fmax(16.0 * MIB, floor(pct_sorted_u64(rq, k, 90)));
  free(rc); free(rq);
  for (size_t i = 0; i < m->n; i++) {
    file_t *f = &m->f[i];
    if (f->n < 1 || f->requested < 1) continue;
    int layout = classify(f);
    for (int b = 0; b < NBS; b++) f->score[b] = block_score(f, BS[b], layout, NULL);
    int action = known_action(f->rel);
    if (action == A_STORE) layout = L_STREAMING;
    bool hot = layout != L_STREAMING && ((f->n >= 16 && f->repeat64 >= 0.22) || (double)f->n >= read_hot ||
                                         (f->n >= 8 && (double)f->requested >= byte_hot));
    int best = -1;
    for (int b = 0; b < NBS; b++) {
      uint32_t B = BS[b];
      if (layout == L_RANDOM && B > 128 * 1024) continue;
      if (layout == L_MIXED && (B < 32 * 1024 || B > 256 * 1024)) continue;
      if (layout == L_STREAMING && B < 64 * 1024) continue;
      if (best < 0) { best = b; continue; }
      double d1 = f->score[b], d0 = f->score[best];
      double t1 = fabs(log2((double)B / 65536.0)), t0 = fabs(log2((double)BS[best] / 65536.0));
      if (d1 < d0 || (d1 == d0 && (t1 < t0 || (t1 == t0 && B < BS[best])))) best = b;
    }
    if (f->p50 <= 256 && f->n >= 64) { best = 0; layout = L_RANDOM; hot = true; }   /* micro-index files */
    f->action = action; f->layout = layout; f->block_idx = best < 0 ? 2 : best; f->hot = hot;
    f->recommended = true;
  }
}

/* ------------------------------------------------------- cache simulation -- */
/* simulate_cache() of ampr_pack_profile.py: byte-weighted LRU, second-touch admission (hot blocks
 * are admitted at once), up to 5M block touches simulated exactly, otherwise 64 evenly spread
 * windows that each start with an empty cache. */
static uint64_t mix64(uint64_t x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; return x ^ (x >> 33); }

/* u64 -> u32 open-addressing map with deletion; keys are stored + 1 (0 = empty, ~0 = deleted) */
typedef struct { uint64_t *k; uint32_t *v; size_t cap, used, live; } hmap;
#define HM_EMPTY 0ull
#define HM_TOMB UINT64_MAX

static void hm_free(hmap *h) { free(h->k); free(h->v); memset(h, 0, sizeof *h); }
static int hm_init(hmap *h, size_t cap) {
  memset(h, 0, sizeof *h);
  h->k = calloc(cap, sizeof *h->k);
  h->v = malloc(cap * sizeof *h->v);
  if (!h->k || !h->v) { hm_free(h); return -1; }
  h->cap = cap;
  return 0;
}
static void hm_clear(hmap *h) { memset(h->k, 0, h->cap * sizeof *h->k); h->used = h->live = 0; }
static size_t hm_slot(const hmap *h, uint64_t key, bool *found) {
  uint64_t s = key + 1;
  size_t pos = mix64(s) & (h->cap - 1), tomb = SIZE_MAX;
  for (;;) {   /* terminates: used < cap is kept by hm_reserve */
    uint64_t c = h->k[pos];
    if (c == HM_EMPTY) { *found = false; return tomb != SIZE_MAX ? tomb : pos; }
    if (c == HM_TOMB) { if (tomb == SIZE_MAX) tomb = pos; }
    else if (c == s) { *found = true; return pos; }
    pos = (pos + 1) & (h->cap - 1);
  }
}
static int hm_reserve(hmap *h) {
  if ((h->used + 1) * 4 <= h->cap * 3) return 0;
  size_t nc = h->cap;
  while ((h->live + 1) * 2 > nc) nc *= 2;   /* rehash drops the tombstones; grow when really full */
  hmap n;
  if (hm_init(&n, nc) != 0) return -1;
  for (size_t i = 0; i < h->cap; i++) {
    uint64_t c = h->k[i];
    if (c == HM_EMPTY || c == HM_TOMB) continue;
    bool f;
    size_t s = hm_slot(&n, c - 1, &f);
    n.k[s] = c; n.v[s] = h->v[i]; n.used++; n.live++;
  }
  hm_free(h);
  *h = n;
  return 0;
}
static int64_t hm_get(const hmap *h, uint64_t key) {
  bool f;
  size_t s = hm_slot(h, key, &f);
  return f ? (int64_t)h->v[s] : -1;
}
static int hm_put(hmap *h, uint64_t key, uint32_t val) {
  if (hm_reserve(h) != 0) return -1;
  bool f;
  size_t s = hm_slot(h, key, &f);
  if (!f) {
    if (h->k[s] == HM_EMPTY) h->used++;
    h->k[s] = key + 1;
    h->live++;
  }
  h->v[s] = val;
  return 0;
}
static void hm_del(hmap *h, uint64_t key) {
  bool f;
  size_t s = hm_slot(h, key, &f);
  if (f) { h->k[s] = HM_TOMB; h->live--; }
}

#define SEEN_MAX 1000000u   /* bound the "seen once" set on the console (about 24 MiB); Python has no bound */
typedef struct {
  uint64_t *key; uint32_t *size; int32_t *prev, *next;
  size_t cap, hw;            /* allocated nodes, high-water mark */
  int32_t head, tail, freel;
  uint64_t bytes, limit;
  hmap map, seen;
  bool oom;
} lru_t;

static int lru_init(lru_t *c, uint64_t limit) {
  memset(c, 0, sizeof *c);
  c->limit = limit; c->head = c->tail = c->freel = -1;
  if (hm_init(&c->map, 4096) || hm_init(&c->seen, 4096)) { hm_free(&c->map); return -1; }
  return 0;
}
static void lru_free(lru_t *c) { free(c->key); free(c->size); free(c->prev); free(c->next); hm_free(&c->map); hm_free(&c->seen); }
static void lru_reset(lru_t *c) {
  c->hw = 0; c->bytes = 0; c->head = c->tail = c->freel = -1;
  hm_clear(&c->map); hm_clear(&c->seen);
}
static void lru_unlink(lru_t *c, int32_t i) {
  if (c->prev[i] >= 0) c->next[c->prev[i]] = c->next[i]; else c->head = c->next[i];
  if (c->next[i] >= 0) c->prev[c->next[i]] = c->prev[i]; else c->tail = c->prev[i];
}
static void lru_front(lru_t *c, int32_t i) {
  c->prev[i] = -1; c->next[i] = c->head;
  if (c->head >= 0) c->prev[c->head] = i;
  c->head = i;
  if (c->tail < 0) c->tail = i;
}
static int32_t lru_node(lru_t *c) {
  if (c->freel >= 0) { int32_t i = c->freel; c->freel = c->next[i]; return i; }
  if (c->hw == c->cap) {
    size_t nc = c->cap ? c->cap * 2 : 4096;
    if (nc > INT32_MAX) return -1;
    uint64_t *k = realloc(c->key, nc * sizeof *k);
    if (k) c->key = k;
    uint32_t *s = realloc(c->size, nc * sizeof *s);
    if (s) c->size = s;
    int32_t *p = realloc(c->prev, nc * sizeof *p);
    if (p) c->prev = p;
    int32_t *n = realloc(c->next, nc * sizeof *n);
    if (n) c->next = n;
    if (!k || !s || !p || !n) return -1;
    c->cap = nc;
  }
  return (int32_t)c->hw++;
}
/* returns the bytes of this touch served from the cache */
static uint64_t lru_touch(lru_t *c, uint64_t key, uint32_t raw, uint32_t overlap, bool hot) {
  int64_t i = hm_get(&c->map, key);
  if (i >= 0) { lru_unlink(c, (int32_t)i); lru_front(c, (int32_t)i); return overlap; }
  bool admit = hot || hm_get(&c->seen, key) >= 0;
  if (!admit) {
    if (c->seen.live >= SEEN_MAX) hm_clear(&c->seen);
    if (hm_put(&c->seen, key, 0) != 0) c->oom = true;
    return 0;
  }
  hm_del(&c->seen, key);
  if (raw > c->limit) return 0;
  while (c->bytes + raw > c->limit && c->tail >= 0) {
    int32_t v = c->tail;
    lru_unlink(c, v);
    c->bytes -= c->size[v];
    hm_del(&c->map, c->key[v]);
    c->next[v] = c->freel; c->freel = v;
  }
  if (c->bytes + raw > c->limit) return 0;
  int32_t n = lru_node(c);
  if (n < 0 || hm_put(&c->map, key, (uint32_t)n) != 0) {
    if (n >= 0) { c->next[n] = c->freel; c->freel = n; }
    c->oom = true;
    return 0;
  }
  c->key[n] = key; c->size[n] = raw;
  lru_front(c, n);
  c->bytes += raw;
  return 0;
}

#define SIM_MAX_TOUCHES 5000000ull   /* DEFAULT_CACHE_MAX_TOUCHES */
#define SIM_MAX_WINDOWS 64
static const uint64_t CACHE_CAND[] = {32 * MIB, 64 * MIB, 128 * MIB, 256 * MIB, 512 * MIB};

static bool sim_eligible(const file_t *f) { return f->recommended && f->action != A_LOOSE && f->layout != L_STREAMING; }

static uint64_t choose_decoded(model_t *m, double *best_hit) {
  uint64_t total = 0;
  for (size_t i = 0; i < m->gn; i++) {
    const file_t *f = &m->f[m->g[i].file];
    if (!sim_eligible(f)) continue;
    uint32_t B = BS[f->block_idx];
    total += (m->g[i].off + m->g[i].len - 1) / B - m->g[i].off / B + 1;
  }
  *best_hit = 0;
  if (!total) return 64 * MIB;
  /* sampled windows [ws, we) of the global touch sequence */
  uint64_t ws[SIM_MAX_WINDOWS], we[SIM_MAX_WINDOWS];
  int nw = 0;
  if (total <= SIM_MAX_TOUCHES) { ws[0] = 0; we[0] = total; nw = 1; }
  else {
    uint64_t wc = SIM_MAX_TOUCHES / 4096;
    if (wc > SIM_MAX_WINDOWS) wc = SIM_MAX_WINDOWS;
    if (wc < 1) wc = 1;
    uint64_t budget = SIM_MAX_TOUCHES / wc;
    for (uint64_t w = 0; w < wc; w++) {
      uint64_t s = total * w / wc, e = total * (w + 1) / wc, size = e - s;
      if (!size) continue;
      uint64_t b = budget < size ? budget : size;
      ws[nw] = s + (size - b) / 2; we[nw] = ws[nw] + b; nw++;
    }
  }
  double hits[5];
  bool oom = false;
  for (int c = 0; c < 5; c++) {
    lru_t L;
    hits[c] = 0;
    if (lru_init(&L, CACHE_CAND[c]) != 0) { oom = true; continue; }
    uint64_t hit = 0, req = 0, t = 0;
    int w = 0;
    bool started = false;
    for (size_t i = 0; i < m->gn && w < nw; i++) {
      const file_t *f = &m->f[m->g[i].file];
      if (!sim_eligible(f)) continue;
      uint32_t B = BS[f->block_idx];
      uint64_t off = m->g[i].off, end = off + m->g[i].len;
      for (uint64_t b = off / B; b * B < end; b++, t++) {
        while (w < nw && t >= we[w]) { w++; started = false; }
        if (w >= nw) break;
        if (t < ws[w]) continue;
        if (!started) { lru_reset(&L); started = true; }
        uint64_t bs = b * B, be = bs + B < f->size ? bs + B : f->size;
        uint64_t os = off > bs ? off : bs, oe = end < be ? end : be;
        uint32_t overlap = (uint32_t)(oe > os ? oe - os : 0);
        req += overlap;
        hit += lru_touch(&L, ((uint64_t)f->ordinal << 40) | b, (uint32_t)(be - bs), overlap, f->hot);
      }
    }
    oom |= L.oom;
    lru_free(&L);
    hits[c] = req ? (double)hit / (double)req : 0;
  }
  if (oom) { m->sim_oom = true; e2a_log("trace: cache simulation ran out of memory, cache size is approximate"); }
  double best = 0;
  for (int c = 0; c < 5; c++) if (hits[c] > best) best = hits[c];
  *best_hit = best;
  if (best < 0.08) return 64 * MIB;
  if (best < 0.15) return 128 * MIB;
  if (best < 0.25) return 256 * MIB;
  double prev = -1;
  for (int c = 0; c < 5; c++) {
    if (CACHE_CAND[c] < 64 * MIB) { prev = hits[c]; continue; }
    double marginal = prev >= 0 ? hits[c] - prev : hits[c];
    if (best - hits[c] <= 0.03 || (hits[c] >= 0.30 && marginal < 0.05)) return CACHE_CAND[c];
    prev = hits[c];
  }
  return CACHE_CAND[4];
}

static void choose_runtime(model_t *m, uint64_t *decoded, uint64_t *physical, uint32_t *workers, uint32_t *reserve, double *hit) {
  *decoded = choose_decoded(m, hit);
  /* physical page cache: pressure of small random reuse, weighted by requested bytes */
  double wsum = 0, wr = 0, ws = 0, wrand = 0, req_total = 0;
  for (size_t i = 0; i < m->n; i++) {
    file_t *f = &m->f[i];
    if (!f->recommended || f->action == A_LOOSE || f->layout == L_STREAMING) continue;
    double w = (double)f->requested;
    wsum += w; wr += w * f->repeat64; ws += w * f->small64; wrand += w * f->random;
    req_total += w;
  }
  if (wsum <= 0) *physical = 8 * MIB;
  else {
    double pressure = wr / wsum + 0.5 * (ws / wsum) * (wrand / wsum);
    if (req_total >= 16.0 * 1024 * MIB && pressure >= 0.20) *physical = 64 * MIB;
    else if (req_total >= 2.0 * 1024 * MIB || pressure >= 0.10) *physical = 32 * MIB;
    else *physical = 16 * MIB;
  }
  /* workers: how often consecutive reads switch file */
  uint64_t n = 0, switches = 0, small = 0, tiny = 0, bulk = 0;
  int64_t prev = -1;
  uint64_t *bytes = calloc(m->n ? m->n : 1, sizeof *bytes);
  uint64_t tot = 0, files = 0;
  for (size_t i = 0; i < m->gn; i++) {
    file_t *f = &m->f[m->g[i].file];
    if (!f->recommended || f->action == A_LOOSE) continue;
    if (prev >= 0 && prev != (int64_t)m->g[i].file) switches++;
    prev = (int64_t)m->g[i].file;
    n++;
    uint32_t L = m->g[i].len;
    if (L <= 65536) small++;
    if (L <= 16384) tiny++;
    if (L >= 262144) bulk++;
    if (bytes) { if (!bytes[m->g[i].file]) files++; bytes[m->g[i].file] += L; }
    tot += L;
  }
  uint64_t dom = 0;
  for (size_t i = 0; bytes && i < m->n; i++) if (bytes[i] > dom) dom = bytes[i];
  free(bytes);
  if (!n) *workers = 2;
  else {
    double sw = n > 1 ? (double)switches / (double)(n - 1) : 0, dshare = tot ? (double)dom / (double)tot : 0;
    if (files >= 1000 && sw >= 0.70) *workers = 8;
    else if (files >= 128 && sw >= 0.50) *workers = 6;
    else if (dshare >= 0.85 && files <= 8) *workers = 2;
    else *workers = 4;
  }
  if (*workers <= 1 || !n) *reserve = 0;
  else {
    double s = (double)small / n, t = (double)tiny / n, b = (double)bulk / n;
    *reserve = (s >= 0.45 || t >= 0.20 || (s >= 0.25 && b >= 0.20)) ? 1 : 0;
  }
}

/* ------------------------------------------------------------------ TOML -- */
static void toml_str(FILE *f, const char *s) {
  fputc('"', f);
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') fputc('\\', f);
    if ((unsigned char)*s < 0x20) continue;
    fputc(*s, f);
  }
  fputc('"', f);
}

typedef struct { int action, layout, block_idx; bool hot; const char *rel; uint64_t requested; } rec_t;
#define TOML_PATH_BUDGET (3u << 20)
static int cmp_req_desc(const void *a, const void *b) {
  const rec_t *x = a, *y = b;
  return x->requested < y->requested ? 1 : x->requested > y->requested ? -1 : strcmp(x->rel, y->rel);
}
static int cmp_rec(const void *a, const void *b) {
  const rec_t *x = a, *y = b;
  if (x->action != y->action) return x->action - y->action;
  if (x->layout != y->layout) return x->layout - y->layout;
  if (x->block_idx != y->block_idx) return x->block_idx - y->block_idx;
  if (x->hot != y->hot) return (int)x->hot - (int)y->hot;
  return strcmp(x->rel, y->rel);
}

static int write_toml(model_t *m, const char *out, uint64_t dec, uint64_t phys, uint32_t workers, uint32_t reserve,
                      size_t *nrules, size_t *nfiles) {
  rec_t *r = malloc((m->n ? m->n : 1) * sizeof *r);
  if (!r) return -1;
  size_t k = 0;
  for (size_t i = 0; i < m->n; i++) {
    file_t *f = &m->f[i];
    if (!f->recommended || f->action == A_LOOSE) continue;
    if (!strchr(f->rel, '/') || strpbrk(f->rel, "\"\\]#*?") || strlen(f->rel) > 400) continue;
    r[k].action = f->action; r[k].layout = f->layout; r[k].block_idx = f->block_idx; r[k].hot = f->hot; r[k].rel = f->rel;
    r[k].requested = f->requested;
    k++;
  }
  /* keep the profile well under the loader's 4 MiB: drop the least read files first */
  size_t path_bytes = 0, dropped = 0;
  for (size_t i = 0; i < k; i++) path_bytes += strlen(r[i].rel) + 6;
  if (path_bytes > TOML_PATH_BUDGET) {
    qsort(r, k, sizeof *r, cmp_req_desc);
    size_t keep = 0, b = 0;
    while (keep < k && b + strlen(r[keep].rel) + 6 <= TOML_PATH_BUDGET) b += strlen(r[keep++].rel) + 6;
    dropped = k - keep;
    k = keep;
    e2a_log("trace profile: %zu least read files left out to keep the profile small", dropped);
  }
  qsort(r, k, sizeof *r, cmp_rec);
  char tmp[700];
  snprintf(tmp, sizeof tmp, "%s.trace-tmp", out);   /* not the .e2a-tmp name profile saves use */
  FILE *f = fopen(tmp, "w");
  if (!f) { free(r); return -1; }
  time_t now = time(NULL);
  fprintf(f, "# Generated by exfat2ampr %s from %d trace(s), %zu observed file(s), %ld\n", E2A_VERSION, m->traces, k, (long)now);
  fprintf(f, "# Port of ampr_emu ampr_pack_profile.py (exact pattern mode). Unobserved files keep the default rules.\n\n");
  fprintf(f, "[pack]\ncompression_level = 12\ndefault_block_size = \"64KiB\"\n\n");
  fprintf(f, "[runtime]\ndecoded_cache_bytes = \"%lluMiB\"\nphysical_cache_bytes = \"%lluMiB\"\nworkers = %u\nlatency_reserve_workers = %u\n",
          (unsigned long long)(dec / MIB), (unsigned long long)(phys / MIB), workers, reserve);
  /* "keep": the trace only tunes the geometry; what is packed stays decided by the safety list */
  static const char *LAY[] = {"mixed", "random", "streaming"};
  size_t rules = 0;
  for (size_t i = 0; i < k;) {
    size_t j = i, bytes = 0;
    while (j < k && j - i < 256 && bytes < 48 * 1024 && r[j].action == r[i].action && r[j].layout == r[i].layout &&
           r[j].block_idx == r[i].block_idx && r[j].hot == r[i].hot)
      bytes += strlen(r[j++].rel) + 6;
    fprintf(f, "\n[[rule]]\naction = \"keep\"\nblock_size = \"%uKiB\"\nlayout = \"%s\"\n", BS[r[i].block_idx] / 1024,
            LAY[r[i].layout]);
    if (r[i].hot) fprintf(f, "hot = true\n");
    fprintf(f, "include = [\n");
    for (size_t x = i; x < j; x++) { fputs("  ", f); toml_str(f, r[x].rel); fputs(x + 1 < j ? ",\n" : "\n", f); }
    fprintf(f, "]\n");
    rules++;
    i = j;
  }
  free(r);
  int rc = fclose(f) == 0 ? 0 : -1;
  if (rc == 0 && rename(tmp, out) != 0) rc = -1;
  if (rc != 0) unlink(tmp);
  *nrules = rules; *nfiles = k;
  return rc;
}

static void model_free(model_t *m) {
  for (size_t i = 0; i < m->n; i++) { free(m->f[i].rel); free(m->f[i].ev); }
  free(m->f); free(m->tab); free(m->g);
  memset(m, 0, sizeof *m);
}

int trace_build_profile(const char *const *journals, const char *const *indexes, int pairs, const char *out_toml,
                        char *summary, size_t cap) {
  model_t m;
  memset(&m, 0, sizeof m);
  for (int i = 0; i < pairs; i++) load_trace(&m, journals[i], indexes[i], i);
  if (m.gn == 0) {
    snprintf(summary, cap, "no file read found in %d trace(s) (%llu records): was the debug emulator active?", pairs,
             (unsigned long long)m.records);
    model_free(&m);
    return -1;
  }
  for (size_t i = 0; i < m.n; i++) file_metrics(&m.f[i]);
  recommend(&m);
  uint64_t dec, phys;
  uint32_t workers, reserve;
  double hit;
  choose_runtime(&m, &dec, &phys, &workers, &reserve, &hit);
  size_t rules = 0, files = 0;
  int rc = write_toml(&m, out_toml, dec, phys, workers, reserve, &rules, &files);
  size_t cnt[3] = {0, 0, 0}, hot = 0;
  for (size_t i = 0; i < m.n; i++)
    if (m.f[i].recommended && m.f[i].action != A_LOOSE) { cnt[m.f[i].layout]++; hot += m.f[i].hot; }
  snprintf(summary, cap,
           "%d trace(s), %llu reads, %zu files: %zu mixed / %zu random / %zu streaming, %zu hot; cache %llu+%llu MiB "
           "(simulated hit %.0f%%), %u workers; %zu rules%s%s",
           m.traces, (unsigned long long)m.gn, files, cnt[0], cnt[1], cnt[2], hot, (unsigned long long)(dec / MIB),
           (unsigned long long)(phys / MIB), hit * 100.0, workers, rules,
           m.decode_errors ? " (some records could not be decoded)" : "",
           m.oom || m.sim_oom ? " (out of memory: partial result)" : "");
  e2a_log("trace profile: %s -> %s", summary, out_toml);
  model_free(&m);
  return rc;
}

/* ------------------------------------------------------------ console glue -- */
static void traces_dir(const char *title_id, char *out, size_t cap) { snprintf(out, cap, "%s/traces/%s", E2A_STATE_DIR, title_id); }

/* our fakelib2 carries a marker so that a backport's own fakelib2 is never touched */
bool trace_enabled(const char *game_dir) {
  char p[700];
  snprintf(p, sizeof p, "%s/fakelib2/.e2a-trace", game_dir);
  return e2a_is_file(p);
}

int trace_enable(const char *game_dir, char *err, size_t errcap) {
  char dir[700], dst[1024], src_dir[700];
  if (libSceAmpr_debug_sprx_len < 4096 || libSceAmpr_debug_sprx[0] != 0x4F) {
    snprintf(err, errcap, "debug emulator missing from this build");
    return -1;
  }
  snprintf(dir, sizeof dir, "%s/fakelib2", game_dir);
  snprintf(src_dir, sizeof src_dir, "%s/fakelib", game_dir);
  if (e2a_is_dir(dir) && !trace_enabled(game_dir)) {
    snprintf(err, errcap, "this game already has its own fakelib2 (backport): trace mode not possible");
    return -1;
  }
  if (e2a_mkdir_p(dir) != 0) { snprintf(err, errcap, "cannot create %s (read-only source?)", dir); return -1; }
  /* ShadowMountPlus mounts fakelib2 instead of fakelib: carry over the other libraries the game needs */
  DIR *d = opendir(src_dir);
  if (d) {
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
      if (de->d_name[0] == '.' || strcasecmp(de->d_name, "libSceAmpr.sprx") == 0) continue;
      char s[1024];
      snprintf(s, sizeof s, "%s/%s", src_dir, de->d_name);
      snprintf(dst, sizeof dst, "%s/%s", dir, de->d_name);
      if (e2a_is_file(s) && e2a_copy_file(s, dst, (int64_t)time(NULL)) != 0) e2a_log("trace: cannot copy %s", s);
    }
    closedir(d);
  }
  snprintf(dst, sizeof dst, "%s/libSceAmpr.sprx", dir);
  if (e2a_write_file_atomic(dst, libSceAmpr_debug_sprx, libSceAmpr_debug_sprx_len) != 0) {
    snprintf(err, errcap, "cannot write the debug emulator");
    e2a_rm_rf(dir);
    return -1;
  }
  snprintf(dst, sizeof dst, "%s/.e2a-trace", dir);
  if (e2a_write_file_atomic(dst, "exfat2ampr trace mode\n", 22) != 0) {
    snprintf(err, errcap, "cannot write the trace marker");
    e2a_rm_rf(dir);
    return -1;
  }
  e2a_log("trace mode on: %s (debug emulator %u bytes in fakelib2)", game_dir, libSceAmpr_debug_sprx_len);
  return 0;
}

int trace_disable(const char *game_dir) {
  char dir[700];
  snprintf(dir, sizeof dir, "%s/fakelib2", game_dir);
  if (!e2a_is_dir(dir)) return 0;
  if (!trace_enabled(game_dir)) return -1;   /* not ours */
  e2a_log("trace mode off: %s", game_dir);
  return e2a_rm_rf(dir);
}

static bool trace_dir_complete(const char *dir, const char *name) {
  char j[800], i[800];
  snprintf(j, sizeof j, "%s/%s/ampr_commands.bin", dir, name);
  snprintf(i, sizeof i, "%s/%s/ampr_emu.index", dir, name);
  return e2a_is_file(j) && e2a_is_file(i);
}

int trace_count(const char *title_id) {
  char dir[400];
  traces_dir(title_id, dir, sizeof dir);
  DIR *d = opendir(dir);
  if (!d) return 0;
  int n = 0;
  struct dirent *de;
  while ((de = readdir(d)) != NULL)
    if (de->d_name[0] != '.' && trace_dir_complete(dir, de->d_name)) n++;
  closedir(d);
  return n;
}

/* a game session is identified by its first record (the emulator truncates the journal at launch) */
static int journal_session(const char *journal, char *out, size_t cap) {
  int fd = open(journal, O_RDONLY);
  if (fd < 0) return -1;
  cmd_hdr h;
  size_t got = 0;
  int rc = e2a_read_full(fd, &h, sizeof h, &got) == 0 && got == sizeof h && memcmp(h.magic, "AMPRCMD1", 8) == 0 ? 0 : -1;
  close(fd);
  if (rc == 0) snprintf(out, cap, "%llx-%llx-%llx", (unsigned long long)h.utc_ns, (unsigned long long)h.monotonic_ns,
                        (unsigned long long)h.sequence);
  return rc;
}

int trace_collect(const char *game_dir, const char *title_id, char *err, size_t errcap) {
  char journal[700], index[700], dir[400], stamp_path[500];
  snprintf(journal, sizeof journal, "%s/ampr_commands.bin", game_dir);
  snprintf(index, sizeof index, "%s/ampr_emu.index", game_dir);
  struct stat st;
  if (stat(journal, &st) != 0 || st.st_size < 96) { snprintf(err, errcap, "no trace yet: play the game with trace mode on, then quit it"); return 0; }
  if (!e2a_is_file(index)) { snprintf(err, errcap, "ampr_emu.index is missing next to the trace"); return -1; }
  if (time(NULL) - st.st_mtime < 5) { snprintf(err, errcap, "the game is still writing its trace: quit it first"); return 0; }
  char session[80];
  if (journal_session(journal, session, sizeof session) != 0) { snprintf(err, errcap, "the trace is not an AMPRCMD1 journal"); return -1; }
  traces_dir(title_id, dir, sizeof dir);
  if (e2a_mkdir_p(dir) != 0) { snprintf(err, errcap, "cannot create %s", dir); return -1; }
  /* "last" = "<session> <size> <folder>": the same session collected again (it was copied while the
   * game was still running) replaces its folder instead of being counted twice */
  char *old = NULL, osess[80] = "", oname[32] = "";
  long long osize = -1;
  snprintf(stamp_path, sizeof stamp_path, "%s/last", dir);
  if (e2a_read_text(stamp_path, &old, NULL, 200) == 0) {
    if (sscanf(old, "%79s %lld %31s", osess, &osize, oname) != 3) osess[0] = 0;
    free(old);
  }
  char sub[500], dst[700], name[32];
  bool same = osess[0] && strcmp(osess, session) == 0 && !strchr(oname, '/') && oname[0] != '.';
  if (same && osize == (long long)st.st_size) { snprintf(err, errcap, "this session is already collected"); return 0; }
  if (same) snprintf(name, sizeof name, "%s", oname);
  else {
    for (int n = trace_count(title_id) + 1;; n++) {
      snprintf(name, sizeof name, "%03d", n);
      snprintf(sub, sizeof sub, "%s/%s", dir, name);
      if (!e2a_is_dir(sub)) break;
    }
  }
  snprintf(sub, sizeof sub, "%s/%s", dir, name);
  if (e2a_mkdir_p(sub) != 0) { snprintf(err, errcap, "cannot create %s", sub); return -1; }
  snprintf(dst, sizeof dst, "%s/ampr_commands.bin", sub);
  int rc = e2a_copy_file(journal, dst, st.st_mtime);
  if (rc == 0) {
    snprintf(dst, sizeof dst, "%s/ampr_emu.index", sub);
    rc = e2a_copy_file(index, dst, st.st_mtime);
  }
  if (rc != 0) {
    snprintf(err, errcap, "copy failed: %s", strerror(errno));
    e2a_rm_rf(sub);
    return -1;
  }
  char stamp[200];
  snprintf(stamp, sizeof stamp, "%s %lld %s", session, (long long)st.st_size, name);
  e2a_write_file_atomic(stamp_path, stamp, strlen(stamp));
  e2a_log("trace %s for %s: %s (%lld bytes)", same ? "updated" : "collected", title_id, sub, (long long)st.st_size);
  return 1;
}

int trace_generate(const char *title_id, char *summary, size_t cap) {
  char dir[400];
  traces_dir(title_id, dir, sizeof dir);
  DIR *d = opendir(dir);
  if (!d) { snprintf(summary, cap, "no collected trace for %s", title_id); return -1; }
  char *jp[256], *ip[256];
  int n = 0;
  struct dirent *de;
  while ((de = readdir(d)) != NULL && n < 256) {
    if (de->d_name[0] == '.') continue;
    char j[800], i[800];
    snprintf(j, sizeof j, "%s/%s/ampr_commands.bin", dir, de->d_name);
    snprintf(i, sizeof i, "%s/%s/ampr_emu.index", dir, de->d_name);
    if (!trace_dir_complete(dir, de->d_name)) continue;
    jp[n] = strdup(j); ip[n] = strdup(i);
    if (!jp[n] || !ip[n]) { free(jp[n]); free(ip[n]); break; }
    n++;
  }
  closedir(d);
  if (!n) { snprintf(summary, cap, "no collected trace for %s", title_id); return -1; }
  e2a_mkdir_p(E2A_PROFILE_DIR);
  char out[400];
  snprintf(out, sizeof out, "%s/%s.toml", E2A_PROFILE_DIR, title_id);
  int rc = trace_build_profile((const char *const *)jp, (const char *const *)ip, n, out, summary, cap);
  for (int k = 0; k < n; k++) { free(jp[k]); free(ip[k]); }
  return rc;
}

static const char *TRACE_ROOTS[] = {E2A_HOMEBREW_DIR, "/mnt/ext0/homebrew", "/mnt/ext1/homebrew", "/mnt/usb0/homebrew",
                                   "/mnt/usb1/homebrew", "/mnt/usb2/homebrew", "/mnt/usb3/homebrew", "/mnt/usb4/homebrew",
                                   "/mnt/usb5/homebrew", "/mnt/usb6/homebrew", "/mnt/usb7/homebrew", NULL};

bool trace_game_allowed(const char *path) {
  char real[PATH_MAX];
  if (strstr(path, "/../") || !realpath(path, real)) return false;
  for (int r = 0; TRACE_ROOTS[r]; r++) {
    char root[PATH_MAX];
    if (!realpath(TRACE_ROOTS[r], root)) continue;
    size_t n = strlen(root);
    if (strncmp(real, root, n) == 0 && real[n] == '/' && !strchr(real + n + 1, '/')) return true;
  }
  return false;
}

int trace_list_json(char *json, size_t cap) {
  size_t pos = 0;
  bool first = true;
  if (cap < 3) return -1;
  json[pos++] = '[';
  for (int r = 0; TRACE_ROOTS[r]; r++) {
    DIR *d = opendir(TRACE_ROOTS[r]);
    if (!d) continue;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
      if (de->d_name[0] == '.') continue;
      char g[600], a[700], s[700], j[700], prof[500];
      snprintf(g, sizeof g, "%s/%s", TRACE_ROOTS[r], de->d_name);
      snprintf(s, sizeof s, "%s/.e2a-state", g);
      if (e2a_is_dir(s)) continue;   /* conversion in progress or interrupted */
      e2a_game gm;
      if (e2a_read_game(g, &gm) != 0 || strlen(gm.title_id) != 9) continue;
      snprintf(a, sizeof a, "%s/ampr_assets.index", g);
      snprintf(j, sizeof j, "%s/ampr_commands.bin", g);
      snprintf(prof, sizeof prof, "%s/%s.toml", E2A_PROFILE_DIR, gm.title_id);
      char ge[1300], te[300];
      e2a_json_escape(g, ge, sizeof ge);
      e2a_json_escape(gm.title, te, sizeof te);
      int n = snprintf(json + pos, cap - pos,
                       "%s{\"path\":\"%s\",\"title_id\":\"%s\",\"title\":\"%s\",\"converted\":%s,\"trace_mode\":%s,"
                       "\"pending\":%s,\"collected\":%d,\"profile\":%s}",
                       first ? "" : ",", ge, gm.title_id, te, e2a_is_file(a) ? "true" : "false", trace_enabled(g) ? "true" : "false",
                       e2a_file_size(j) >= 96 ? "true" : "false", trace_count(gm.title_id), e2a_is_file(prof) ? "true" : "false");
      if (n < 0 || (size_t)n >= cap - pos - 2) break;
      pos += (size_t)n;
      first = false;
    }
    closedir(d);
  }
  json[pos++] = ']';
  json[pos] = 0;
  return 0;
}
