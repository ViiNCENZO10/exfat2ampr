/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
 * Navigation sounds, synthesized at start (no sound files): soft ticks and chimes, mixed on the
 * platform's audio thread (48 kHz stereo). */
#ifndef E2A_APP_SOUND_H
#define E2A_APP_SOUND_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  SND_NONE, SND_MOVE, SND_TOGGLE_ON, SND_TOGGLE_OFF, SND_TAB, SND_BACK, SND_SELECT, SND_NOTIFY, SND_START, SND_DONE,
  SND_ERROR, SND_COUNT   /* in priority order: one cue per frame, the most important wins */
} snd_cue;

void snd_init(void);
void snd_set_enabled(bool on);
bool snd_enabled(void);
/* asks for a cue during this frame; snd_flush() plays the most important one */
void snd_cue_frame(snd_cue cue);
void snd_flush(void);
/* fills frames x 2 (left, right) samples; called by the platform's audio thread */
void snd_mix(int16_t *lr, int frames);

#endif
