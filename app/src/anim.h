/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * Animation clock and helpers. Everything moves with the real frame time, so the motion keeps
 * the same speed whatever the frame rate. No libm: sine and exponential are computed here. */
#ifndef E2A_APP_ANIM_H
#define E2A_APP_ANIM_H

#include <stdbool.h>
#include <stdint.h>

#define M_TAU 6.28318530717958647692

double m_sin(double x);   /* radians */
double m_cos(double x);
double m_exp(double x);

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float ease_out(float t) {   /* cubic: fast start, soft landing */
  t = clampf(t, 0.0f, 1.0f);
  float u = 1.0f - t;
  return 1.0f - u * u * u;
}
static inline float ease_out_back(float t) {   /* lands with a small overshoot */
  t = clampf(t, 0.0f, 1.0f) - 1.0f;
  return 1.0f + t * t * (2.70158f * t + 1.70158f);
}

extern uint64_t g_now;        /* ms, start of the frame being drawn */
extern float g_dt;            /* seconds since the previous frame (clamped) */
extern bool g_anim_enabled;   /* user setting: off = everything snaps to its place */
extern bool g_anim_busy;      /* something moved this frame: draw the next one at once */
extern bool g_anim_ambient;   /* a slow ambient effect wants frames (pulses, sheen) */

void anim_frame(uint64_t now_ms);
/* exponential approach of *v to target; speed in 1/s, eps = "close enough" */
float smooth(float *v, float target, float speed, float eps);
/* the same for a value drawn every frame: it starts at its target when it was not on screen in
 * the previous frame (a list that just appeared does not glide in from an old position) */
typedef struct { float v; uint64_t frame; } anim_val;
float anim_track(anim_val *a, float target, float speed, float eps);
/* 0..1 progress of a timed effect started at start_ms (keeps frames coming until it ends) */
float anim_t(uint64_t start_ms, unsigned dur_ms);
/* 0..1 slow pulse for focus glows (ambient) */
float anim_pulse(unsigned period_ms);
/* true when an ambient effect may run now (and asks for its frames) */
bool anim_ambient(void);
/* ambient effects stop after a while without input, to leave the CPU to the conversion */
void anim_input(uint64_t now_ms);
bool anim_ambient_allowed(void);

#endif
