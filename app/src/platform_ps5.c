/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * Console side of platform.h. The VideoOut setup and the tiled framebuffer layout are adapted
 * from blackbearreloaded/ps5-native-app-boilerplate (src/demo_renderer.cpp, GPL-3.0-or-later,
 * hardware-validated on firmware 6.02 and 12.70); the scePad sample layout and the sceNet socket
 * options come from its examples (self-update, sandbox-elevation). AudioOut follows the
 * boilerplate's recipe (48 kHz, 256-frame grains, sceAudioOutOutput paces the thread); the main
 * port opens with the system user id on PS5 (measured by the m8c PS5 port), the logged-in user
 * is the fallback. */
#ifndef E2A_APP_HOST
#include "platform.h"
#include "log.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

size_t sceKernelGetDirectMemorySize(void);
int sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t len, size_t align, int type, int64_t *phys);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, int64_t phys, size_t align);
int sceKernelSendNotificationRequest(int device, void *request, size_t size, int blocking);
int sceKernelUsleep(unsigned int us);
int sceKernelDebugOutText(int channel, const char *text);   /* the text goes to the kernel log as it is (no format) */
uint64_t sceKernelGetProcessTime(void);
int sceSystemServiceHideSplashScreen(void);
int sceSystemServiceLoadExec(const char *path, const char **argv);
int sceSystemServiceParamGetInt(int id, int *value);
int sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void *param);
int sceVideoOutSetFlipRate(int32_t handle, int32_t rate);
int sceVideoOutSubmitFlip(int32_t handle, int32_t index, uint32_t mode, int64_t arg);
int sceVideoOutWaitVblank(int32_t handle);
int sceUserServiceInitialize(const void *params);
int sceUserServiceGetInitialUser(int *user);
int scePadInit(void);
int scePadOpen(int user, int type, int index, const void *params);
int sceNetSocket(const char *name, int domain, int type, int protocol);
int sceNetConnect(int s, const void *addr, uint32_t len);
int sceNetSend(int s, const void *buf, size_t len, int flags);
int sceNetRecv(int s, void *buf, size_t len, int flags);
int sceNetSetsockopt(int s, int level, int opt, const void *val, uint32_t size);
int sceNetSocketClose(int s);
int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetPoolDestroy(int pool);
int sceSslInit(size_t pool_size);
int sceSslTerm(int context);
int sceHttpInit(int net_pool, int ssl_context, size_t pool_size);
int sceHttpTerm(int context);
int sceHttpCreateTemplate(int context, const char *user_agent, int version, int auto_proxy);
int sceHttpDeleteTemplate(int template_id);
int sceHttpCreateConnectionWithURL(int template_id, const char *url, int keep_alive);
int sceHttpDeleteConnection(int connection);
int sceHttpCreateRequestWithURL(int connection, int method, const char *url, uint64_t content_length);
int sceHttpDeleteRequest(int request);
int sceHttpSetAutoRedirect(int id, int enabled);
int sceHttpSetResolveTimeOut(int id, uint32_t usec);
int sceHttpSetConnectTimeOut(int id, uint32_t usec);
int sceHttpSetSendTimeOut(int id, uint32_t usec);
int sceHttpSetRecvTimeOut(int id, uint32_t usec);
int sceHttpsEnableOption(int id, uint32_t flags);
int sceHttpSendRequest(int request, const void *data, size_t size);
int sceHttpGetStatusCode(int request, int *status);
int sceHttpGetAllResponseHeaders(int request, char **headers, size_t *size);
int sceHttpReadData(int request, void *data, size_t size);
int sceAudioOutInit(void);
int sceAudioOutOpen(int user, int type, int index, uint32_t len, uint32_t freq, uint32_t param);
int sceAudioOutOutput(int handle, const void *ptr);

typedef struct { void *data, *metadata, *reserved0, *reserved1; } video_buffer;
typedef struct { uint8_t reserved[80]; } video_attribute;
void sceVideoOutSetBufferAttribute2(video_attribute *attr, uint64_t format, uint32_t tiling, uint32_t width, uint32_t height,
                                    uint64_t option, uint32_t dcc_control, uint64_t dcc_clear);
int sceVideoOutRegisterBuffers2(int32_t handle, int32_t set, int32_t start, video_buffer *buffers, int32_t count,
                                video_attribute *attr, int32_t category, void *option);

