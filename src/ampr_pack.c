/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * AMPRPAK4 / AMPRDAT3 / AMPRCRC1 writer: a C port of the essential parts of
 * drakmor/ampr_emu tools/ampr_pack.py (single group, mixed layout, 64 KiB blocks,
 * io-page aware placement, LZ4 raw blocks, raw fallback, auto-loose, block
 * deduplication), with a read-ahead thread and byte-level checkpoints so a
 * conversion can be paused and resumed. */
#include "ampr_pack.h"
#include "activity.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lz4.h"
#include "lz4hc.h"

/* Per-file block size (16 KiB .. 1 MiB, from the profile). The batch buffers have a fixed byte
 * budget; the number of blocks per batch adapts to the block size of the file being packed. */
#define POOL_BYTES (24u << 20)   /* one batch: read by four threads, compressed by the workers, written in one piece */
#define RATE_LOG_S 120.0         /* how often the packer logs what each stage sustains */
#define MIN_BLOCK (16u << 10)
#define MAX_SLOTS (POOL_BYTES / MIN_BLOCK)
#define CKPT_INTERVAL_S 10.0
#define MAX_VOLS 1024

static char g_pause_reason[160];
const char *e2a_pack_pause_reason(void) { return g_pause_reason; }
void e2a_pack_set_pause_reason(const char *why) { snprintf(g_pause_reason, sizeof g_pause_reason, "%s", why ? why : ""); }

/* write/read errors that mean "the drive is full, gone or read-only": pause (resumable), not fail */
static bool storage_pause(int err) {
  if (err == ENOSPC) { e2a_pack_set_pause_reason("destination full: free some space, then resume"); return true; }
  if (err == EIO || err == ENXIO || err == ENODEV || err == ENOENT || err == EROFS || err == EBADF) {
    e2a_pack_set_pause_reason("storage disconnected or read-only: plug it back in, then resume");
    return true;
  }
  return false;
}

/* ------------------------------------------------------------ containers -- */
typedef struct { uint8_t *p; size_t n, cap; } bytebuf;
static int bb_reserve(bytebuf *b, size_t extra) {
  if (b->n + extra <= b->cap) return 0;
  size_t nc = b->cap ? b->cap * 2 : 4096;
  while (nc < b->n + extra) nc *= 2;
  uint8_t *np = realloc(b->p, nc);
  if (!np) return -1;
  b->p = np; b->cap = nc;
  return 0;
}
static int bb_push(bytebuf *b, const void *d, size_t n) {
  if (bb_reserve(b, n)) return -1;
  memcpy(b->p + b->n, d, n); b->n += n;
  return 0;
}
static int32_t strtab_add(bytebuf *b, const char *s) {
  int32_t off = (int32_t)b->n;
  if (bb_push(b, s, strlen(s) + 1)) return -1;
  return off;
}

typedef struct {
  int fd;
  uint32_t id;
  char name[64];
  uint64_t pos;         /* next write position */
  uint64_t file_size;
  uint32_t flags;
} volume;

/* deduplication record (persisted in dedup.bin) */
typedef struct __attribute__((packed)) {
  uint8_t fp[16];       /* sha256(raw)[0:16] */
  uint32_t raw_len;
  uint32_t want_raw;    /* store-only files never share LZ4 blocks */
  uint64_t location;
  uint32_t descriptor;
} dd_rec;

/* ------------------------------------------------------------ checkpoint -- */
#define CKPT_MAGIC "E2ACKPT3"
typedef struct __attribute__((packed)) {
  char magic[8];
  uint32_t version;
  uint32_t level;
  uint64_t entry_count, total_bytes, entries_hash;
  uint64_t next_entry;          /* entry being processed (or next to process) */
  uint64_t cur_offset;          /* bytes of that entry already packed */
  uint32_t cur_first_chunk, cur_nchunks;
  uint64_t packed_in;
  uint64_t files_len, chunks_len, crcs_len, strings_len, dedup_len;
  e2a_pack_stats st;
  uint32_t nvols, reserved;
  uint64_t vol_pos[MAX_VOLS];
  uint32_t vol_flags[MAX_VOLS];
} e2a_ckpt;

/* a volume handed to the read-back thread: its chunk records and CRCs, copied (the main thread keeps appending) */
typedef struct { uint32_t id; char path[1100]; size_t nrec; ampr_chunk_record *recs; uint32_t *crcs; } bgv_item;

typedef struct {
  const e2a_pack_opts *o;
  uint32_t payload_offset;
  volume vols[MAX_VOLS];
  uint32_t nvols;
  volume *cur;
  bytebuf files, chunks, packs, strings, crcs, dedup;
  uint64_t packed_in;
  e2a_pack_stats *st;
  uint64_t entries_hash;
  /* dedup hash table: 1-based indices into dedup (dd_rec array), open addressing */
  uint32_t *dd_tab; size_t dd_cap, dd_count;
  bool dd_capped;             /* DD_MAX_RECORDS reached: later blocks are written without being offered for sharing */
  /* a volume that filled up is read back and checked on its own thread while the next one fills */
  pthread_t bgv_th;
  bool bgv_started, bgv_stop;
  volatile bool bgv_stop_now;
  pthread_mutex_t bgv_mx;
  pthread_cond_t bgv_cv;
  bgv_item *bgv_queue[MAX_VOLS];
  int bgv_head, bgv_tail;
  volatile int bgv_err;
  char bgv_msg[200];
  uint32_t vol_first_chunk[MAX_VOLS];   /* first chunk record of each volume opened in this run (UINT32_MAX: reopened) */
  /* checkpoint bookkeeping */
  uint64_t ck_files, ck_chunks, ck_crcs, ck_strings, ck_dedup;
  double last_ckpt;
  /* compression batch (double buffered input) */
  int slots, bound;           /* blocks per batch and LZ4 bound for the current file */
  uint32_t block;             /* block size of the current file */
  int layout;                 /* E2A_LAYOUT_* of the current file */
  bool want_fp;               /* fingerprints only when the current file can be deduplicated */
  uint32_t pool;              /* bytes per input buffer (POOL_BYTES, or less when the console is short of memory) */
  uint8_t *raw[2];
  uint32_t *raw_len[2];
  int bcount[2], bstate[2];   /* 0 free, 1 full, 2 last batch of the file */
  int rd_fd, reader_err, reader_abort;
  uint64_t rd_remaining, rd_pos;
  pthread_mutex_t mx;
  pthread_cond_t cv;
  /* output staging: the blocks of a batch land in one buffer and reach the volume in one write */
  uint8_t *stg;
  size_t stg_cap, stg_len;
  uint64_t stg_off;
  volume *stg_vol;
  bool stg_failed;            /* a flush failed: the chunk records of this batch are not on disk, no checkpoint */
  /* what each stage sustains, logged now and then; w_*: the values at the start of the current window */
  double t_read, t_comp, t_write, t_rate_log;
  uint64_t b_read, b_comp, b_write;
  double w_at, w_tr, w_tc, w_tw;
  uint64_t w_br, w_bc, w_bw;
  uint64_t autoloose;   /* bytes of files found incompressible and left as they are */
  /* worker outputs for the batch being processed */
  const uint8_t *cur_raw;
  const uint32_t *cur_len;
  uint8_t *comp;
  int *comp_len;
  uint32_t *raw_crc;
  uint8_t *fp;                /* 16 bytes per slot */
  bool batch_store;           /* skip LZ4 for store-only files */
} packer;

static uint64_t entries_hash(const e2a_list *l) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < l->n; i++) {
    const e2a_entry *e = &l->v[i];
    const uint8_t *k = (const uint8_t *)e->key;
    for (size_t j = 0; k[j]; j++) { h ^= k[j]; h *= 0x100000001B3ull; }
    /* geometry is part of the hash: a changed profile cannot resume a half-packed file */
    uint64_t v = e->size ^ ((uint64_t)e->mtime << 1) ^ ((uint64_t)e->block_shift << 56) ^ ((uint64_t)e->layout << 52) ^
                 ((uint64_t)e->hot << 50);
    for (int j = 0; j < 8; j++) { h ^= (v >> (8 * j)) & 0xFF; h *= 0x100000001B3ull; }
  }
  return h;
}

static int append_tail(const char *dir, const char *name, const bytebuf *b, uint64_t from) {
  if (b->n < from) return -1;
  char path[700];
  snprintf(path, sizeof path, "%s/%s", dir, name);
  int fd = open(path, O_WRONLY | O_CREAT, 0666);
  if (fd < 0) return -1;
  int rc = 0;
  if (ftruncate(fd, (off_t)from) != 0) rc = -1;
  if (rc == 0 && b->n > from && e2a_pwrite_all(fd, b->p + from, b->n - from, from) != 0) rc = -1;
  if (rc == 0 && fsync(fd) != 0) rc = -1;
  close(fd);
  return rc;
}

