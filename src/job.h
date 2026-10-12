/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE. */
#ifndef E2A_JOB_H
#define E2A_JOB_H

#include "common.h"

typedef enum { JOB_IDLE = 0, JOB_RUNNING, JOB_DONE, JOB_ERROR, JOB_CANCELLED, JOB_PAUSED } job_state;

#define E2A_REQ_MAGIC "E2AREQ05"
#define E2A_REQ_MAGIC_V4 "E2AREQ04"
#define E2A_QUEUE_MAX 16

typedef struct {
  char magic[8];        /* E2A_REQ_MAGIC when persisted */
  char source[512];     /* game root to convert, or the image file when smp_title is set */
  char smp_title[16];   /* image managed by ShadowMountPlus: mounted through its API for the job */
  char dest_base[256];  /* storage folder, e.g. /data/homebrew or /mnt/usb0/homebrew */
  char name[64];        /* destination folder name (default: title id) */
  int level;            /* LZ4 level 1-12 */
  int workers;
  bool clean_leftovers; /* skip _DUPLEX_ and <project>/Saved folders */
  bool overwrite;       /* replace an existing <dest_base>/<name> */
  bool remove_source;   /* delete the source folder after a successful conversion */
  bool resume;          /* continue an interrupted conversion found in <dest_base>/<name> */
  bool verify;          /* decode every block after packing */
  bool dedup;           /* share identical blocks */
  int cache_mode;       /* emulator cache: 0 standard (128/32 MiB, 4 workers), 1 large (192/64 MiB, 6 workers) */
  uint64_t test_pause_after; /* test hook (host build) */
  /* E2AREQ05 */
  bool force;           /* convert even when the game does not use AMPR (page only, at your own risk) */
  uint8_t reserved[63];
} job_request;

typedef struct {
  job_state state;
  char phase[64];
  char message[256];                  /* English detail (log, older clients) */
  char code[32];                      /* stable reason: the app and the page word it in their own language */
  char reason[32];                    /* ShadowMountPlus error_reason while waiting for it (game_active, ...) */
  bool waiting;                       /* waiting for ShadowMountPlus to mount the image (no tight loop) */
  int percent;
  uint64_t bytes_total, bytes_done;   /* overall (pack + loose copy [+ verify]) */
  uint64_t phase_base;                /* bytes completed in previous phases */
  double speed_bps;                   /* current throughput */
  int64_t eta_seconds;                /* -1 while unknown */
  char dest[600];
  char title_id[16];
  char title[128];
  char profile[320];                  /* profile file applied, empty = blind */
  uint64_t source_bytes, packed_bytes, shared_bytes;
  /* forecasts: the image file (0 for a folder), the converted size expected (refined while packing) */
  uint64_t image_bytes, estimated_bytes, pack_total, loose_total, verify_bytes;
  double est_ratio;
  char est_method[12];
  uint64_t started, finished;         /* unix seconds */
  int queue_len;
  char queue[E2A_QUEUE_MAX][96];
} job_status;

int  job_init(void);                        /* scheduler thread + runtime detection */
int  job_enqueue(const job_request *req);   /* position in queue (0 = starts now), -1 when full */
int  job_resume(const char *dest);          /* enqueue the request saved in <dest>/.e2a-state */
/* level 1..12: the LZ4 level for what is left (0 keeps it); -1 nothing to resume, -2 already running or queued, -3 queue full */
int  job_resume_level(const char *dest, int level);
int  job_discard(const char *dest);         /* delete an interrupted conversion */
void job_cancel(void);                      /* abort the current job and delete its destination */
void job_pause(void);                       /* checkpoint and stop the current job; resume later */
void job_pause_because(const char *why);    /* same, with the reason shown to the user */
void job_watch_storage(void);               /* pauses the job when its source or destination drive is gone */
/* checks before converting (JSON with error / warning codes) */
int job_preflight(const char *path, const char *smp, const char *dest_base, char *json, size_t cap);
void job_get_status(job_status *out);
bool job_running(void);
bool job_busy(void);                        /* running or queue not empty */
bool job_can_retry(void);                   /* the last conversion failed or was cancelled */
int  job_retry(void);                       /* start it again (from its checkpoint when there is one) */
int  job_dismiss(void);                     /* forget the last result (done, error, cancelled, paused) */
/* delete a copy from the source list (duplicate, old version): a game or app folder or an image one
 * level inside the homebrew folders, or an image ShadowMountPlus lists; refused (code set) when a
 * conversion, a running title, this app or ShadowMountPlus uses it. Runs on its own thread. */
int  job_delete_source(const char *path, char *code, size_t cap);
/* the last deletion, as JSON members for /api/state ("deleting":...,"delete_seq":...) */
void job_delete_status_json(char *out, size_t cap);

/* JSON for the web UI */
int job_list_sources(char *json, size_t cap);
int job_list_destinations(char *json, size_t cap);
int job_list_resumable(char *json, size_t cap);
int job_list_profiles(char *json, size_t cap);
int job_runtime_json(char *json, size_t cap);
/* the source or destination of a conversion that runs, waits in the queue or is paused: nobody else may touch it */
bool job_path_busy(const char *path);


#endif