/* scePadRead sample (120 bytes) */
typedef struct {
  uint32_t buttons;
  uint8_t sticks[6];          /* lx, ly, rx, ry, l2, r2 */
  uint8_t reserved0[66];
  int32_t connected;
  uint64_t timestamp_us;
  uint8_t reserved1[32];
} pad_sample;
_Static_assert(sizeof(pad_sample) == 120, "scePadRead sample");
int scePadRead(int handle, pad_sample *samples, int count);

typedef struct { uint8_t len, family; uint16_t port; uint32_t addr; uint16_t vport; uint8_t zero[6]; } net_sockaddr_in;

#define FRAME_BYTES 0x1000000u
#define MEM_BYTES (FRAME_BYTES * 2)
#define MEM_ALIGN 0x200000u
#define PIXEL_RGBA8_SRGB 0x8000000022000000ull

static int g_video = -1;
static void *g_vram[2];
static int64_t g_frame = 1;
static uint32_t g_xo[SCREEN_W], g_yo[SCREEN_H], g_xb[SCREEN_W], g_yb[SCREEN_H];
static int g_pad = -1;
static uint64_t g_pad_retry;
static uint32_t g_held;
static int g_lx, g_ly;

static void flush_range(void *addr, size_t len) {
  uint8_t *p = addr, *end = p + len;
  for (; p < end; p += 64) __asm__ volatile("clflush (%0)" : : "r"(p) : "memory");
  __asm__ volatile("mfence" ::: "memory");
}

/* byte offset of pixel (x, y) in the tiled scan-out buffer: 64 KiB blocks of 128x128 pixels,
 * swizzled inside a block = (xo[x] ^ yo[y]) + block base xb[x] + yb[y] */
static void build_tiling(void) {
  const uint32_t blocks_per_row = (SCREEN_W + 127u) >> 7;
  for (uint32_t x = 0; x < SCREEN_W; x++)
    g_xo[x] = (((x << 2) & 0xcu) ^ ((x << 5) & 0x380u) ^ ((x << 4) & 0x400u) ^ ((x << 6) & 0x800u) ^ ((x << 9) & 0xa000u));
  for (uint32_t x = 0; x < SCREEN_W; x++) g_xb[x] = (x >> 7) << 16;
  for (uint32_t y = 0; y < SCREEN_H; y++)
    g_yo[y] = (((y << 4) & 0x70u) ^ ((y << 5) & 0xf00u) ^ ((y << 9) & 0x1000u) ^ ((y << 8) & 0x4000u));
  for (uint32_t y = 0; y < SCREEN_H; y++) g_yb[y] = ((y >> 7) * blocks_per_row) << 16;
}

int plat_init(void) {
  (void)sceSystemServiceHideSplashScreen();
  build_tiling();
  g_video = sceVideoOutOpen(0xff, 0, 0, NULL);
  if (g_video < 0) { plat_notify("exFAT2AMPR: sceVideoOutOpen failed"); return -1; }
  size_t pool = sceKernelGetDirectMemorySize();
  int64_t phys = 0;
  if (pool < MEM_BYTES || sceKernelAllocateDirectMemory(0, (int64_t)pool, MEM_BYTES, MEM_ALIGN, 3, &phys) < 0) {
    plat_notify("exFAT2AMPR: no video memory");
    return -1;
  }
  void *mapped = NULL;
  if (sceKernelMapDirectMemory(&mapped, MEM_BYTES, 0x33, 0, phys, MEM_ALIGN) < 0) { plat_notify("exFAT2AMPR: map failed"); return -1; }
  memset(mapped, 0, MEM_BYTES);
  flush_range(mapped, MEM_BYTES);
  g_vram[0] = mapped;
  g_vram[1] = (uint8_t *)mapped + FRAME_BYTES;
  video_buffer buffers[2] = {{g_vram[0], NULL, NULL, NULL}, {g_vram[1], NULL, NULL, NULL}};
  video_attribute attr;
  memset(&attr, 0, sizeof attr);
  (void)sceVideoOutSetFlipRate(g_video, 0);
  sceVideoOutSetBufferAttribute2(&attr, PIXEL_RGBA8_SRGB, 0, SCREEN_W, SCREEN_H, 0, 0, 0);
  if (sceVideoOutRegisterBuffers2(g_video, 0, 0, buffers, 2, &attr, 0, NULL) < 0) { plat_notify("exFAT2AMPR: buffer registration failed"); return -1; }
  if (sceVideoOutSubmitFlip(g_video, 0, 1, 1) < 0) return -1;
  (void)sceVideoOutWaitVblank(g_video);
  (void)sceUserServiceInitialize(NULL);
  (void)scePadInit();
  return 0;
}