static int load_whole(const char *dir, const char *name, bytebuf *b, uint64_t len) {
  char path[700];
  snprintf(path, sizeof path, "%s/%s", dir, name);
  b->n = 0;
  if (len == 0) return 0;
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  int rc = 0;
  size_t got = 0;
  if (bb_reserve(b, (size_t)len) || e2a_read_full(fd, b->p, (size_t)len, &got) != 0 || got != len) rc = -1;
  close(fd);
  b->n = rc == 0 ? (size_t)len : 0;
  return rc;
}

/* Persist everything needed to continue from (entry next_entry, byte cur_offset). */
static int checkpoint(packer *P, uint64_t next_entry, uint64_t cur_offset, uint32_t cur_first_chunk,
                      uint32_t cur_nchunks, uint64_t entry_count, uint64_t total_bytes) {
  const char *dir = P->o->state_dir;
  if (!dir) return 0;
  if (e2a_mkdir_p(dir) != 0) return -1;
  for (uint32_t i = 0; i < P->nvols; i++) if (P->vols[i].fd >= 0 && fsync(P->vols[i].fd) != 0) return -1;
  if (append_tail(dir, "files.bin", &P->files, P->ck_files) || append_tail(dir, "chunks.bin", &P->chunks, P->ck_chunks) ||
      append_tail(dir, "crcs.bin", &P->crcs, P->ck_crcs) || append_tail(dir, "strings.bin", &P->strings, P->ck_strings) ||
      append_tail(dir, "dedup.bin", &P->dedup, P->ck_dedup))
    return -1;

  e2a_ckpt *c = calloc(1, sizeof *c);
  if (!c) return -1;
  memcpy(c->magic, CKPT_MAGIC, 8);
  c->version = 2; c->level = (uint32_t)P->o->level;
  c->entry_count = entry_count; c->total_bytes = total_bytes; c->entries_hash = P->entries_hash;
  c->next_entry = next_entry; c->cur_offset = cur_offset;
  c->cur_first_chunk = cur_first_chunk; c->cur_nchunks = cur_nchunks;
  c->packed_in = P->packed_in;
  c->files_len = P->files.n; c->chunks_len = P->chunks.n; c->crcs_len = P->crcs.n;
  c->strings_len = P->strings.n; c->dedup_len = P->dedup.n;
  c->st = *P->st;
  c->nvols = P->nvols;
  for (uint32_t i = 0; i < P->nvols; i++) { c->vol_pos[i] = P->vols[i].pos; c->vol_flags[i] = P->vols[i].flags; }

  char final[700];
  snprintf(final, sizeof final, "%s/state.bin", dir);
  int rc = e2a_write_file_atomic(final, c, sizeof *c);
  free(c);
  if (rc == 0) {
    P->ck_files = P->files.n; P->ck_chunks = P->chunks.n; P->ck_crcs = P->crcs.n;
    P->ck_strings = P->strings.n; P->ck_dedup = P->dedup.n;
    P->last_ckpt = e2a_now();
  } else {
    e2a_log("checkpoint failed: %s", strerror(errno));
  }
  return rc;
}

static int ckpt_read(const char *dir, e2a_ckpt *c) {
  char path[700];
  snprintf(path, sizeof path, "%s/state.bin", dir);
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  size_t got = 0;
  int rc = e2a_read_full(fd, c, sizeof *c, &got) == 0 && got == sizeof *c ? 0 : -1;
  close(fd);
  if (rc == 0 && (memcmp(c->magic, CKPT_MAGIC, 8) != 0 || c->version != 2 || c->nvols > MAX_VOLS)) rc = -1;
  return rc;
}

bool e2a_pack_has_checkpoint(const char *state_dir) {
  e2a_ckpt *c = malloc(sizeof *c);
  if (!c) return false;
  bool ok = ckpt_read(state_dir, c) == 0;
  free(c);
  return ok;
}

void e2a_pack_clear_checkpoint(const char *dir) {
  if (!dir) return;
  static const char *names[] = {"state.bin", "state.bin.e2a-tmp", "files.bin", "chunks.bin", "crcs.bin", "strings.bin", "dedup.bin", NULL};
  char path[700];
  for (int i = 0; names[i]; i++) { snprintf(path, sizeof path, "%s/%s", dir, names[i]); unlink(path); }
}

/* --------------------------------------------------------------- dedup -- */
static uint64_t dd_hash(const dd_rec *r) {
  uint64_t h;
  memcpy(&h, r->fp, 8);
  return h ^ ((uint64_t)r->raw_len << 32) ^ (uint64_t)r->want_raw;
}

static int dd_rehash(packer *P, size_t cap) {
  uint32_t *tab = calloc(cap, sizeof *tab);
  if (!tab) return -1;
  const dd_rec *recs = (const dd_rec *)P->dedup.p;
  size_t n = P->dedup.n / sizeof(dd_rec);
  for (size_t i = 0; i < n; i++) {
    size_t pos = (size_t)(dd_hash(&recs[i]) & (cap - 1));
    while (tab[pos]) pos = (pos + 1) & (cap - 1);
    tab[pos] = (uint32_t)(i + 1);
  }
  free(P->dd_tab);
  P->dd_tab = tab; P->dd_cap = cap; P->dd_count = n;
  return 0;
}

static const dd_rec *dd_find(packer *P, const uint8_t fp[16], uint32_t raw_len, uint32_t want_raw) {
  if (!P->dd_tab) return NULL;
  dd_rec key;
  memcpy(key.fp, fp, 16); key.raw_len = raw_len; key.want_raw = want_raw;
  const dd_rec *recs = (const dd_rec *)P->dedup.p;
  size_t pos = (size_t)(dd_hash(&key) & (P->dd_cap - 1));
  while (P->dd_tab[pos]) {
    const dd_rec *r = &recs[P->dd_tab[pos] - 1];
    if (r->raw_len == raw_len && r->want_raw == want_raw && memcmp(r->fp, fp, 16) == 0) return r;
    pos = (pos + 1) & (P->dd_cap - 1);
  }
  return NULL;
}

/* the table costs 36 bytes per record plus the index: a 300 GB game would ask for hundreds of MiB, so past this
 * many records new blocks are no longer offered for sharing (earlier ones still are); the pack stays valid */
#define DD_MAX_RECORDS (4u << 20)

static int dd_insert(packer *P, const uint8_t fp[16], uint32_t raw_len, uint32_t want_raw, uint64_t location, uint32_t descriptor) {
  if (P->dd_count >= DD_MAX_RECORDS) {
    if (!P->dd_capped) { P->dd_capped = true; e2a_log("dedup table full (%u blocks): the following blocks are written without sharing", DD_MAX_RECORDS); }
    return 0;
  }
  if (!P->dd_tab && dd_rehash(P, 1 << 16)) return -1;
  if ((P->dd_count + 1) * 2 > P->dd_cap && dd_rehash(P, P->dd_cap * 2)) return -1;
  dd_rec r;
  memcpy(r.fp, fp, 16); r.raw_len = raw_len; r.want_raw = want_raw; r.location = location; r.descriptor = descriptor;
  if (bb_push(&P->dedup, &r, sizeof r)) return -1;
  size_t pos = (size_t)(dd_hash(&r) & (P->dd_cap - 1));
  while (P->dd_tab[pos]) pos = (pos + 1) & (P->dd_cap - 1);
  P->dd_tab[pos] = (uint32_t)(P->dedup.n / sizeof(dd_rec));
  P->dd_count++;
  return 0;
}

/* ---------------------------------------------------------------- volumes -- */
static void vol_path(packer *P, const volume *v, char *out, size_t cap) {
  snprintf(out, cap, "%s/%s.e2a-tmp", P->o->out_dir, v->name);
}

static int vol_open(packer *P) {
  if (P->nvols >= MAX_VOLS) return -1;
  volume *v = &P->vols[P->nvols];
  memset(v, 0, sizeof *v);
  v->id = P->nvols;
  snprintf(v->name, sizeof v->name, "ampr_assets-%03u.pak", v->id);
  char path[1100];
  vol_path(P, v, path, sizeof path);
  v->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (v->fd < 0) { e2a_log("cannot create %s: %s", path, strerror(errno)); return -1; }
  v->pos = P->payload_offset;
  v->flags = AMPR_PACK_IO_PAGE_LAYOUT;
  P->vol_first_chunk[v->id] = (uint32_t)(P->chunks.n / sizeof(ampr_chunk_record));
  P->cur = v;
  P->nvols++;
  e2a_log("volume %s opened", v->name);
  return 0;
}

