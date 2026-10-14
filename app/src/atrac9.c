/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE. */
#include "atrac9.h"

#include "anim.h"
#include "log.h"

#include <math.h>   /* before the macros below: its own prototypes must not be renamed */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mem.h"   /* last: the allocations go through the app's own memory */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* LibAtrac9 compiled here as one unit (its sources are stored as .inc: every .c below app/src is compiled on its
 * own by the console build, not by the host build). The app runtime has no libm: its own sin/cos (1e-9) and
 * the one pow() call, pow(2, x), from exp. */
static double at9_pow(double base, double x) { return m_exp(x * (base == 2 ? 0.6931471805599453 : 1.0)); }
#define sin m_sin
#define cos m_cos
#define pow at9_pow
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#include "third_party/atrac9/libatrac9.h"
#include "third_party/atrac9/band_extension.inc"
#include "third_party/atrac9/bit_allocation.inc"
#include "third_party/atrac9/bit_reader.inc"
#include "third_party/atrac9/decinit.inc"
#include "third_party/atrac9/decoder.inc"
#include "third_party/atrac9/huffCodes.inc"
#include "third_party/atrac9/imdct.inc"
#include "third_party/atrac9/libatrac9.inc"
#include "third_party/atrac9/quantization.inc"
#include "third_party/atrac9/scale_factors.inc"
#include "third_party/atrac9/tables.inc"
#include "third_party/atrac9/unpack.inc"
#include "third_party/atrac9/utility.inc"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#undef sin
#undef cos
#undef pow

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* the ATRAC9 sub-format GUID of a WAVE_FORMAT_EXTENSIBLE "fmt " chunk */
static const uint8_t AT9_GUID[16] = {0xd2, 0x42, 0xe1, 0x47, 0xba, 0x36, 0x8d, 0x4d, 0x88, 0xfc, 0x61, 0x65, 0x4f, 0x8c, 0x83, 0x6c};

int at9_decode(const uint8_t *f, size_t len, at9_track *out) {
  memset(out, 0, sizeof *out);
  if (len < 12 || memcmp(f, "RIFF", 4) != 0 || memcmp(f + 8, "WAVE", 4) != 0) return -1;
  const uint8_t *fmt = NULL, *data = NULL, *fact = NULL, *smpl = NULL;
  size_t fmt_len = 0, data_len = 0, fact_len = 0, smpl_len = 0;
  for (size_t at = 12; at + 8 <= len;) {   /* the chunks: fmt, fact (delays), smpl (loop points), data */
    size_t clen = rd32(f + at + 4);
    const uint8_t *body = f + at + 8;
    if (clen > len - at - 8) clen = len - at - 8;
    if (memcmp(f + at, "fmt ", 4) == 0) { fmt = body; fmt_len = clen; }
    else if (memcmp(f + at, "data", 4) == 0) { data = body; data_len = clen; }
    else if (memcmp(f + at, "fact", 4) == 0) { fact = body; fact_len = clen; }
    else if (memcmp(f + at, "smpl", 4) == 0) { smpl = body; smpl_len = clen; }
    at += 8 + clen + (clen & 1);
  }
  if (!fmt || fmt_len < 52 || !data || data_len < 64) return -1;
  if (rd16(fmt) != 0xFFFE || memcmp(fmt + 24, AT9_GUID, 16) != 0) return -1;
  int channels = rd16(fmt + 2), rate = (int)rd32(fmt + 4), block = rd16(fmt + 12);
  if (channels < 1 || channels > 2 || rate < 8000 || rate > 192000 || block < 32) return -1;
  unsigned char config[ATRAC9_CONFIG_DATA_SIZE];
  memcpy(config, fmt + 44, ATRAC9_CONFIG_DATA_SIZE);
  int total = 0, delay = 0;
  if (fact && fact_len >= 12) { total = (int)rd32(fact); delay = (int)rd32(fact + 8); }   /* samples, encoder delay */
  if (fact && fact_len >= 8 && delay <= 0) delay = (int)rd32(fact + 4);
  if (delay < 0 || delay > 4096) delay = 0;

  void *h = Atrac9GetHandle();
  if (!h) return -1;
  Atrac9CodecInfo info;
  if (Atrac9InitDecoder(h, config) != 0 || Atrac9GetCodecInfo(h, &info) != 0 || info.channels != channels ||
      info.superframeSize <= 0 || info.framesInSuperframe <= 0 || info.frameSamples <= 0) {
    Atrac9ReleaseHandle(h);
    return -1;
  }
  size_t superframes = data_len / (size_t)info.superframeSize;
  size_t decoded_cap = superframes * (size_t)info.framesInSuperframe * (size_t)info.frameSamples;
  if (decoded_cap == 0 || decoded_cap > (size_t)(1 << 26)) { Atrac9ReleaseHandle(h); return -1; }   /* 64 M frames: not a snd0 */
  int16_t *pcm = malloc(decoded_cap * (size_t)channels * sizeof *pcm);
  int16_t *frame = malloc((size_t)info.frameSamples * (size_t)channels * sizeof *frame);
  if (!pcm || !frame) { free(pcm); free(frame); Atrac9ReleaseHandle(h); return -1; }
  size_t got = 0;   /* frames decoded so far */
  for (size_t s = 0; s < superframes; s++) {
    const unsigned char *p = data + s * (size_t)info.superframeSize;
    for (int k = 0; k < info.framesInSuperframe; k++) {
      int used = 0;
      if (Atrac9Decode(h, p, (short *)frame, &used) != 0 || used <= 0) { superframes = s; break; }   /* damaged: keep what we have */
      memcpy(pcm + got * (size_t)channels, frame, (size_t)info.frameSamples * (size_t)channels * sizeof *frame);
      got += (size_t)info.frameSamples;
      p += used;
    }
  }
  free(frame);
  Atrac9ReleaseHandle(h);
  if (got <= (size_t)delay) { free(pcm); return -1; }
  /* the encoder delay dropped from the front; the fact chunk's count bounds the end */
  size_t valid = got - (size_t)delay;
  if (total > 0 && (size_t)total < valid) valid = (size_t)total;
  if (delay) memmove(pcm, pcm + (size_t)delay * (size_t)channels, valid * (size_t)channels * sizeof *pcm);
  out->pcm = pcm;
  out->frames = (int)valid;
  out->channels = channels;
  out->rate = rate;
  /* loop points (absolute decoded samples, delay included) from the sampler chunk, when the file has one */
  if (smpl && smpl_len >= 36 + 24 && rd32(smpl + 28) >= 1) {
    long ls = (long)rd32(smpl + 36 + 8) - delay, le = (long)rd32(smpl + 36 + 12) - delay;
    if (ls < 0) ls = 0;
    if (le > (long)valid) le = (long)valid;
    if (le > ls + rate) { out->loop_start = (int)ls; out->loop_end = (int)le; }   /* a loop shorter than a second is noise */
  }
  return 0;
}
