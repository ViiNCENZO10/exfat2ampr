/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * Linux side of platform.h, for the CI: the same app code runs against tests/mock_engine.py and a
 * scripted controller, and writes screenshots (PPM) of the screens it reaches.
 *
 *   E2A_SCRIPT="wait:800 shot:games down down square cross wait:300 shot:options circle r1 shot:progress"
 *   E2A_SHOTS=shots/  E2A_ASSETS=assets/  E2A_ENGINE_PORT=6972  E2A_LANG=2  E2A_ICON=app/sce_sys/icon0.png
 *
 * Demo recording (both on the same clock, muxed afterwards):
 *   E2A_VIDEO=demo.mp4      every presented frame, 1280x720 at 30 fps, through ffmpeg
 *   E2A_VIDEO_FULL=1        the same at the screen's own 1920x1080 (the presentation video)
 *   E2A_AUDIO_WAV=demo.wav  what the app's mixer plays, 48 kHz stereo */
#include "../src/platform.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static const uint32_t *g_last;
static char *g_steps[512];
static int g_nsteps, g_step;
static uint64_t g_wait_until, g_t0;
static uint32_t g_press;

uint64_t plat_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
int plat_pid(void) { return (int)getpid(); }

/* ------------------------------------------------------------------ video -- */
#define VFPS 30
static int VW = 1280, VH = 720;   /* E2A_VIDEO_FULL: SCREEN_W x SCREEN_H */
static FILE *g_video;
static uint8_t *g_vframe;
static uint64_t g_vframes;

/* 1920x1080 -> 1280x720: every 3x3 block becomes 2x2 (exact area weights 2:1 / 1:2) */
static void downscale(const uint32_t *src) {
  for (int by = 0; by < VH / 2; by++)
    for (int bx = 0; bx < VW / 2; bx++)
      for (int ch = 0; ch < 3; ch++) {
        int s[3][3], h[3][2];
        for (int r = 0; r < 3; r++)
          for (int c = 0; c < 3; c++) s[r][c] = (int)(src[(size_t)(3 * by + r) * SCREEN_W + 3 * bx + c] >> (8 * ch)) & 0xff;
        for (int r = 0; r < 3; r++) { h[r][0] = 2 * s[r][0] + s[r][1]; h[r][1] = s[r][1] + 2 * s[r][2]; }
        for (int c = 0; c < 2; c++) {
          g_vframe[((size_t)(2 * by) * VW + 2 * bx + c) * 3 + ch] = (uint8_t)((2 * h[0][c] + h[1][c] + 4) / 9);
          g_vframe[((size_t)(2 * by + 1) * VW + 2 * bx + c) * 3 + ch] = (uint8_t)((h[1][c] + 2 * h[2][c] + 4) / 9);
        }
      }
}

/* 1920x1080 as it is: 0xAABBGGRR -> RGB */
static void convert_full(const uint32_t *src) {
  for (size_t i = 0; i < (size_t)SCREEN_W * SCREEN_H; i++) {
    uint32_t c = src[i];
    g_vframe[i * 3] = (uint8_t)(c & 0xff);
    g_vframe[i * 3 + 1] = (uint8_t)(c >> 8 & 0xff);
    g_vframe[i * 3 + 2] = (uint8_t)(c >> 16 & 0xff);
  }
}

/* frames are written on the wall clock: what is on screen is repeated while the app is idle */
static void video_pump(void) {
  if (!g_video) return;
  uint64_t due = (plat_ms() - g_t0) * VFPS / 1000 + 1;
  while (g_vframes < due) {
    fwrite(g_vframe, 1, (size_t)VW * VH * 3, g_video);
    g_vframes++;
  }
}

/* ------------------------------------------------------------------ audio -- */
static FILE *g_wav;
static uint64_t g_wav_frames;
static volatile int g_audio_stop;
static pthread_t g_audio_th;
static void (*g_mix)(int16_t *lr, int frames);