static int vol_reopen(packer *P, uint32_t id, uint64_t pos, uint32_t flags) {
  volume *v = &P->vols[id];
  memset(v, 0, sizeof *v);
  v->id = id;
  snprintf(v->name, sizeof v->name, "ampr_assets-%03u.pak", id);
  char path[1100];
  vol_path(P, v, path, sizeof path);
  v->fd = open(path, O_WRONLY);
  if (v->fd < 0) { e2a_log("cannot reopen %s: %s", path, strerror(errno)); return -1; }
  if (ftruncate(v->fd, (off_t)pos) != 0) { close(v->fd); v->fd = -1; return -1; }  /* drop bytes written after the checkpoint */
  v->pos = pos; v->flags = flags;
  P->vol_first_chunk[id] = UINT32_MAX;   /* filled in a previous run: the final verification covers it */
  return 0;
}

/* -------------------------------------------------- read-back of volumes -- */
/* A volume that just filled up is read back on its own thread while the next one fills: every block it holds
 * is decoded and CRC-compared, so a bad disk is caught within minutes instead of at the end, and the final
 * verification skips it. The records are copied at enqueue time: the main thread keeps appending to P->chunks. */
static void *bgv_thread(void *arg) {
  packer *P = arg;
  uint8_t *stored = malloc((1u << 20) + 4096), *raw = malloc(1u << 20);
  for (;;) {
    pthread_mutex_lock(&P->bgv_mx);
    while (P->bgv_head == P->bgv_tail && !P->bgv_stop) pthread_cond_wait(&P->bgv_cv, &P->bgv_mx);
    if (P->bgv_head == P->bgv_tail) { pthread_mutex_unlock(&P->bgv_mx); break; }
    bgv_item *it = P->bgv_queue[P->bgv_head++];
    pthread_mutex_unlock(&P->bgv_mx);
    const char *what = NULL;
    size_t bad = 0, k = 0;
    int fd = -1;
    if (!P->bgv_stop_now && !P->bgv_err) {
      if (!stored || !raw) what = "out of memory";
      else if ((fd = open(it->path, O_RDONLY)) < 0) what = "cannot reopen the volume";
    }
    for (; !what && fd >= 0 && k < it->nrec && !P->bgv_stop_now; k++) {
      const ampr_chunk_record *cr = &it->recs[k];
      uint32_t pack_id = (uint32_t)(cr->location >> 48), flags = cr->descriptor >> 22;
      if (pack_id != it->id || (flags & AMPR_CHUNK_SHARED)) continue;   /* a block shared from another volume */
      uint64_t off = cr->location & 0xFFFFFFFFFFFFull;
      uint32_t ssize = (cr->descriptor & 0xFFFFF) + 1, codec = (cr->descriptor >> 20) & 3;
      if (ssize > (1u << 20) + 4096 || e2a_pread_all(fd, stored, ssize, off) != 0) { what = "read failed"; bad = k; break; }
      const uint8_t *data = stored;
      uint32_t n = ssize;
      if (codec == AMPR_CODEC_LZ4) {
        int d = LZ4_decompress_safe((const char *)stored, (char *)raw, (int)ssize, 1 << 20);
        if (d < 0) { what = "lz4 decode failed"; bad = k; break; }
        data = raw;
        n = (uint32_t)d;
      }
      if (e2a_crc32(0, data, n) != it->crcs[k]) { what = "crc mismatch"; bad = k; break; }
      activity_throttle(ssize);
    }
    bool closed_ok = fd >= 0;
    if (fd >= 0) close(fd);
    if (!closed_ok) k = 0;
    if (what && !P->bgv_stop_now) {
      pthread_mutex_lock(&P->bgv_mx);
      if (!P->bgv_err) {
        snprintf(P->bgv_msg, sizeof P->bgv_msg, "read-back of volume %u failed: %s (block %zu of %zu)", it->id, what, bad, it->nrec);
        P->bgv_err = 1;
      }
      pthread_mutex_unlock(&P->bgv_mx);
    } else if (!what && fd >= 0 && k == it->nrec && !P->bgv_stop_now && P->o->verified_bits && it->id < MAX_VOLS) {   /* every block read */
      P->o->verified_bits[it->id >> 6] |= 1ull << (it->id & 63);
      e2a_log("volume %u read back: %zu blocks OK", it->id, it->nrec);
    }
    free(it->recs);
    free(it->crcs);
    free(it);
  }
  free(stored);
  free(raw);
  return NULL;
}

static int bgv_enqueue(packer *P, const volume *v) {
  if (!P->o->verified_bits || v->id >= MAX_VOLS || P->vol_first_chunk[v->id] == UINT32_MAX) return 0;
  uint32_t a = P->vol_first_chunk[v->id], b = (uint32_t)(P->chunks.n / sizeof(ampr_chunk_record));
  if (b < a) return 0;
  bgv_item *it = calloc(1, sizeof *it);
  if (!it) return -1;
  it->id = v->id;
  it->nrec = b - a;
  vol_path(P, v, it->path, sizeof it->path);
  if (it->nrec) {
    it->recs = malloc(it->nrec * sizeof(ampr_chunk_record));
    it->crcs = malloc(it->nrec * sizeof(uint32_t));
    if (!it->recs || !it->crcs) { free(it->recs); free(it->crcs); free(it); return -1; }
    memcpy(it->recs, (const ampr_chunk_record *)P->chunks.p + a, it->nrec * sizeof(ampr_chunk_record));
    memcpy(it->crcs, (const uint32_t *)P->crcs.p + a, it->nrec * sizeof(uint32_t));
  }
  pthread_mutex_lock(&P->bgv_mx);
  if (!P->bgv_started) {
    if (pthread_create(&P->bgv_th, NULL, bgv_thread, P) != 0) {   /* no read-back: the final pass does it all */
      pthread_mutex_unlock(&P->bgv_mx);
      free(it->recs); free(it->crcs); free(it);
      return 0;
    }
    P->bgv_started = true;
  }
  P->bgv_queue[P->bgv_tail++] = it;   /* at most one item per volume: MAX_VOLS slots suffice */
  pthread_cond_signal(&P->bgv_cv);
  pthread_mutex_unlock(&P->bgv_mx);
  return 0;
}

/* wait_queue: every queued volume is checked first (the end of a pack); otherwise the work is dropped (pause,
 * cancel, error). Returns -1 when a read-back found an error. */
static int bgv_shutdown(packer *P, bool wait_queue) {
  if (!P->bgv_started) return P->bgv_err ? -1 : 0;
  pthread_mutex_lock(&P->bgv_mx);
  P->bgv_stop = true;
  if (!wait_queue) P->bgv_stop_now = true;
  pthread_cond_broadcast(&P->bgv_cv);
  pthread_mutex_unlock(&P->bgv_mx);
  pthread_join(P->bgv_th, NULL);
  P->bgv_started = false;
  return P->bgv_err ? -1 : 0;
}

static int vol_finish(packer *P, volume *v, const uint8_t build_id[16]) {
  v->file_size = e2a_align_up(v->pos, E2A_PAGE);
  if (v->file_size < P->payload_offset + E2A_PAGE) v->file_size = P->payload_offset + E2A_PAGE;
  if (ftruncate(v->fd, (off_t)v->file_size) != 0) return -1;
  ampr_dat_header h;
  memset(&h, 0, sizeof h);
  memcpy(h.magic, AMPR_MAGIC_DAT, 8);
  h.version = 3; h.header_size = 64; h.pack_id = v->id; h.flags = v->flags;
  memcpy(h.build_id, build_id, 16);
  h.payload_offset = P->payload_offset;
  h.payload_bytes = v->file_size - P->payload_offset;
  h.header_crc = e2a_crc32(0, &h, sizeof h);
  if (e2a_pwrite_all(v->fd, &h, sizeof h, 0) != 0) return -1;
  if (fsync(v->fd) != 0) return -1;
  if (close(v->fd) != 0) return -1;
  v->fd = -1;
  return 0;
}

/* --------------------------------------------------------------- threads -- */
typedef struct { packer *P; int tid; int count; } wjob;