void plat_present(const uint32_t *pixels) {
  g_frame++;
  int back = (int)(g_frame & 1) ^ 1;
  uint8_t *dst = g_vram[back];
  for (uint32_t y = 0; y < SCREEN_H; y++) {
    const uint32_t *row = pixels + (size_t)y * SCREEN_W;
    const uint32_t yo = g_yo[y];
    uint8_t *base = dst + g_yb[y];
    for (uint32_t x = 0; x < SCREEN_W; x++) *(uint32_t *)(base + g_xb[x] + (g_xo[x] ^ yo)) = row[x];
  }
  flush_range(dst, FRAME_BYTES);
  if (sceVideoOutSubmitFlip(g_video, back, 1, g_frame) < 0) (void)sceKernelUsleep(4000);
  (void)sceVideoOutWaitVblank(g_video);
}

uint32_t plat_buttons(int *lx, int *ly) {
  if (g_pad < 0 && plat_ms() >= g_pad_retry) {
    int user = 0;
    if (sceUserServiceGetInitialUser(&user) == 0) g_pad = scePadOpen(user, 0, 0, NULL);
    g_pad_retry = plat_ms() + 1000;
  }
  if (g_pad >= 0) {
    static pad_sample samples[16];
    int n = scePadRead(g_pad, samples, 16);
    if (n > 0) {
      g_held = samples[n - 1].buttons;
      g_lx = (int)samples[n - 1].sticks[0] - 128;
      g_ly = (int)samples[n - 1].sticks[1] - 128;
    }
  }
  if (lx) *lx = g_lx;
  if (ly) *ly = g_ly;
  return g_held;
}

uint64_t plat_ms(void) { return sceKernelGetProcessTime() / 1000u; }
int plat_pid(void) { return (int)getpid(); }
void plat_sleep_ms(unsigned ms) { (void)sceKernelUsleep(ms * 1000u); }

void plat_debug_text(const char *line) {
  char buf[APP_LOG_LEN + 24];
  snprintf(buf, sizeof buf, "[exfat2ampr] %.200s\n", line);
  (void)sceKernelDebugOutText(0, buf);
}

void plat_notify(const char *msg) {
  static struct { char unused[45]; char message[3075]; } req;
  memset(&req, 0, sizeof req);
  snprintf(req.message, sizeof req.message, "%s", msg);
  (void)sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

int plat_language(void) {
  int v = 1;
  if (sceSystemServiceParamGetInt(1, &v) != 0) v = 1;   /* SCE_SYSTEM_SERVICE_PARAM_ID_LANG */
  return v;
}

void plat_exit(void) {
  /* a native title ends through the system, never by returning from main */
  (void)sceSystemServiceLoadExec("exit", NULL);
  for (;;) (void)sceKernelUsleep(1000000);
}

int plat_tcp_connect(uint16_t port, unsigned timeout_ms) {
  int s = sceNetSocket("exfat2ampr", 2, 1, 6);
  if (s < 0) return -1;
  uint32_t io_us = 15000000u, conn_us = timeout_ms * 1000u;
  sceNetSetsockopt(s, 0xffff, 0x1105, &io_us, sizeof io_us);    /* send timeout */
  sceNetSetsockopt(s, 0xffff, 0x1106, &io_us, sizeof io_us);    /* receive timeout */
  sceNetSetsockopt(s, 0xffff, 0x1109, &conn_us, sizeof conn_us); /* connect timeout */
  net_sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.len = sizeof a; a.family = 2;
  a.port = (uint16_t)((port << 8) | (port >> 8));
  a.addr = 0x0100007fu;   /* 127.0.0.1 */
  if (sceNetConnect(s, &a, sizeof a) < 0) { sceNetSocketClose(s); return -1; }
  return s;
}
int plat_tcp_send(int s, const void *buf, size_t len) { return sceNetSend(s, buf, len, 0); }
int plat_tcp_recv(int s, void *buf, size_t len) { return sceNetRecv(s, buf, len, 0); }
void plat_tcp_close(int s) { sceNetSocketClose(s); }

int plat_read_file(const char *path, uint8_t **data, size_t *len) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  size_t cap = 1 << 16, n = 0;
  uint8_t *buf = malloc(cap);
  for (;;) {
    if (!buf) { close(fd); return -1; }
    if (n == cap) {
      uint8_t *nb = realloc(buf, cap * 2);
      if (!nb) { free(buf); close(fd); return -1; }
      buf = nb; cap *= 2;
    }
    ssize_t r = read(fd, buf + n, cap - n);
    if (r < 0) { free(buf); close(fd); return -1; }
    if (r == 0) break;
    n += (size_t)r;
  }
  close(fd);
  *data = buf; *len = n;
  return 0;
}

