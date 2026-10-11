/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "sound.h"

#include "anim.h"
#include "platform.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define SR 48000
#define VOICES 8
#define MASTER 0.9

typedef struct { int16_t *pcm; int len; } cue_pcm;
typedef struct { const int16_t *pcm; int len, pos; snd_cue cue; } voice_t;

static cue_pcm g_pcm[SND_COUNT];
static voice_t g_voice[VOICES];
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static bool g_on = true, g_ready;
static snd_cue g_pending;

/* one bell-like note: sine with a touch of 2nd/3rd harmonic, short attack, exponential decay
 * (tau seconds); the frequency can glide from f0 to f1 */
static void note(float *b, int n, double at, double f0, double f1, double dur, double attack, double tau, double gain, double h2,
                 double h3) {
  int s0 = (int)(at * SR), len = (int)(dur * SR), fade = SR * 3 / 1000;
  double ph = 0, env = 1, k = m_exp(-1.0 / (tau * SR));
  for (int i = 0; i < len && s0 + i < n; i++) {
    double f = f0 + (f1 - f0) * (len > 1 ? (double)i / (len - 1) : 0);
    ph += M_TAU * f / SR;
    if (ph > M_TAU) ph -= M_TAU;
    double t = (double)i / SR, a = t < attack ? t / attack : 1, tail = len - i < fade ? (double)(len - i) / fade : 1;
    double v = m_sin(ph) + h2 * m_sin(2 * ph) + h3 * m_sin(3 * ph);
    b[s0 + i] += (float)(v * a * env * gain * tail);
    env *= k;
  }
}

/* filtered noise whose brightness rises: a short "whoosh" */
static void whoosh(float *b, int n, double at, double dur, double c0, double c1, double gain) {
  int s0 = (int)(at * SR), len = (int)(dur * SR);
  uint32_t rng = 0x9e3779b9u;
  double lp = 0, lp2 = 0, low = 0, low_a = 1 - m_exp(-M_TAU * 300.0 / SR);
  for (int i = 0; i < len && s0 + i < n; i++) {
    double t = (double)i / len, a = 1 - m_exp(-M_TAU * (c0 + (c1 - c0) * t) / SR);
    rng = rng * 1664525u + 1013904223u;
    double w = (double)(rng >> 8) / 8388608.0 - 1.0;
    lp += a * (w - lp);
    lp2 += a * (lp - lp2);
    low += low_a * (lp2 - low);
    b[s0 + i] += (float)((lp2 - low) * m_sin(M_TAU / 2 * t) * gain);
  }
}

/* small room: four damped combs and two all-passes (Schroeder / Freeverb tunings at 48 kHz) */
static void reverb(float *b, int n, double wet) {
  static const int comb[4] = {1694, 1759, 1622, 1547}, ap[2] = {605, 480};
  float *out = calloc((size_t)n, sizeof *out), *buf = calloc(2048, sizeof *buf);
  if (!out || !buf) { free(out); free(buf); return; }
  for (int c = 0; c < 4; c++) {
    memset(buf, 0, 2048 * sizeof *buf);
    float filt = 0;
    for (int i = 0, idx = 0; i < n; i++, idx = idx + 1 == comb[c] ? 0 : idx + 1) {
      float y = buf[idx];
      filt = y * 0.72f + filt * 0.28f;
      buf[idx] = b[i] + filt * 0.80f;
      out[i] += y * 0.25f;
    }
  }
  for (int a = 0; a < 2; a++) {
    memset(buf, 0, 2048 * sizeof *buf);
    for (int i = 0, idx = 0; i < n; i++, idx = idx + 1 == ap[a] ? 0 : idx + 1) {
      float v = buf[idx], x = out[i];
      buf[idx] = x + v * 0.5f;
      out[i] = v - x;
    }
  }
  for (int i = 0; i < n; i++) b[i] = b[i] * (float)(1 - wet * 0.3) + out[i] * (float)wet;
  free(out);
  free(buf);
}