/* compress + fingerprint + crc of the blocks of the current batch */
static void *worker(void *arg) {
  wjob *w = arg;
  packer *P = w->P;
  int level = P->o->level;
  for (int i = w->tid; i < w->count; i += P->o->workers) {
    const char *src = (const char *)P->cur_raw + (size_t)i * P->block;
    uint32_t len = P->cur_len[i];
    char *dst = (char *)P->comp + (size_t)i * (size_t)P->bound;
    if (P->batch_store) P->comp_len[i] = 0;
    else if (level <= 4) P->comp_len[i] = LZ4_compress_fast(src, dst, (int)len, P->bound, 5 - level);
    else {
      /* a fast probe first (~0.1 ms): data LZ4 barely shrinks (textures, audio, archives) never reaches the
       * I/O-neutral thresholds with the slow HC pass either, so the block is stored as it is. The margin
       * follows what lz4_worth_it asks later: 1.5 % where a block must save 12.5 % (random/mixed), 0.4 %
       * where 1 % is enough (streaming), so no block HC would have kept is lost. */
      int fast = LZ4_compress_default(src, dst, (int)len, P->bound);
      uint32_t margin = P->layout == E2A_LAYOUT_STREAMING ? (len >> 8) : (len >> 6);
      if (fast <= 0 || (uint32_t)fast > len - margin) P->comp_len[i] = 0;
      else P->comp_len[i] = LZ4_compress_HC(src, dst, (int)len, P->bound, level);
    }
    P->raw_crc[i] = e2a_crc32(0, src, len);
    if (P->want_fp) {
      e2a_sha256 c;
      uint8_t d[32];
      e2a_sha256_init(&c);
      e2a_sha256_update(&c, src, len);
      e2a_sha256_final(&c, d);
      memcpy(P->fp + (size_t)i * 16, d, 16);
    }
  }
  return NULL;
}

static int compress_batch(packer *P, int count) {
  int nt = activity_workers(P->o->workers);   /* 2 while a game is running */
  if (nt > count) nt = count;
  pthread_t th[64];
  wjob jobs[64];
  for (int t = 0; t < nt; t++) {
    jobs[t].P = P; jobs[t].tid = t; jobs[t].count = count;
    if (pthread_create(&th[t], NULL, worker, &jobs[t]) != 0) {
      for (int k = 0; k < t; k++) pthread_join(th[k], NULL);
      return -1;
    }
  }
  for (int t = 0; t < nt; t++) pthread_join(th[t], NULL);
  return 0;
}

/* read-ahead: fills raw[k] while the main thread compresses and writes raw[k^1]. A batch is one range of the
 * file, fetched by E2A_READ_THREADS threads at once (a compressed image decodes on the thread that asks). */
static void *reader_fn(void *arg) {
  packer *P = arg;
  int k = 0;
  for (;;) {
    pthread_mutex_lock(&P->mx);
    while (P->bstate[k] != 0 && !P->reader_abort) pthread_cond_wait(&P->cv, &P->mx);
    if (P->reader_abort) { pthread_mutex_unlock(&P->mx); return NULL; }
    pthread_mutex_unlock(&P->mx);

    int count = 0;
    size_t want = (size_t)P->slots * P->block;
    if (P->rd_remaining < want) want = (size_t)P->rd_remaining;
    if (want) {
      double t0 = e2a_now();
      if (e2a_pread_parallel(P->rd_fd, P->raw[k], want, P->rd_pos, E2A_READ_THREADS) != 0) P->reader_err = 1;
      else {
        for (size_t done = 0; done < want; count++) {
          size_t l = want - done < P->block ? want - done : P->block;
          P->raw_len[k][count] = (uint32_t)l;
          done += l;
        }
        P->rd_pos += want;
        P->rd_remaining -= want;
        pthread_mutex_lock(&P->mx);
        P->t_read += e2a_now() - t0;
        P->b_read += want;
        pthread_mutex_unlock(&P->mx);
      }
    }
    bool last = P->rd_remaining == 0 || P->reader_err;
    pthread_mutex_lock(&P->mx);
    P->bcount[k] = count;
    P->bstate[k] = last ? 2 : 1;
    pthread_cond_broadcast(&P->cv);
    pthread_mutex_unlock(&P->mx);
    if (last) return NULL;
    k ^= 1;
  }
}

/* ------------------------------------------------------------- placement -- */
/* placement_for() in ampr_pack.py: streaming extents are dense (64 B aligned, extent start
 * page aligned); random/mixed chunks never straddle an I/O page, large ones start on a page. */
static uint64_t place(uint64_t pos, uint32_t stored, int layout, bool extent_start) {
  if (extent_start) pos = e2a_align_up(pos, E2A_PAGE);
  if (layout == E2A_LAYOUT_STREAMING) return e2a_align_up(pos, E2A_CHUNK_ALIGN);
  uint64_t off;
  if (stored >= E2A_PAGE) off = e2a_align_up(pos, E2A_PAGE);
  else {
    off = e2a_align_up(pos, E2A_CHUNK_ALIGN);
    if (off / E2A_PAGE != (off + stored - 1) / E2A_PAGE) off = e2a_align_up(off, E2A_PAGE);
  }
  return off;
}

/* _compress_block(): the stricter "I/O neutral" thresholds apply to random/mixed only */
static bool lz4_worth_it(uint32_t raw, int comp, int layout) {
  if (comp <= 0 || (uint32_t)comp >= raw) return false;
  uint32_t saved = raw - (uint32_t)comp;
  double ratio = (double)saved / (double)raw;
  uint32_t need_bytes = 64;
  double need_ratio = 0.01;
  uint32_t pages_raw = (raw + E2A_PAGE - 1) / E2A_PAGE, pages_comp = ((uint32_t)comp + E2A_PAGE - 1) / E2A_PAGE;
  if (layout != E2A_LAYOUT_STREAMING && pages_comp >= pages_raw) { need_bytes = 8192; need_ratio = 0.125; }
  return saved >= need_bytes && ratio >= need_ratio;
}

static void set_geometry(packer *P, const e2a_entry *e) {
  unsigned shift = e->block_shift ? e->block_shift : E2A_BLOCK_SHIFT;
  if (shift < 14) shift = 14;
  if (shift > 20) shift = 20;
  P->block = 1u << shift;
  P->bound = LZ4_compressBound((int)P->block);
  int slots = (int)(P->pool / P->block);
  int cap = P->o->workers * 64;   /* 6 workers: 24 MiB batches of 64 KiB blocks */
  if (slots > cap) slots = cap;
  if (slots < 2) slots = 2;
  P->slots = slots;
  P->layout = e->layout;
}

/* Sample blocks spread over a big file: keep it loose when LZ4 barely helps. The first blocks alone would
 * judge a compressible header and pack gigabytes of incompressible data behind it. */
static int auto_loose_check(packer *P, int fd, uint64_t size, bool *loose) {
  *loose = false;
  if (size < E2A_AUTO_LOOSE_MIN) return 0;
  int count = 0, want = P->slots < 32 ? P->slots : 32;
  uint64_t raw_total = 0, comp_total = 0, step = size / (uint64_t)want;
  if (step < P->block) step = P->block;
  step &= ~((uint64_t)P->block - 1);
  for (uint64_t off = 0; count < want && off + P->block <= size; off += step) {
    if (e2a_pread_all(fd, P->raw[0] + (size_t)count * P->block, P->block, off) != 0) return -1;
    P->raw_len[0][count] = P->block;
    count++;
  }
  if (count == 0) return 0;
  P->cur_raw = P->raw[0]; P->cur_len = P->raw_len[0]; P->batch_store = false;
  if (compress_batch(P, count) != 0) return -1;
  int raw_blocks = 0;
  for (int i = 0; i < count; i++) {
    bool worth = lz4_worth_it(P->raw_len[0][i], P->comp_len[i], P->layout);
    raw_total += P->raw_len[0][i];
    comp_total += worth ? (uint64_t)P->comp_len[i] : P->raw_len[0][i];
    if (!worth) raw_blocks++;
  }
  double saved = raw_total ? 1.0 - (double)comp_total / (double)raw_total : 0.0;
  if (saved < 0.05 || raw_blocks * 10 >= count * 9) *loose = true;   /* little gained, or 90 % of it stays raw */
  if (lseek(fd, 0, SEEK_SET) < 0) return -1;
  return 0;
}

/* ------------------------------------------------------------- staging -- */
/* one write per volume range: a passing hiccup of a USB drive gets three more tries; returns errno or 0 */
static int vol_write(packer *P, volume *v, const void *data, size_t len, uint64_t off) {
  int werr = 0;
  double t0 = e2a_now();
  for (int attempt = 0; attempt < 4; attempt++) {
    if (e2a_pwrite_all(v->fd, data, len, off) == 0) { werr = 0; break; }
    werr = errno;
    if (attempt == 3 || !(werr == EIO || werr == EAGAIN || werr == ETIMEDOUT || werr == EBUSY || werr == EINTR)) break;
    e2a_log("write failed on %s: %s (retry %d)", v->name, strerror(werr), attempt + 1);
    sleep(2);
  }
  P->t_write += e2a_now() - t0;
  P->b_write += len;
  if (werr) e2a_log("write failed on %s: %s", v->name, strerror(werr));
  return werr;
}