int plat_write_file(const char *path, const void *data, size_t len) {
  char tmp[256];
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return -1;
  int rc = write(fd, data, len) == (ssize_t)len ? 0 : -1;
  if (close(fd) != 0) rc = -1;
  if (rc == 0 && rename(tmp, path) != 0) rc = -1;
  return rc;
}

const char *plat_asset_dir(void) { return "/app0/assets"; }
const char *plat_settings_path(void) { return "/download0/settings.txt"; }

/* The console's HTTPS: sceHttp and sceSsl with the system's certificate store. Sequence and option values
 * from ps5-native-app-boilerplate's examples/update-check (UPDATE_CHECK_USE_SCEHTTP, GPL-3.0-or-later),
 * proven on hardware for sandboxed apps like this one. */
static unsigned g_https_err;   /* the library's error code of the last failure, for the message shown */
unsigned plat_https_last_error(void) { return g_https_err; }

/* GET over HTTPS through the console's own library, the body handed to sink as it arrives. A redirect (a
 * GitHub release download answers 302 towards objects.githubusercontent.com) is followed by the library
 * when it does so, and by hand otherwise: up to five hops, read from the Location header. */
int plat_https_get(const char *url0, int (*sink)(void *ctx, const uint8_t *data, size_t len), void *ctx, bool follow) {
  enum { NET_POOL = 1024 * 1024, SSL_POOL = 304 * 1024, HTTP_POOL = 4 * 1024 * 1024, HTTP_1_1 = 2, GET = 0,
         VERIFY = 0x01 | 0x04 | 0x08 | 0x10 | 0x20 | 0x80 };   /* server, name, dates, known CA, SNI */
  static char url[2048];   /* one transfer at a time: the update thread (a signed download URL is long) */
  int pool = -1, ssl = -1, http = -1, tmpl = -1, conn = -1, req = -1, status = 0, rc;
  uint32_t recv_us = follow ? 60000000u : 8000000u;
  g_https_err = 0;
  if (!url0 || strncmp(url0, "https://", 8) != 0 || strlen(url0) >= sizeof url) {
    app_log("https: invalid or insecure address refused");
    return -1;
  }
  snprintf(url, sizeof url, "%s", url0);
  rc = pool = sceNetPoolCreate("e2a_https", NET_POOL, 0);
  if (rc >= 0) rc = ssl = sceSslInit(SSL_POOL);
  if (rc >= 0) rc = http = sceHttpInit(pool, ssl, HTTP_POOL);
  if (rc >= 0) rc = tmpl = sceHttpCreateTemplate(http, "exFAT2AMPR", HTTP_1_1, 0);
  /* the library's own redirect following fails on GitHub's signed download addresses (a 302 to
   * release-assets.githubusercontent.com with a 900-byte query): it is switched off and the loop below
   * follows by hand, one plain request per hop */
  if (rc >= 0) (void)sceHttpSetAutoRedirect(tmpl, 0);
  if (rc >= 0) rc = sceHttpSetResolveTimeOut(tmpl, 8000000u);
  if (rc >= 0) rc = sceHttpSetConnectTimeOut(tmpl, 8000000u);
  if (rc >= 0) rc = sceHttpSetSendTimeOut(tmpl, 8000000u);
  if (rc >= 0) rc = sceHttpSetRecvTimeOut(tmpl, recv_us);
  if (rc >= 0) rc = sceHttpsEnableOption(tmpl, VERIFY);
  char host[80];
  if (rc < 0) app_log("https: setup failed 0x%08x", (unsigned)rc);
  for (int hop = 0; rc >= 0 && hop < 6; hop++) {
    rc = conn = sceHttpCreateConnectionWithURL(tmpl, url, 0);
    if (rc >= 0) rc = req = sceHttpCreateRequestWithURL(conn, GET, url, 0);
    if (rc >= 0) rc = sceHttpSendRequest(req, NULL, 0);
    if (rc >= 0) rc = sceHttpGetStatusCode(req, &status);
    if (rc < 0) { app_log("https: %s failed 0x%08x (hop %d)", app_log_host(url, host, sizeof host), (unsigned)rc, hop); break; }
    app_log("https: %s -> %d (hop %d)", app_log_host(url, host, sizeof host), status, hop);
    if (!follow || (status != 301 && status != 302 && status != 303 && status != 307 && status != 308)) break;
    /* the library left the redirect to us: the Location header, then the same request on the new address */
    char *hdr = NULL;
    size_t hlen = 0;
    bool found = false;
    if (sceHttpGetAllResponseHeaders(req, &hdr, &hlen) >= 0 && hdr) {
      for (size_t i = 0; i + 9 < hlen; i++) {
        if (i > 0 && hdr[i - 1] != '\n') continue;
        bool name = true;   /* "Location:" in any case */
        for (int c = 0; c < 9 && name; c++) name = (hdr[i + c] | 0x20) == ("location:"[c] | 0x20);
        if (!name) continue;
        size_t s = i + 9, e;
        while (s < hlen && (hdr[s] == ' ' || hdr[s] == '\t')) s++;
        for (e = s; e < hlen && hdr[e] != '\r' && hdr[e] != '\n'; e++) {}
        while (e > s && (hdr[e - 1] == ' ' || hdr[e - 1] == '\t')) e--;
        if (e - s >= 8 && e - s < sizeof url && strncasecmp(hdr + s, "https://", 8) == 0) {   /* never down to clear HTTP */
          memcpy(url, hdr + s, e - s);
          url[e - s] = 0;
          found = true;
        }
        break;
      }
    }
    sceHttpDeleteRequest(req); req = -1;
    sceHttpDeleteConnection(conn); conn = -1;
    if (!found) { app_log("https: redirect without a usable Location header"); break; }   /* the 3xx status is reported as it is */
  }
  if (rc >= 0 && status == 200) {
    static uint8_t buf[64 * 1024];
    unsigned long long total = 0;
    for (;;) {
      int got = sceHttpReadData(req, buf, sizeof buf);
      if (got < 0) { rc = got; app_log("https: read failed 0x%08x after %llu bytes", (unsigned)got, total); break; }
      if (got == 0) break;
      total += (unsigned long long)got;
      if (sink(ctx, buf, (size_t)got) != 0) { rc = -1; app_log("https: stopped by the receiver after %llu bytes", total); break; }
    }
    if (rc >= 0) app_log("https: %llu bytes received", total);
  }
  if (rc < -1) g_https_err = (unsigned)rc;
  if (req >= 0) sceHttpDeleteRequest(req);
  if (conn >= 0) sceHttpDeleteConnection(conn);
  if (tmpl >= 0) sceHttpDeleteTemplate(tmpl);
  if (http >= 0) sceHttpTerm(http);
  if (ssl >= 0) sceSslTerm(ssl);
  if (pool >= 0) sceNetPoolDestroy(pool);
  return rc < 0 ? (rc < -1 ? -2 : -1) : status;
}
const char *plat_icon_path(void) { return "/app0/sce_sys/icon0.png"; }

