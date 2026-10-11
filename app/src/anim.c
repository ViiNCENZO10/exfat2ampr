/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "anim.h"

uint64_t g_now;
float g_dt = 1.0f / 60.0f;
bool g_anim_enabled = true;
bool g_anim_busy, g_anim_ambient;

static uint64_t g_last_frame, g_last_input, g_frame;

#define AMBIENT_MS 20000u   /* pulses and sheens run this long after the last button press */

double m_sin(double x) {
  const double pi = M_TAU / 2;
  long long k = (long long)(x / M_TAU);
  x -= (double)k * M_TAU;
  if (x > pi) x -= M_TAU;
  else if (x < -pi) x += M_TAU;
  if (x > pi / 2) x = pi - x;
  else if (x < -pi / 2) x = -pi - x;
  double x2 = x * x;   /* Taylor series up to x^13 on [-pi/2, pi/2]: error < 1e-9 */
  return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110 * (1 - x2 / 156))))));
}

double m_cos(double x) { return m_sin(x + M_TAU / 4); }

double m_exp(double x) {
  if (x < -60) return 0;
  if (x > 60) x = 60;
  int halvings = 0;
  while (x > 0.5 || x < -0.5) { x /= 2; halvings++; }
  double term = 1, sum = 1;
  for (int i = 1; i < 14; i++) { term *= x / i; sum += term; }
  while (halvings--) sum *= sum;
  return sum;
}

void anim_frame(uint64_t now_ms) {
  float dt = g_last_frame ? (float)(now_ms - g_last_frame) / 1000.0f : 1.0f / 60.0f;
  g_dt = clampf(dt, 0.0f, 0.05f);   /* after a pause the motion restarts gently */
  g_last_frame = now_ms;
  g_now = now_ms;
  g_frame++;
  g_anim_busy = false;
  g_anim_ambient = false;
}

float smooth(float *v, float target, float speed, float eps) {
  float d = target - *v;
  if (!g_anim_enabled || (d < eps && d > -eps)) {
    *v = target;
    return target;
  }
  *v += d * (float)(1.0 - m_exp(-(double)(speed * g_dt)));
  g_anim_busy = true;
  return *v;
}

float anim_track(anim_val *a, float target, float speed, float eps) {
  if (!a->frame || g_frame > a->frame + 1) a->v = target;   /* same frame drawn twice: still continuous */
  a->frame = g_frame;
  return smooth(&a->v, target, speed, eps);
}

float anim_t(uint64_t start_ms, unsigned dur_ms) {
  if (!g_anim_enabled || !dur_ms) return 1.0f;
  if (g_now < start_ms) { g_anim_busy = true; return 0.0f; }
  float t = (float)(g_now - start_ms) / (float)dur_ms;
  if (t < 1.0f) g_anim_busy = true;
  return clampf(t, 0.0f, 1.0f);
}

void anim_input(uint64_t now_ms) { g_last_input = now_ms; }
bool anim_ambient_allowed(void) { return g_anim_enabled && (g_now < g_last_input || g_now - g_last_input < AMBIENT_MS); }

bool anim_ambient(void) {
  if (!anim_ambient_allowed()) return false;
  g_anim_ambient = true;
  return true;
}

float anim_pulse(unsigned period_ms) {
  if (!anim_ambient()) return 1.0f;
  return (float)(0.5 + 0.5 * m_sin((double)(g_now % period_ms) * M_TAU / period_ms));
}