static int stg_flush(packer *P) {
  if (!P->stg_len) return 0;
  int werr = vol_write(P, P->stg_vol, P->stg, P->stg_len, P->stg_off);
  P->stg_len = 0;
  if (werr) P->stg_failed = true;
  return werr;
}

/* a block placed at off in volume v: appended to the staging buffer (gaps zeroed), flushed when the buffer
 * is full or the volume changes; the data reaches the disk in a few large writes instead of one per block */
static int stg_put(packer *P, volume *v, uint64_t off, const void *data, size_t len) {
  if (P->stg_len && (v != P->stg_vol || off < P->stg_off + P->stg_len || off + len - P->stg_off > P->stg_cap)) {
    int werr = stg_flush(P);
    if (werr) return werr;
  }
  if (!P->stg_len) { P->stg_off = off; P->stg_vol = v; }
  if (len > P->stg_cap) return vol_write(P, v, data, len, off);   /* never: a block is far smaller than the buffer */
  size_t at = (size_t)(off - P->stg_off);
  if (at > P->stg_len) memset(P->stg + P->stg_len, 0, at - P->stg_len);
  memcpy(P->stg + at, data, len);
  P->stg_len = at + len;
  return 0;
}

static pthread_mutex_t g_rates_mx = PTHREAD_MUTEX_INITIALIZER;
static e2a_pack_rates g_rates;

void e2a_pack_rates_get(e2a_pack_rates *out) {
  pthread_mutex_lock(&g_rates_mx);
  *out = g_rates;
  pthread_mutex_unlock(&g_rates_mx);
}

static void rates_set(double r, double c, double w, int threads, int workers) {
  pthread_mutex_lock(&g_rates_mx);
  g_rates.read_mbps = r; g_rates.compress_mbps = c; g_rates.write_mbps = w;
  g_rates.read_threads = threads; g_rates.workers = workers;
  pthread_mutex_unlock(&g_rates_mx);
}

/* what is packed so far against what it takes on disk (volumes included, page padding too): the forecast */
static void rate_totals(packer *P, uint64_t file_done) {
  uint64_t out = 0;
  for (uint32_t i = 0; i < P->nvols; i++) out += P->vols[i].pos;
  pthread_mutex_lock(&g_rates_mx);
  g_rates.in_bytes = P->packed_in + file_done;
  g_rates.out_bytes = out;
  g_rates.autoloose_bytes = P->autoloose;
  pthread_mutex_unlock(&g_rates_mx);
}

/* every few seconds the rates of the last window go to the clients; every two minutes a line goes to the log */
static void rate_log(packer *P, bool final) {
  double now = e2a_now();
  pthread_mutex_lock(&P->mx);   /* the reader thread updates these */
  double tr = P->t_read;
  uint64_t br = P->b_read;
  pthread_mutex_unlock(&P->mx);
  if (!br) { if (final) rates_set(0, 0, 0, 0, 0); return; }
  if (final) rates_set(0, 0, 0, 0, 0);
  else if (now - P->w_at >= 3.0) {
    double dtr = tr - P->w_tr, dtc = P->t_comp - P->w_tc, dtw = P->t_write - P->w_tw;
    rates_set(dtr > 0 ? (double)(br - P->w_br) / dtr / 1e6 : 0, dtc > 0 ? (double)(P->b_comp - P->w_bc) / dtc / 1e6 : 0,
              dtw > 0 ? (double)(P->b_write - P->w_bw) / dtw / 1e6 : 0, E2A_READ_THREADS, activity_workers(P->o->workers));
    P->w_at = now; P->w_tr = tr; P->w_tc = P->t_comp; P->w_tw = P->t_write; P->w_br = br; P->w_bc = P->b_comp; P->w_bw = P->b_write;
  }
  if (!final && now - P->t_rate_log < RATE_LOG_S) return;
  P->t_rate_log = now;
  e2a_log("pack stages: read %d MB/s (%d threads), compress %d MB/s (%d workers), write %d MB/s%s",
          tr > 0 ? (int)(br / tr / 1e6) : 0, E2A_READ_THREADS,
          P->t_comp > 0 ? (int)(P->b_comp / P->t_comp / 1e6) : 0, activity_workers(P->o->workers),
          P->t_write > 0 ? (int)(P->b_write / P->t_write / 1e6) : 0, final ? " (whole run)" : "");
}

