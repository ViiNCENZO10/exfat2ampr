/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * What the app needs from the system. platform_ps5.c implements it on the console
 * (VideoOut, scePad, sceNet, AudioOut); app/host/platform_host.c renders screenshots, a demo
 * video and its sound track on Linux. */
#ifndef E2A_APP_PLATFORM_H
#define E2A_APP_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCREEN_W 1920
#define SCREEN_H 1080

/* DualSense buttons (scePad bit layout) */
enum {
  BTN_L3 = 0x2, BTN_R3 = 0x4, BTN_OPTIONS = 0x8,
  BTN_UP = 0x10, BTN_RIGHT = 0x20, BTN_DOWN = 0x40, BTN_LEFT = 0x80,
  BTN_L2 = 0x100, BTN_R2 = 0x200, BTN_L1 = 0x400, BTN_R1 = 0x800,
  BTN_TRIANGLE = 0x1000, BTN_CIRCLE = 0x2000, BTN_CROSS = 0x4000, BTN_SQUARE = 0x8000,
  BTN_TOUCHPAD = 0x100000,
};

int plat_init(void);                          /* video + controller; < 0 on failure */
void plat_present(const uint32_t *pixels);    /* SCREEN_W x SCREEN_H, 0xAABBGGRR */
uint32_t plat_buttons(int *lx, int *ly);      /* held buttons, left stick -128..127 */
uint64_t plat_ms(void);
int plat_pid(void);
void plat_sleep_ms(unsigned ms);
void plat_notify(const char *msg);
int plat_language(void);                      /* system language id (PS4/PS5 numbering) */
void plat_exit(void);                         /* close the app */

/* TCP to 127.0.0.1 */
int plat_tcp_connect(uint16_t port, unsigned timeout_ms);
int plat_tcp_send(int s, const void *buf, size_t len);   /* bytes sent or < 0 */
int plat_tcp_recv(int s, void *buf, size_t len);         /* bytes, 0 at end, < 0 on error */
void plat_tcp_close(int s);

/* files: whole-file read (malloc'd), atomic small writes for the settings */
int plat_read_file(const char *path, uint8_t **data, size_t *len);
int plat_write_file(const char *path, const void *data, size_t len);
const char *plat_asset_dir(void);             /* packaged read-only files (engine payload, version) */
const char *plat_settings_path(void);
const char *plat_icon_path(void);             /* the app's own icon0.png */

/* HTTPS GET (the app's update): every piece of the body goes to sink (non-zero stops the transfer); follow:
 * redirects followed (a release download). Returns the HTTP status, < 0 when no server answers. */
int plat_https_get(const char *url, int (*sink)(void *ctx, const uint8_t *data, size_t len), void *ctx, bool follow);
/* the library's error code of the last plat_https_get that failed (0 when there is none): shown to the user */
unsigned plat_https_last_error(void);

/* sound: the platform's audio thread pulls 48 kHz stereo 16-bit samples from mix(); < 0 = no output */
int plat_audio_start(void (*mix)(int16_t *lr, int frames));

#endif