static void build(snd_cue c) {
  double seconds = c == SND_DONE ? 2.2 : c == SND_START || c == SND_NOTIFY ? 1.2 : c == SND_SELECT || c == SND_BACK ? 0.7 : 0.35;
  int n = (int)(seconds * SR);
  float *b = calloc((size_t)n, sizeof *b);
  if (!b) return;
  static const double up[4] = {1046.50, 1318.51, 1567.98, 2093.00}, chord[4] = {523.25, 659.25, 783.99, 1046.50};
  switch (c) {
    case SND_MOVE:
      note(b, n, 0, 1760, 1760, 0.045, 0.0015, 0.010, 0.20, 0.10, 0);
      note(b, n, 0, 3520, 3520, 0.020, 0.0010, 0.004, 0.04, 0, 0);
      break;
    case SND_TOGGLE_ON:
    case SND_TOGGLE_OFF: {
      double lo = 1567.98, hi = 2093.00, first = c == SND_TOGGLE_ON ? lo : hi, second = c == SND_TOGGLE_ON ? hi : lo;
      note(b, n, 0, first, first, 0.035, 0.001, 0.008, 0.15, 0.08, 0);
      note(b, n, 0.032, second, second, 0.045, 0.001, 0.010, 0.15, 0.08, 0);
      break;
    }
    case SND_TAB:
      whoosh(b, n, 0, 0.10, 600, 4200, 0.30);
      note(b, n, 0, 783.99, 1046.50, 0.07, 0.003, 0.025, 0.07, 0.10, 0);
      break;
    case SND_BACK:
      note(b, n, 0, 987.77, 987.77, 0.14, 0.002, 0.045, 0.17, 0.12, 0.03);
      note(b, n, 0.050, 739.99, 739.99, 0.22, 0.002, 0.060, 0.16, 0.12, 0.03);
      reverb(b, n, 0.15);
      break;
    case SND_SELECT:
      note(b, n, 0, 1318.51, 1318.51, 0.16, 0.002, 0.050, 0.20, 0.15, 0.04);
      note(b, n, 0.055, 1975.53, 1975.53, 0.26, 0.002, 0.075, 0.19, 0.12, 0.03);
      reverb(b, n, 0.22);
      break;
    case SND_NOTIFY:
      note(b, n, 0, 1567.98, 1567.98, 0.35, 0.003, 0.090, 0.15, 0.10, 0.02);
      note(b, n, 0.080, 2349.32, 2349.32, 0.40, 0.003, 0.110, 0.12, 0.08, 0.02);
      reverb(b, n, 0.20);
      break;
    case SND_START:
      for (int k = 0; k < 4; k++) note(b, n, 0.07 * k, up[k], up[k], 0.35, 0.002, 0.10, 0.13, 0.12, 0.03);
      reverb(b, n, 0.25);
      break;
    case SND_DONE:
      for (int k = 0; k < 4; k++) note(b, n, 0.09 * k, chord[k], chord[k], 1.3, 0.004, 0.40, 0.12, 0.18, 0.05);
      note(b, n, 0.36, 2093.00, 2093.00, 0.9, 0.004, 0.30, 0.045, 0, 0);
      reverb(b, n, 0.30);
      break;
    case SND_ERROR:
      note(b, n, 0, 233.08, 233.08, 0.13, 0.004, 0.060, 0.28, 0.45, 0.25);
      note(b, n, 0.135, 207.65, 207.65, 0.16, 0.004, 0.070, 0.26, 0.45, 0.25);
      break;
    default: break;
  }
  /* trim the silent tail, convert */
  size_t len = (size_t)n;
  while (len > 0 && b[len - 1] < 0.0004f && b[len - 1] > -0.0004f) len--;
  int16_t *pcm = len ? malloc(len * sizeof *pcm) : NULL;
  if (pcm) {
    for (size_t i = 0; i < len; i++) {
      double v = b[i] * MASTER * 32767.0;
      pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    g_pcm[c].pcm = pcm;
    g_pcm[c].len = (int)len;
  }
  free(b);
}

void snd_mix(int16_t *lr, int frames) {
  pthread_mutex_lock(&g_mx);
  for (int i = 0; i < frames; i++) {
    int32_t s = 0;
    for (int v = 0; v < VOICES; v++)
      if (g_voice[v].pos < g_voice[v].len) s += g_voice[v].pcm[g_voice[v].pos++];
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    lr[2 * i] = lr[2 * i + 1] = (int16_t)s;
  }
  pthread_mutex_unlock(&g_mx);
}

static void play(snd_cue c) {
  if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE) || !g_on || c <= SND_NONE || c >= SND_COUNT || !g_pcm[c].pcm) return;
  pthread_mutex_lock(&g_mx);
  int slot = -1;
  for (int v = 0; v < VOICES && slot < 0; v++)   /* fast scrolling: one tick restarts instead of piling up */
    if (c == SND_MOVE && g_voice[v].cue == SND_MOVE && g_voice[v].pos < g_voice[v].len) slot = v;
  for (int v = 0; v < VOICES && slot < 0; v++)
    if (g_voice[v].pos >= g_voice[v].len) slot = v;
  if (slot < 0) {   /* all busy: take the one closest to its end */
    slot = 0;
    for (int v = 1; v < VOICES; v++)
      if (g_voice[v].len - g_voice[v].pos < g_voice[slot].len - g_voice[slot].pos) slot = v;
  }
  g_voice[slot].pcm = g_pcm[c].pcm;
  g_voice[slot].len = g_pcm[c].len;
  g_voice[slot].pos = 0;
  g_voice[slot].cue = c;
  pthread_mutex_unlock(&g_mx);
}

/* synthesis takes a few tens of ms: done beside the start screen, not before it */
static void *init_thread(void *arg) {
  (void)arg;
  for (int c = SND_NONE + 1; c < SND_COUNT; c++) build((snd_cue)c);
  if (plat_audio_start(snd_mix) == 0) __atomic_store_n(&g_ready, true, __ATOMIC_RELEASE);
  return NULL;
}

void snd_init(void) {
  pthread_t th;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&th, &at, init_thread, NULL) != 0) init_thread(NULL);
  pthread_attr_destroy(&at);
}

void snd_set_enabled(bool on) { g_on = on; }
bool snd_enabled(void) { return g_on; }

void snd_cue_frame(snd_cue cue) {
  if (cue > g_pending) g_pending = cue;
}

void snd_flush(void) {
  if (g_pending != SND_NONE) play(g_pending);
  g_pending = SND_NONE;
}