/* ------------------------------------------------------------- pack file -- */
/* Returns E2A_PACK_OK, 1 (left loose), E2A_PACK_PAUSED (checkpoint saved) or E2A_PACK_ERROR. */
static int pack_one(packer *P, size_t index, uint64_t entry_count, uint64_t total_bytes, e2a_entry *e,
                    ampr_file_record *fr, uint64_t start_offset, uint32_t first_chunk0, uint32_t nchunks0) {
  e2a_pack_stats *st = P->st;
  const e2a_pack_opts *o = P->o;
  char path[1100];
  snprintf(path, sizeof path, "%s/%s", o->src_root, e->rel);
  int fd = open(path, O_RDONLY);
  if (fd < 0) { e2a_log("cannot open %s: %s", e->rel, strerror(errno)); return E2A_PACK_ERROR; }
  bool store = e->action == E2A_STORE;
  set_geometry(P, e);
  int layout = P->layout;
  bool dedup = o->dedup && layout != E2A_LAYOUT_STREAMING;   /* dense extents are not shared (ampr_pack.py) */
  P->want_fp = false;   /* auto-loose sampling does not need fingerprints */

  uint32_t first_chunk, nchunks;
  uint64_t file_done;
  if (start_offset == 0) {
    bool loose = false;
    /* hot files are never auto-loosed (auto_loose_hot_files = false) */
    if (!store && !e->hot && auto_loose_check(P, fd, e->size, &loose) != 0) { close(fd); return E2A_PACK_ERROR; }
    if (loose) {
      close(fd);
      e->action = E2A_LOOSE;
      e2a_log("auto-loose (incompressible): %s", e->rel);
      return 1;
    }
    first_chunk = (uint32_t)(P->chunks.n / sizeof(ampr_chunk_record));
    nchunks = 0;
    file_done = 0;
  } else {
    first_chunk = first_chunk0; nchunks = nchunks0; file_done = start_offset;
    if (lseek(fd, (off_t)start_offset, SEEK_SET) < 0) { close(fd); return E2A_PACK_ERROR; }
    e2a_log("resuming %s at %llu MiB", e->rel, (unsigned long long)(start_offset >> 20));
  }

  int rc = E2A_PACK_OK;
  P->want_fp = dedup;
  /* rd_remaining belongs to the reader once it runs: a small file can be read entirely before
   * this thread looks at it again, so the loop condition is taken from this local copy */
  const uint64_t to_read = e->size - file_done;
  P->rd_fd = fd; P->rd_remaining = to_read; P->rd_pos = file_done; P->reader_err = 0; P->reader_abort = 0;
  P->stg_len = 0; P->stg_failed = false;
  P->bstate[0] = P->bstate[1] = 0; P->bcount[0] = P->bcount[1] = 0;
  pthread_t reader;
  bool have_reader = false;
  if (to_read) {
    if (pthread_create(&reader, NULL, reader_fn, P) != 0) { close(fd); return E2A_PACK_ERROR; }
    have_reader = true;
  }

  int k = 0;
  bool last = to_read == 0;
  while (!last) {
    pthread_mutex_lock(&P->mx);
    while (P->bstate[k] == 0) pthread_cond_wait(&P->cv, &P->mx);
    int count = P->bcount[k];
    last = P->bstate[k] == 2;
    pthread_mutex_unlock(&P->mx);
    if (P->reader_err) {
      e2a_log("read error: %s", e->rel);
      /* the source drive was removed: stop cleanly, the checkpoint lets the conversion resume */
      if (!e2a_is_dir(o->src_root)) { e2a_pack_set_pause_reason("source disconnected: plug it back in, then resume"); rc = E2A_PACK_PAUSED; }
      else rc = E2A_PACK_ERROR;
      break;
    }

    if (count > 0) {
      P->cur_raw = P->raw[k]; P->cur_len = P->raw_len[k]; P->batch_store = store;
      double tc = e2a_now();
      if (compress_batch(P, count) != 0) { rc = E2A_PACK_ERROR; break; }
      P->t_comp += e2a_now() - tc;
      for (int i = 0; i < count; i++) P->b_comp += P->cur_len[i];
      for (int i = 0; i < count; i++) {
        uint32_t raw = P->cur_len[i];
        const uint8_t *fp = P->fp + (size_t)i * 16;
        const dd_rec *dup = dedup ? dd_find(P, fp, raw, store) : NULL;
        ampr_chunk_record cr = {0, 0};
        if (dup) {
          cr.location = dup->location;
          cr.descriptor = dup->descriptor | ((uint32_t)AMPR_CHUNK_SHARED << 22);
          st->chunks_shared++;
          st->shared_bytes += raw;
        } else {
          const uint8_t *data;
          uint32_t stored, codec;
          if (!store && lz4_worth_it(raw, P->comp_len[i], layout)) { data = P->comp + (size_t)i * (size_t)P->bound; stored = (uint32_t)P->comp_len[i]; codec = AMPR_CODEC_LZ4; }
          else { data = P->cur_raw + (size_t)i * P->block; stored = raw; codec = AMPR_CODEC_RAW; }
          bool extent_start = layout == E2A_LAYOUT_STREAMING && nchunks == 0;

          /* a full volume is left open and finalized with the build id at the end */
          if (!P->cur || e2a_align_up(P->cur->pos, E2A_PAGE) + stored > o->max_pack_size) {
            int werr = stg_flush(P);   /* the full volume must be complete on disk before it is read back */
            if (werr) { rc = storage_pause(werr) ? E2A_PACK_PAUSED : E2A_PACK_ERROR; break; }
            if (P->cur && bgv_enqueue(P, P->cur) != 0) { rc = E2A_PACK_ERROR; break; }   /* the full one is read back meanwhile */
            if (vol_open(P) != 0) { rc = E2A_PACK_ERROR; break; }
            extent_start = layout == E2A_LAYOUT_STREAMING;
          }
          volume *v = P->cur;
          uint64_t off = place(v->pos, stored, layout, extent_start);
          int werr = stg_put(P, v, off, data, stored);
          if (werr) {
            rc = storage_pause(werr) ? E2A_PACK_PAUSED : E2A_PACK_ERROR;
            break;
          }
          v->pos = off + stored;
          uint32_t cflags = layout == E2A_LAYOUT_STREAMING ? AMPR_CHUNK_STREAMING : 0;
          if (stored <= E2A_PAGE && off / E2A_PAGE == (off + stored - 1) / E2A_PAGE) cflags |= AMPR_CHUNK_PAGE_CONTAINED;
          if (off % E2A_PAGE == 0) cflags |= AMPR_CHUNK_PAGE_ALIGNED;
          cr.location = off | ((uint64_t)v->id << 48);
          cr.descriptor = (stored - 1) | (codec << 20) | (cflags << 22);
          if (codec == AMPR_CODEC_LZ4) st->chunks_lz4++; else st->chunks_raw++;
          st->stored_bytes += stored;
          if (dedup && dd_insert(P, fp, raw, store, cr.location, cr.descriptor) != 0) { rc = E2A_PACK_ERROR; break; }
        }
        if (bb_push(&P->chunks, &cr, sizeof cr) || bb_push(&P->crcs, &P->raw_crc[i], 4)) { rc = E2A_PACK_ERROR; break; }
        nchunks++;
        st->chunks++;
        file_done += raw;
      }
      if (rc == E2A_PACK_ERROR) break;
      if (rc == E2A_PACK_OK) {   /* the batch reaches the disk before anything (progress, checkpoint) counts on it */
        int werr = stg_flush(P);
        if (werr) { rc = storage_pause(werr) ? E2A_PACK_PAUSED : E2A_PACK_ERROR; if (rc == E2A_PACK_ERROR) break; }
      }
      rate_log(P, false);
      rate_totals(P, file_done);
      if (o->progress) o->progress(o->ctx, P->packed_in + file_done, e->rel);
      {
        uint64_t batch = 0;
        for (int i = 0; i < count; i++) batch += P->cur_len[i];
        activity_throttle(batch);   /* paces the disks while a game is running */
      }
      if (o->test_pause_after && o->pause && P->packed_in + file_done >= o->test_pause_after) *o->pause = 1;
    }

    pthread_mutex_lock(&P->mx);
    P->bstate[k] = 0;
    pthread_cond_broadcast(&P->cv);
    pthread_mutex_unlock(&P->mx);
    k ^= 1;

    if (rc == E2A_PACK_PAUSED) break;
    if (o->cancel && *o->cancel) { rc = E2A_PACK_ERROR; break; }
    if (o->pause && *o->pause) { rc = E2A_PACK_PAUSED; break; }
    if (P->bgv_err) {   /* a finished volume read back wrong: stop here rather than hours later */
      pthread_mutex_lock(&P->bgv_mx);
      e2a_log("%s", P->bgv_msg);
      pthread_mutex_unlock(&P->bgv_mx);
      rc = E2A_PACK_ERROR;
      break;
    }
    if (e2a_now() - P->last_ckpt > CKPT_INTERVAL_S && !P->stg_failed)
      checkpoint(P, index, file_done, first_chunk, nchunks, entry_count, total_bytes);  /* best effort */
  }
  if (rc == E2A_PACK_OK && !P->stg_failed) {
    int werr = stg_flush(P);
    if (werr) rc = storage_pause(werr) ? E2A_PACK_PAUSED : E2A_PACK_ERROR;
  }

  if (have_reader) {
    pthread_mutex_lock(&P->mx);
    P->reader_abort = 1;
    pthread_cond_broadcast(&P->cv);
    pthread_mutex_unlock(&P->mx);
    pthread_join(reader, NULL);
  }
  close(fd);

  if (rc == E2A_PACK_PAUSED) {
    /* after a failed flush the records of the last batch have no data on disk: the previous checkpoint (every
     * 10 s, taken after a flush) stays the one to resume from */
    if (P->stg_failed) { e2a_log("pause after a write failure: resuming from the last checkpoint"); return E2A_PACK_PAUSED; }
    if (checkpoint(P, index, file_done, first_chunk, nchunks, entry_count, total_bytes) != 0) return E2A_PACK_ERROR;
    return E2A_PACK_PAUSED;
  }
  if (rc != E2A_PACK_OK) return rc;
  if (file_done != e->size) {   /* never publish a packed file whose chunks do not cover it */
    e2a_log("internal: %s packed %llu of %llu bytes (%u chunks, block %u, slots %d, last %d, err %d, state %d/%d)", e->rel,
            (unsigned long long)file_done, (unsigned long long)e->size, nchunks, P->block, P->slots, (int)last,
            (int)P->reader_err, P->bstate[0], P->bstate[1]);
    return E2A_PACK_ERROR;
  }

  fr->logical_size = e->size;
  fr->first_chunk = first_chunk;
  fr->chunk_count = nchunks;
  fr->flags = AMPR_FILE_PACKED | (store ? AMPR_FILE_STORE_ONLY : 0) |
              (layout == E2A_LAYOUT_STREAMING ? AMPR_FILE_STREAMING : layout == E2A_LAYOUT_RANDOM ? AMPR_FILE_RANDOM_ACCESS : 0) |
              (e->hot ? AMPR_FILE_HOT : 0);
  fr->block_shift = (uint8_t)__builtin_ctz(P->block);
  fr->packing_class = 0;
  st->files_packed++;
  st->logical_bytes += e->size;
  return E2A_PACK_OK;
}

/* ------------------------------------------------------------------ main -- */
static int write_out(const char *dir, const char *name, const void *a, size_t na, const void *b, size_t nb) {
  char final[1100];
  snprintf(final, sizeof final, "%s/%s", dir, name);
  if (nb == 0) return e2a_write_file_atomic(final, a, na);
  uint8_t *buf = malloc(na + nb);
  if (!buf) return -1;
  memcpy(buf, a, na); memcpy(buf + na, b, nb);
  int rc = e2a_write_file_atomic(final, buf, na + nb);
  free(buf);
  return rc;
}

