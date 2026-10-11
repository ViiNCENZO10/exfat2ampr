/* exfat2ampr - on-console AMPR LZ4 packer for PS5 (ps5-payload-sdk).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef E2A_COMMON_H
#define E2A_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifndef E2A_VERSION
#define E2A_VERSION "dev"
#endif

#ifdef E2A_HOST
/* host build (Linux CI): everything relative to the working directory */
#define E2A_HOMEBREW_DIR "homebrew"
#define E2A_STATE_DIR "e2a-state"
#define E2A_SPRX_SMP "e2a-state/libSceAmpr.sprx"
#define E2A_SMP_MOUNT_DIR "e2a-state/shadowmnt"
#define E2A_APPMETA_DIR "e2a-state/appmeta"
#else
#define E2A_HOMEBREW_DIR "/data/homebrew"
#define E2A_STATE_DIR "/data/exfat2ampr"
#define E2A_SPRX_SMP "/data/shadowmount/emus/libSceAmpr.sprx"
#define E2A_SMP_MOUNT_DIR "/mnt/shadowmnt"
#define E2A_APPMETA_DIR "/user/appmeta"   /* the system's copy of every registered title's icon0.png / pic0.png */
#endif
#define E2A_LOG_FILE E2A_STATE_DIR "/exfat2ampr.log"
#define E2A_PROFILE_DIR E2A_STATE_DIR "/profiles"
#define E2A_AMPR_RUNTIME_VERSION "0.4.2.1"   /* embedded libSceAmpr.sprx (drakmor/ampr_emu release) */
#define E2A_AMPR_FORMAT "AMPRPAK4"

#define E2A_HTTP_PORT 6972

#define E2A_PAGE 65536u          /* io_page_size and payload alignment */
#define E2A_CHUNK_ALIGN 64u
#define E2A_BLOCK_SHIFT 16       /* 64 KiB blocks */
#define E2A_MAX_PACK (16ull << 30)
#define E2A_AUTO_LOOSE_MIN (64ull << 20)

static inline uint64_t e2a_align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

/* util.c */
void e2a_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void e2a_notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  e2a_mkdir_p(const char *path);
int  e2a_rm_rf(const char *path);
int  e2a_copy_file(const char *src, const char *dst, int64_t mtime);
extern void (*e2a_copy_hook)(uint64_t bytes);   /* called per copied buffer (game mode pacing) */
int  e2a_write_all(int fd, const void *buf, size_t len);
int  e2a_pwrite_all(int fd, const void *buf, size_t len, uint64_t off);
int  e2a_pread_all(int fd, void *buf, size_t len, uint64_t off);
int  e2a_read_full(int fd, void *buf, size_t len, size_t *got);
int  e2a_read_text(const char *path, char **out, size_t *len, size_t max);
int  e2a_write_file_atomic(const char *path, const void *data, size_t len);
int  e2a_is_dir(const char *path);
int  e2a_is_file(const char *path);
int64_t e2a_file_size(const char *path);
uint32_t e2a_crc32(uint32_t crc, const void *buf, size_t len);
uint64_t e2a_fnv1a64_fold(const void *buf, size_t len);
void e2a_fold_ascii(char *s);
double e2a_now(void);
void e2a_log_tail_copy(char *out, size_t cap);  /* last lines of the in-memory log (for the web UI) */
void e2a_json_escape(const char *in, char *out, size_t cap);

/* sha256.c */
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t buflen; } e2a_sha256;
void e2a_sha256_init(e2a_sha256 *c);
void e2a_sha256_update(e2a_sha256 *c, const void *data, size_t len);
void e2a_sha256_final(e2a_sha256 *c, uint8_t out[32]);
int  e2a_sha256_file(const char *path, char hex[65]);

#endif