static void wav_header(FILE *f, uint32_t data_bytes) {
  const uint32_t rate = 48000, byte_rate = 48000 * 4, fmt_len = 16, riff_len = 36 + data_bytes;
  const uint16_t pcm = 1, channels = 2, block = 4, bits = 16;
  fwrite("RIFF", 1, 4, f); fwrite(&riff_len, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
  fwrite(&fmt_len, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&channels, 2, 1, f); fwrite(&rate, 4, 1, f);
  fwrite(&byte_rate, 4, 1, f); fwrite(&block, 2, 1, f); fwrite(&bits, 2, 1, f);
  fwrite("data", 1, 4, f); fwrite(&data_bytes, 4, 1, f);
}

/* plays the console's audio thread: 256-frame grains, as many as the wall clock asks for */
static void *audio_thread(void *arg) {
  (void)arg;
  static int16_t grain[256 * 2];
  while (!g_audio_stop) {
    uint64_t due = (plat_ms() - g_t0) * 48;
    while (g_wav_frames + 256 <= due) {
      g_mix(grain, 256);
      fwrite(grain, 4, 256, g_wav);
      g_wav_frames += 256;
    }
    usleep(2000);
  }
  return NULL;
}

int plat_audio_start(void (*mix)(int16_t *lr, int frames)) {
  const char *path = getenv("E2A_AUDIO_WAV");
  if (!path || !*path || g_wav) return -1;   /* no sound device in the CI */
  g_wav = fopen(path, "wb");
  if (!g_wav) return -1;
  wav_header(g_wav, 0);
  g_mix = mix;
  if (pthread_create(&g_audio_th, NULL, audio_thread, NULL) != 0) { fclose(g_wav); g_wav = NULL; return -1; }
  return 0;
}

static void host_finish(void) {
  if (g_wav) {
    g_audio_stop = 1;
    pthread_join(g_audio_th, NULL);
    fseek(g_wav, 0, SEEK_SET);
    wav_header(g_wav, (uint32_t)(g_wav_frames * 4));
    fclose(g_wav);
    g_wav = NULL;
    fprintf(stderr, "[host] audio: %.1f s\n", (double)g_wav_frames / 48000);
  }
  if (g_video) {
    video_pump();
    pclose(g_video);
    g_video = NULL;
    fprintf(stderr, "[host] video: %llu frames\n", (unsigned long long)g_vframes);
  }
}

/* ------------------------------------------------------------------- base -- */
void plat_sleep_ms(unsigned ms) {
  video_pump();
  usleep(ms * 1000u);
}

int plat_init(void) {
  g_t0 = plat_ms();
  const char *s = getenv("E2A_SCRIPT");
  char *copy = strdup(s ? s : "wait:1000 shot:screen"), *save = NULL;
  for (char *t = strtok_r(copy, " \n", &save); t && g_nsteps < 512; t = strtok_r(NULL, " \n", &save)) g_steps[g_nsteps++] = t;
  const char *video = getenv("E2A_VIDEO");
  if (video && *video) {
    const char *full = getenv("E2A_VIDEO_FULL");
    if (full && *full == '1') { VW = SCREEN_W; VH = SCREEN_H; }
    char cmd[1024];
    snprintf(cmd, sizeof cmd,
             "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgb24 -s %dx%d -r %d -i - -c:v libx264 -preset veryfast -crf %d "
             "-pix_fmt yuv420p '%s'", VW, VH, VFPS, VW == SCREEN_W ? 16 : 20, video);
    signal(SIGPIPE, SIG_IGN);   /* ffmpeg missing or stopped: no crash, the screenshots still come */
    g_vframe = calloc((size_t)VW * VH, 3);
    g_video = g_vframe ? popen(cmd, "w") : NULL;
    if (!g_video) fprintf(stderr, "[host] video recording unavailable\n");
  }
  atexit(host_finish);
  return 0;
}

static void shot(const char *name) {
  if (!g_last) return;
  const char *dir = getenv("E2A_SHOTS");
  char path[512];
  snprintf(path, sizeof path, "%s/%s.ppm", dir ? dir : ".", name);
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
  for (size_t i = 0; i < (size_t)SCREEN_W * SCREEN_H; i++) {
    uint32_t c = g_last[i];
    unsigned char rgb[3] = {(unsigned char)(c & 0xff), (unsigned char)(c >> 8 & 0xff), (unsigned char)(c >> 16 & 0xff)};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
  fprintf(stderr, "[host] screenshot %s\n", path);
}

void plat_present(const uint32_t *pixels) {
  g_last = pixels;
  if (g_video) {
    if (VW == SCREEN_W) convert_full(pixels);
    else downscale(pixels);
    video_pump();
  }
}

static uint32_t button(const char *n) {
  static const struct { const char *name; uint32_t bit; } map[] = {
    {"up", BTN_UP}, {"down", BTN_DOWN}, {"left", BTN_LEFT}, {"right", BTN_RIGHT}, {"cross", BTN_CROSS}, {"circle", BTN_CIRCLE},
    {"square", BTN_SQUARE}, {"triangle", BTN_TRIANGLE}, {"l1", BTN_L1}, {"r1", BTN_R1}, {"options", BTN_OPTIONS}};
  for (size_t i = 0; i < sizeof map / sizeof *map; i++)
    if (!strcmp(map[i].name, n)) return map[i].bit;
  fprintf(stderr, "[host] unknown button %s\n", n);
  return 0;
}

/* one script step per call: a press lasts one call, then the buttons are released for one call */
uint32_t plat_buttons(int *lx, int *ly) {
  if (lx) *lx = 0;
  if (ly) *ly = 0;
  if (g_press) { g_press = 0; return 0; }
  if (plat_ms() < g_wait_until) return 0;
  if (g_step >= g_nsteps) { fprintf(stderr, "[host] script finished\n"); exit(0); }
  const char *t = g_steps[g_step++];
  if (!strncmp(t, "wait:", 5)) { g_wait_until = plat_ms() + (uint64_t)atoi(t + 5); return 0; }
  if (!strncmp(t, "shot:", 5)) { shot(t + 5); return 0; }
  bool quick = t[0] == '!';   /* "!r1": the next step comes at once (screenshots in the middle of a motion) */
  g_press = button(quick ? t + 1 : t);
  g_wait_until = plat_ms() + (quick ? 20 : 250);   /* let the app react and redraw */
  return g_press;
}

void plat_notify(const char *msg) { fprintf(stderr, "[notify] %s\n", msg); }
int plat_language(void) { const char *l = getenv("E2A_LANG"); return l ? atoi(l) : 2; }
void plat_exit(void) { fprintf(stderr, "[host] exit requested\n"); exit(0); }

int plat_tcp_connect(uint16_t port, unsigned timeout_ms) {
  const char *ep = getenv("E2A_ENGINE_PORT");
  if (port == 6972 && ep) port = (uint16_t)atoi(ep);
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return -1;
  struct timeval tv = {(time_t)(timeout_ms / 1000 + 5), 0};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) { close(s); return -1; }
  return s;
}
int plat_tcp_send(int s, const void *buf, size_t len) { return (int)send(s, buf, len, MSG_NOSIGNAL); }
int plat_tcp_recv(int s, void *buf, size_t len) { return (int)recv(s, buf, len, 0); }
void plat_tcp_close(int s) { close(s); }

int plat_read_file(const char *path, uint8_t **data, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *b = malloc(n > 0 ? (size_t)n : 1);
  if (!b || (n > 0 && fread(b, 1, (size_t)n, f) != (size_t)n)) { free(b); fclose(f); return -1; }
  fclose(f);
  *data = b; *len = (size_t)n;
  return 0;
}
int plat_write_file(const char *path, const void *data, size_t len) {
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  int rc = fwrite(data, 1, len, f) == len ? 0 : -1;
  fclose(f);
  return rc;
}
/* the CI: curl stands in for the console's sceHttp (the mock engine serves the manifest and the package) */
int plat_https_get(const char *url, int (*sink)(void *ctx, const uint8_t *data, size_t len), void *ctx, bool follow) {
  (void)follow;
  if (strchr(url, '\'') || strlen(url) > 900) return -1;
  char cmd[1024];
  snprintf(cmd, sizeof cmd, "curl -fsSL --max-time 300 '%s'", url);
  FILE *p = popen(cmd, "r");
  if (!p) return -1;
  static uint8_t buf[64 * 1024];
  size_t n;
  bool stopped = false;
  while (!stopped && (n = fread(buf, 1, sizeof buf, p)) > 0) stopped = sink(ctx, buf, n) != 0;
  int rc = pclose(p);
  return stopped || rc != 0 ? -1 : 200;
}
unsigned plat_https_last_error(void) { return 0; }

const char *plat_asset_dir(void) { const char *a = getenv("E2A_ASSETS"); return a ? a : "assets"; }
const char *plat_settings_path(void) { return "/tmp/e2a_app_settings.txt"; }
const char *plat_icon_path(void) { const char *i = getenv("E2A_ICON"); return i ? i : "app/sce_sys/icon0.png"; }