int e2a_pack(e2a_list *entries, const e2a_pack_opts *o, e2a_pack_stats *st) {
  memset(st, 0, sizeof *st);
  packer *P = calloc(1, sizeof *P);
  if (!P) return E2A_PACK_ERROR;
  P->o = o;
  P->st = st;
  P->payload_offset = (uint32_t)e2a_align_up(sizeof(ampr_dat_header), E2A_PAGE);
  /* fixed budget: 2 x 24 MiB input + 24 MiB (+LZ4 bound overhead) output + 26 MiB staging, whatever the block
   * size; a console short of memory gets the 8 MiB pools of the earlier versions */
  for (P->pool = POOL_BYTES; P->pool >= (8u << 20); P->pool /= 3) {
    for (int k = 0; k < 2; k++) { P->raw[k] = malloc(P->pool); P->raw_len[k] = calloc(MAX_SLOTS, sizeof(uint32_t)); }
    P->comp = malloc((size_t)P->pool + P->pool / 255 + (size_t)MAX_SLOTS * 16 + 4096);
    P->stg_cap = (size_t)P->pool + (2u << 20);   /* about a batch; a batch with many page gaps goes in two writes */
    P->stg = malloc(P->stg_cap);
    if (P->raw[0] && P->raw[1] && P->raw_len[0] && P->raw_len[1] && P->comp && P->stg) break;
    for (int k = 0; k < 2; k++) { free(P->raw[k]); free(P->raw_len[k]); P->raw[k] = NULL; P->raw_len[k] = NULL; }
    free(P->comp); free(P->stg); P->comp = NULL; P->stg = NULL;
    e2a_log("packer: %u MiB pools refused, trying smaller ones", P->pool >> 20);
  }
  P->t_rate_log = e2a_now();
  P->w_at = P->t_rate_log;
  rates_set(0, 0, 0, 0, 0);
  pthread_mutex_lock(&g_rates_mx);
  g_rates.in_bytes = g_rates.out_bytes = g_rates.autoloose_bytes = 0;
  pthread_mutex_unlock(&g_rates_mx);
  P->comp_len = calloc(MAX_SLOTS, sizeof(int));
  P->raw_crc = calloc(MAX_SLOTS, sizeof(uint32_t));
  P->fp = calloc(MAX_SLOTS, 16);
  P->entries_hash = entries_hash(entries);
  P->last_ckpt = e2a_now();
  pthread_mutex_init(&P->mx, NULL);
  pthread_cond_init(&P->cv, NULL);
  pthread_mutex_init(&P->bgv_mx, NULL);
  pthread_cond_init(&P->bgv_cv, NULL);
  for (uint32_t i = 0; i < MAX_VOLS; i++) P->vols[i].fd = -1;
  int rc = E2A_PACK_ERROR;
  bytebuf payload = {0};
  if (!P->raw[0] || !P->raw[1] || !P->raw_len[0] || !P->raw_len[1] || !P->comp || !P->comp_len || !P->raw_crc || !P->fp || !P->stg) goto out;

  size_t start = 0;
  uint64_t cur_offset = 0;
  uint32_t cur_first_chunk = 0, cur_nchunks = 0;
  if (o->resume) {
    e2a_ckpt *c = malloc(sizeof *c);
    if (!c || ckpt_read(o->state_dir, c) != 0) { e2a_log("no usable checkpoint in %s", o->state_dir ? o->state_dir : "?"); free(c); goto out; }
    if (c->entry_count != entries->n || c->total_bytes != entries->total_bytes || c->entries_hash != P->entries_hash) {
      e2a_log("source changed since the checkpoint (files, sizes or dates differ): cannot resume");
      free(c); goto out;
    }
    if (load_whole(o->state_dir, "files.bin", &P->files, c->files_len) || load_whole(o->state_dir, "chunks.bin", &P->chunks, c->chunks_len) ||
        load_whole(o->state_dir, "crcs.bin", &P->crcs, c->crcs_len) || load_whole(o->state_dir, "strings.bin", &P->strings, c->strings_len) ||
        load_whole(o->state_dir, "dedup.bin", &P->dedup, c->dedup_len)) {
      e2a_log("checkpoint buffers unreadable"); free(c); goto out;
    }
    if (P->dedup.n) {
      size_t cap = 1 << 16;
      while (cap < (P->dedup.n / sizeof(dd_rec)) * 2) cap *= 2;
      if (dd_rehash(P, cap) != 0) { free(c); goto out; }
    }
    P->nvols = c->nvols;  /* before the loop, so the cleanup closes what was reopened on failure */
    for (uint32_t i = 0; i < c->nvols; i++) {
      if (vol_reopen(P, i, c->vol_pos[i], c->vol_flags[i]) != 0) { free(c); goto out; }
    }
    P->cur = c->nvols ? &P->vols[c->nvols - 1] : NULL;
    P->packed_in = c->packed_in;
    *st = c->st;
    start = (size_t)c->next_entry;
    cur_offset = c->cur_offset; cur_first_chunk = c->cur_first_chunk; cur_nchunks = c->cur_nchunks;
    P->ck_files = P->files.n; P->ck_chunks = P->chunks.n; P->ck_crcs = P->crcs.n; P->ck_strings = P->strings.n; P->ck_dedup = P->dedup.n;
    /* entries already finished are loose or packed according to their saved record */
    const ampr_file_record *recs = (const ampr_file_record *)P->files.p;
    for (size_t i = 0; i < start && i < entries->n; i++) {
      if (!(recs[i].flags & AMPR_FILE_PACKED)) entries->v[i].action = E2A_LOOSE;
      else entries->v[i].action = (recs[i].flags & AMPR_FILE_STORE_ONLY) ? E2A_STORE : E2A_COMPRESS;
    }
    e2a_log("resumed at entry %zu/%zu, %u volumes, %llu MiB already packed", start, entries->n, P->nvols,
            (unsigned long long)(P->packed_in >> 20));
    free(c);
  }

  /* file records, in index order */
  for (size_t i = start; i < entries->n; i++) {
    e2a_entry *e = &entries->v[i];
    ampr_file_record fr;
    memset(&fr, 0, sizeof fr);
    char canon[1100];
    snprintf(canon, sizeof canon, "/app0/%s", e->rel);
    fr.path_hash = e2a_fnv1a64_fold(canon, strlen(canon));
    fr.mtime = e->mtime;
    fr.logical_size = e->size;
    /* the resume position only applies to the first entry of this run */
    uint64_t so = cur_offset;
    uint32_t fc = cur_first_chunk, nc = cur_nchunks;
    cur_offset = 0; cur_first_chunk = 0; cur_nchunks = 0;
    if (e->action != E2A_LOOSE) {
      int r = pack_one(P, i, entries->n, entries->total_bytes, e, &fr, so, fc, nc);
      if (r == E2A_PACK_PAUSED) { rc = E2A_PACK_PAUSED; goto out; }
      if (r < 0) goto out;
      if (r == 1) { fr.flags = 0; fr.chunk_count = 0; fr.first_chunk = 0; fr.block_shift = 0; st->files_loose++; P->autoloose += e->size; }
      else P->packed_in += e->size;
      rate_totals(P, 0);
    } else {
      st->files_loose++;
    }
    int32_t off = strtab_add(&P->strings, canon);
    if (off < 0) goto out;
    fr.path_offset = (uint32_t)off;
    fr.path_length = (uint32_t)strlen(canon);
    if (bb_push(&P->files, &fr, sizeof fr)) goto out;
    if (o->cancel && *o->cancel) { e2a_log("cancelled"); goto out; }
    if (o->pause && *o->pause) {
      if (checkpoint(P, i + 1, 0, 0, 0, entries->n, entries->total_bytes) != 0) goto out;
      rc = E2A_PACK_PAUSED; goto out;
    }
    if (e2a_now() - P->last_ckpt > CKPT_INTERVAL_S) checkpoint(P, i + 1, 0, 0, 0, entries->n, entries->total_bytes);
  }
  if (bgv_shutdown(P, true) != 0) { e2a_log("%s", P->bgv_msg); goto out; }   /* every queued volume checked before the manifest */
  if (P->nvols == 0 && vol_open(P) != 0) goto out;  /* nothing compressible: still emit a valid empty volume */

  /* pack records (names into the string table) */
  for (uint32_t i = 0; i < P->nvols; i++) {
    volume *v = &P->vols[i];
    v->file_size = e2a_align_up(v->pos, E2A_PAGE);
    if (v->file_size < P->payload_offset + E2A_PAGE) v->file_size = P->payload_offset + E2A_PAGE;
    ampr_pack_record pr;
    memset(&pr, 0, sizeof pr);
    pr.payload_bytes = v->file_size - P->payload_offset;
    pr.file_size = v->file_size;
    int32_t off = strtab_add(&P->strings, v->name);
    if (off < 0) goto out;
    pr.name_offset = (uint32_t)off;
    pr.name_length = (uint32_t)strlen(v->name);
    pr.flags = v->flags;
    pr.io_page_size = E2A_PAGE;
    if (bb_push(&P->packs, &pr, sizeof pr)) goto out;
  }

  /* build id: sha256("AMPRPACK4\0" + len/bytes of each part), first 16 bytes */
  uint8_t build_id[16];
  {
    e2a_sha256 c;
    uint8_t digest[32];
    e2a_sha256_init(&c);
    e2a_sha256_update(&c, "AMPRPACK4\0", 10);
    const bytebuf *parts[] = {&P->files, &P->chunks, &P->crcs, &P->packs, &P->strings};
    for (size_t i = 0; i < 5; i++) {
      uint64_t len = parts[i]->n;
      e2a_sha256_update(&c, &len, 8);
      if (parts[i]->n) e2a_sha256_update(&c, parts[i]->p, parts[i]->n);
    }
    e2a_sha256_final(&c, digest);
    memcpy(build_id, digest, 16);
  }

  /* finalize volumes */
  for (uint32_t i = 0; i < P->nvols; i++) {
    if (vol_finish(P, &P->vols[i], build_id) != 0) { e2a_log("finalize %s failed: %s", P->vols[i].name, strerror(errno)); goto out; }
  }

  /* manifest */
  ampr_pak_header h;
  memset(&h, 0, sizeof h);
  memcpy(h.magic, AMPR_MAGIC_PAK, 8);
  h.version = 4; h.header_size = 128; h.flags = 0; h.endian = AMPR_ENDIAN_MARK;
  memcpy(h.build_id, build_id, 16);
  h.file_count = P->files.n / sizeof(ampr_file_record);
  h.chunk_count = P->chunks.n / sizeof(ampr_chunk_record);
  h.pack_count = P->nvols;
  h.file_record_size = 48; h.chunk_record_size = 12; h.pack_record_size = 32;
  h.files_offset = 128;
  h.chunks_offset = h.files_offset + P->files.n;
  h.packs_offset = h.chunks_offset + P->chunks.n;
  h.strings_offset = h.packs_offset + P->packs.n;
  h.strings_size = P->strings.n;
  if (bb_push(&payload, P->files.p, P->files.n) || bb_push(&payload, P->chunks.p, P->chunks.n) ||
      bb_push(&payload, P->packs.p, P->packs.n) || bb_push(&payload, P->strings.p, P->strings.n)) goto out;
  h.payload_crc = e2a_crc32(0, payload.p, payload.n);
  h.header_crc = e2a_crc32(0, &h, sizeof h);

  /* crc sidecar */
  ampr_crc_header ch;
  memset(&ch, 0, sizeof ch);
  memcpy(ch.magic, AMPR_MAGIC_CRC, 8);
  ch.version = 1; ch.header_size = 48;
  memcpy(ch.build_id, build_id, 16);
  ch.chunk_count = P->crcs.n / 4;
  ch.payload_crc = e2a_crc32(0, P->crcs.p, P->crcs.n);
  ch.header_crc = e2a_crc32(0, &ch, sizeof ch);

  /* runtime settings (same values Lazy_AMPR uses by default) */
  ampr_cfg_record cfg;
  memset(&cfg, 0, sizeof cfg);
  memcpy(cfg.magic, AMPR_MAGIC_CFG, 8);
  cfg.version = 1; cfg.size = 64;
  memcpy(cfg.build_id, build_id, 16);
  /* defaults = Lazy_AMPR / ampr_pack example; overridable by the UI or the profile [runtime] */
  cfg.decoded_cache_bytes = o->rt_decoded ? o->rt_decoded : 128ull << 20;
  cfg.physical_cache_bytes = o->rt_physical ? o->rt_physical : 32ull << 20;
  cfg.workers = o->rt_workers ? o->rt_workers : 4;
  cfg.latency_reserve_workers = o->rt_workers ? o->rt_reserve : 1;
  /* the emulator's pool is 384 MiB: 32 windows of 2 MiB, 1 MiB per worker and 32 MiB of reserve, then this
   * manifest, its own index and the two caches; a cache that does not fit is halved until it does (down to
   * 16 MiB), so on a big game a "large" setting would end up smaller than the standard one: bounded here */
  {
    uint64_t pool = 384ull << 20, fixed = (64ull << 20) + ((uint64_t)cfg.workers << 20) + (32ull << 20);
    uint64_t manifest = sizeof h + payload.n, index = 16ull * (uint64_t)h.chunk_count + 64ull * (uint64_t)h.file_count;
    uint64_t used = fixed + manifest + index + cfg.physical_cache_bytes + (16ull << 20);
    uint64_t room = pool > used ? (pool - used) & ~((1ull << 20) - 1) : 0;
    if (room < (16ull << 20)) room = 16ull << 20;
    if (cfg.decoded_cache_bytes > room) {
      e2a_log("runtime: decoded cache %llu MiB -> %llu MiB (the emulator's pool: manifest %llu MiB, index %llu MiB, physical cache %llu MiB)",
              (unsigned long long)(cfg.decoded_cache_bytes >> 20), (unsigned long long)(room >> 20), (unsigned long long)(manifest >> 20),
              (unsigned long long)(index >> 20), (unsigned long long)(cfg.physical_cache_bytes >> 20));
      cfg.decoded_cache_bytes = room;
    }
  }
  cfg.checksum = e2a_crc32(0, &cfg, sizeof cfg);

  /* publish: volumes, crc, runtime, index last */
  for (uint32_t i = 0; i < P->nvols; i++) {
    char tmp[1100], final[1100];
    snprintf(final, sizeof final, "%s/%s", o->out_dir, P->vols[i].name);
    vol_path(P, &P->vols[i], tmp, sizeof tmp);
    if (rename(tmp, final) != 0) { e2a_log("rename %s failed", P->vols[i].name); goto out; }
  }
  if (write_out(o->out_dir, "ampr_assets.index.crc", &ch, sizeof ch, P->crcs.p, P->crcs.n) ||
      write_out(o->out_dir, "ampr_assets.index.runtime", &cfg, sizeof cfg, NULL, 0) ||
      write_out(o->out_dir, "ampr_assets.index", &h, sizeof h, payload.p, payload.n)) {
    e2a_log("writing manifest failed: %s", strerror(errno));
    goto out;
  }
  st->packs = P->nvols;
  e2a_log("pack: %llu files packed, %llu loose, %llu chunks (%llu lz4 / %llu raw / %llu shared), %u volumes",
          (unsigned long long)st->files_packed, (unsigned long long)st->files_loose, (unsigned long long)st->chunks,
          (unsigned long long)st->chunks_lz4, (unsigned long long)st->chunks_raw, (unsigned long long)st->chunks_shared, P->nvols);
  rc = E2A_PACK_OK;

out:
  bgv_shutdown(P, false);   /* a pause, a cancel or an error: the read-back still queued is dropped */
  free(payload.p);
  for (uint32_t i = 0; i < P->nvols; i++) {
    if (P->vols[i].fd >= 0) {
      if (rc == E2A_PACK_PAUSED) fsync(P->vols[i].fd);
      close(P->vols[i].fd);
      if (rc == E2A_PACK_ERROR && o->cancel && *o->cancel) {
        char tmp[1100];
        vol_path(P, &P->vols[i], tmp, sizeof tmp);
        unlink(tmp);
      }
    }
  }
  rate_log(P, true);   /* the report takes mx: before it is destroyed */
  pthread_mutex_destroy(&P->mx);
  pthread_cond_destroy(&P->cv);
  pthread_mutex_destroy(&P->bgv_mx);
  pthread_cond_destroy(&P->bgv_cv);
  for (int k = 0; k < 2; k++) { free(P->raw[k]); free(P->raw_len[k]); }
  free(P->stg);
  free(P->comp); free(P->comp_len); free(P->raw_crc); free(P->fp); free(P->dd_tab);
  free(P->files.p); free(P->chunks.p); free(P->packs.p); free(P->strings.p); free(P->crcs.p); free(P->dedup.p);
  free(P);
  return rc;
}

