/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pair.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PAIR_FILE E2A_STATE_DIR "/paired.txt"
#define MAX_TOKENS 32
#define MAX_FAILS 5
#define BLOCK_S 60

static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static char g_pin[12];
static char g_tok[MAX_TOKENS][33];
static int g_ntok, g_fails;
static time_t g_block_until;

static void new_pin(void) { snprintf(g_pin, sizeof g_pin, "%06u", (unsigned)arc4random_uniform(1000000)); }

static void save(void) {
  char buf[MAX_TOKENS * 34 + 1];
  size_t n = 0;
  for (int i = 0; i < g_ntok; i++) n += (size_t)snprintf(buf + n, sizeof buf - n, "%s\n", g_tok[i]);
  e2a_write_file_atomic(PAIR_FILE, buf, n);
}

void pair_init(void) {
  pthread_mutex_lock(&g_mx);
  new_pin();
  g_ntok = 0;
  char *doc = NULL;
  if (e2a_read_text(PAIR_FILE, &doc, NULL, MAX_TOKENS * 34) == 0) {
    for (char *line = doc; *line && g_ntok < MAX_TOKENS;) {
      char *end = strchr(line, '\n');
      size_t len = end ? (size_t)(end - line) : strlen(line);
      if (len == 32) { memcpy(g_tok[g_ntok], line, 32); g_tok[g_ntok][32] = 0; g_ntok++; }
      if (!end) break;
      line = end + 1;
    }
    free(doc);
  }
  pthread_mutex_unlock(&g_mx);
}

const char *pair_pin(void) { return g_pin; }

static bool same(const char *a, const char *b, size_t n) {   /* constant time */
  unsigned char d = 0;
  for (size_t i = 0; i < n; i++) d |= (unsigned char)(a[i] ^ b[i]);
  return d == 0;
}

bool pair_token_ok(const char *token) {
  if (!token || strlen(token) != 32) return false;
  bool ok = false;
  pthread_mutex_lock(&g_mx);
  for (int i = 0; i < g_ntok; i++) ok |= same(g_tok[i], token, 32);
  pthread_mutex_unlock(&g_mx);
  return ok;
}

int pair_register(const char *pin, char token[65]) {
  int rc;
  pthread_mutex_lock(&g_mx);
  time_t now = time(NULL);
  if (now < g_block_until) rc = -2;
  else if (!pin || strlen(pin) != 6 || !same(pin, g_pin, 6)) {
    rc = -1;
    if (++g_fails >= MAX_FAILS) { g_fails = 0; g_block_until = now + BLOCK_S; new_pin(); }   /* guessing: new code */
  } else {
    uint8_t raw[16];
    arc4random_buf(raw, sizeof raw);
    for (int i = 0; i < 16; i++) snprintf(token + i * 2, 3, "%02x", raw[i]);
    if (g_ntok == MAX_TOKENS) { memmove(g_tok[0], g_tok[1], sizeof g_tok[0] * (MAX_TOKENS - 1)); g_ntok--; }   /* drop the oldest */
    memcpy(g_tok[g_ntok], token, 33);
    g_ntok++;
    g_fails = 0;
    save();
    rc = 0;
  }
  pthread_mutex_unlock(&g_mx);
  if (rc == 0) e2a_log("pairing: new device paired (%d in total)", g_ntok);
  else e2a_log("pairing: wrong code%s", rc == -2 ? " (blocked for a minute)" : "");
  return rc;
}

void pair_reset(void) {
  pthread_mutex_lock(&g_mx);
  g_ntok = 0;
  new_pin();
  save();
  pthread_mutex_unlock(&g_mx);
  e2a_log("pairing: every device forgotten, new code");
}

int pair_devices(void) { return g_ntok; }

/* LAN address of the console: connect a UDP socket (no packet is sent) and read its local name */
void pair_local_ip(char *out, size_t cap) {
  snprintf(out, cap, "<ps5-ip>");
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) return;
  struct sockaddr_in dst, me;
  memset(&dst, 0, sizeof dst);
  dst.sin_family = AF_INET;
  dst.sin_port = htons(53);
  dst.sin_addr.s_addr = htonl(0x08080808u);
  socklen_t len = sizeof me;
  if (connect(s, (struct sockaddr *)&dst, sizeof dst) == 0 && getsockname(s, (struct sockaddr *)&me, &len) == 0)
    snprintf(out, cap, "%s", inet_ntoa(me.sin_addr));
  close(s);
}