/* ------------------------------------------------------------------ audio -- */
#define AUDIO_GRAIN 256
#define AUDIO_PORT_MAIN 0
#define AUDIO_S16_STEREO 1
#define AUDIO_USER_SYSTEM 0xff

static int g_audio = -1;
static void (*g_mix)(int16_t *lr, int frames);

static void *audio_thread(void *arg) {
  (void)arg;
  static int16_t grain[AUDIO_GRAIN * 2] __attribute__((aligned(64)));
  for (;;) {
    g_mix(grain, AUDIO_GRAIN);
    if (sceAudioOutOutput(g_audio, grain) < 0) (void)sceKernelUsleep(5000);   /* blocks until the grain is played */
  }
  return NULL;
}

int plat_audio_start(void (*mix)(int16_t *lr, int frames)) {
  (void)sceAudioOutInit();   /* "already initialised" is fine: the open below is the real test */
  int h = sceAudioOutOpen(AUDIO_USER_SYSTEM, AUDIO_PORT_MAIN, 0, AUDIO_GRAIN, 48000, AUDIO_S16_STEREO);
  if (h <= 0) {
    int user = 0;
    if (sceUserServiceGetInitialUser(&user) == 0) h = sceAudioOutOpen(user, AUDIO_PORT_MAIN, 0, AUDIO_GRAIN, 48000, AUDIO_S16_STEREO);
  }
  if (h <= 0) return -1;
  g_audio = h;
  g_mix = mix;
  pthread_t th;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  int rc = pthread_create(&th, &at, audio_thread, NULL);
  pthread_attr_destroy(&at);
  return rc == 0 ? 0 : -1;
}
#endif