/* ---------------------------------------------------------------- loose -- */
int e2a_copy_loose(const e2a_list *l, const e2a_pack_opts *o) {
  uint64_t done = 0;
  for (size_t i = 0; i < l->n; i++) {
    const e2a_entry *e = &l->v[i];
    if (e->action != E2A_LOOSE || e->synthetic) continue;
    if (o->cancel && *o->cancel) return E2A_PACK_ERROR;
    if (o->pause && *o->pause) return E2A_PACK_PAUSED;
    char src[1100], dst[1100];
    snprintf(src, sizeof src, "%s/%s", o->src_root, e->rel);
    snprintf(dst, sizeof dst, "%s/%s", o->out_dir, e->rel);
    struct stat st;
    if (stat(dst, &st) == 0 && S_ISREG(st.st_mode) && (uint64_t)st.st_size == e->size) {
      done += e->size;  /* already copied by a previous run */
      if (o->skip) o->skip(o->ctx, e->size);
      continue;
    }
    char *slash = strrchr(dst, '/');
    if (slash) { *slash = 0; if (e2a_mkdir_p(dst) != 0) { e2a_log("mkdir failed: %s", dst); return E2A_PACK_ERROR; } *slash = '/'; }
    if (e2a_copy_file(src, dst, e->mtime) != 0) {
      int err = errno;
      e2a_log("copy failed: %s (%s)", e->rel, strerror(err));
      if (storage_pause(err)) return E2A_PACK_PAUSED;
      return E2A_PACK_ERROR;
    }
    done += e->size;
    if (o->progress) o->progress(o->ctx, done, e->rel);
  }
  return E2A_PACK_OK;
}
