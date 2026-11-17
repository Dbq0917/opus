/* Copyright (c) 2024 Xiph.Org Foundation */
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
   OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/* Acceptance test for the in-band FEC/PLC recovery "pop".

   A burst of consecutive packet losses is injected into a music-like signal
   and the decoder is driven through the standard FEC/PLC recovery sequence.
   The largest sample-to-sample discontinuity in the recovery region is then
   compared against the natural discontinuity level of the clean signal. The
   test fails if recovery introduces a large impulsive step (the audible pop),
   and it is exercised for several burst lengths and FEC strengths. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "opus.h"

#define FS 48000
#define FRAME 960
#define TOTAL 120
#define LOSS_START 50
#define MAX_PACKET 1500

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* A deterministic, tonal, harmonically rich signal that behaves like music and
   is sensitive to recovery discontinuities. */
static void gen_music(float *x, int n)
{
   int i;
   for (i = 0; i < n; i++) {
      double t = (double)i/FS;
      double vib = 1.0 + 0.004*sin(2*M_PI*5.5*t);
      double v = 0.55*sin(2*M_PI*440*vib*t)
               + 0.35*sin(2*M_PI*660*vib*t)
               + 0.25*sin(2*M_PI*880*vib*t)
               + 0.18*sin(2*M_PI*1760*vib*t)
               + 0.10*sin(2*M_PI*3520*vib*t);
      x[i] = (float)(0.05*v);
   }
}

#ifdef DISABLE_FLOAT_API
/* Build an int16 source once and reuse it for every sub-case. */
static void to_int16(const float *in, opus_int16 *out, int n)
{
   int i;
   for (i = 0; i < n; i++) {
      double v = in[i]*16000.0;
      if (v > 32767) v = 32767;
      if (v < -32768) v = -32768;
      out[i] = (opus_int16)lrint(v);
   }
}
#endif

/* Returns the largest |x[i]-x[i-1]| in [lo,hi), normalized to full scale. */
static double max_step(const float *x, int lo, int hi)
{
   double worst = 0;
   int i;
   for (i = lo+1; i < hi; i++) {
      double s = fabs((double)x[i]-x[i-1]);
      if (s > worst) worst = s;
   }
   return worst;
}

static int run_case(int loss_len, int loss_perc)
{
   int err, i, out = 0;
   OpusEncoder *enc;
   OpusDecoder *dec;
   unsigned char packets[TOTAL*MAX_PACKET];
   int lens[TOTAL];
   float *input = (float*)malloc(sizeof(float)*FRAME*TOTAL);
#ifdef DISABLE_FLOAT_API
   opus_int16 *in16 = (opus_int16*)malloc(sizeof(opus_int16)*FRAME*TOTAL);
   opus_int16 *o16 = (opus_int16*)calloc(FRAME*(TOTAL+1), sizeof(opus_int16));
#else
   /* Each loss adds one extra output frame at the recovery packet. */
   float *output = (float*)calloc(FRAME*(TOTAL+1), sizeof(float));
#endif
   double clean_level, recovery_step, ratio;
   int ret = 0;

   enc = opus_encoder_create(FS, 1, OPUS_APPLICATION_AUDIO, &err);
   if (err != OPUS_OK) { fprintf(stderr, "enc create failed\n"); return 1; }
   opus_encoder_ctl(enc, OPUS_SET_BITRATE(64000));
   opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));
   opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(loss_perc));
   opus_encoder_ctl(enc, OPUS_SET_DTX(0));
   opus_encoder_ctl(enc, OPUS_SET_FORCE_CHANNELS(1));
   dec = opus_decoder_create(FS, 1, &err);
   if (err != OPUS_OK) { fprintf(stderr, "dec create failed\n"); return 1; }

   gen_music(input, FRAME*TOTAL);
#ifdef DISABLE_FLOAT_API
   to_int16(input, in16, FRAME*TOTAL);
   for (i = 0; i < TOTAL; i++)
      lens[i] = opus_encode(enc, &in16[i*FRAME], FRAME,
                            &packets[(size_t)i*MAX_PACKET], MAX_PACKET);
#else
   for (i = 0; i < TOTAL; i++)
      lens[i] = opus_encode_float(enc, &input[i*FRAME], FRAME,
                                  &packets[(size_t)i*MAX_PACKET], MAX_PACKET);
#endif
   if (lens[0] < 0) { fprintf(stderr, "encode failed\n"); ret = 1; goto done; }

   for (i = 0; i < TOTAL; i++) {
      int lost = (i >= LOSS_START && i < LOSS_START+loss_len);
      int r;
      if (lost) {
#ifdef DISABLE_FLOAT_API
         r = opus_decode(dec, NULL, 0, &o16[out*FRAME], FRAME, 1);
#else
         r = opus_decode_float(dec, NULL, 0, &output[out*FRAME], FRAME, 1);
#endif
         if (r < 0) { fprintf(stderr, "plc decode failed\n"); ret = 1; goto done; }
         out++;
      } else {
         if (i > 0 && i-1 >= LOSS_START && i-1 < LOSS_START+loss_len) {
#ifdef DISABLE_FLOAT_API
            r = opus_decode(dec, &packets[(size_t)i*MAX_PACKET], lens[i],
                            &o16[out*FRAME], FRAME, 1);
#else
            r = opus_decode_float(dec, &packets[(size_t)i*MAX_PACKET], lens[i],
                                  &output[out*FRAME], FRAME, 1);
#endif
            if (r < 0) { fprintf(stderr, "fec decode failed\n"); ret = 1; goto done; }
            out++;
         }
#ifdef DISABLE_FLOAT_API
         r = opus_decode(dec, &packets[(size_t)i*MAX_PACKET], lens[i],
                         &o16[out*FRAME], FRAME, 0);
#else
         r = opus_decode_float(dec, &packets[(size_t)i*MAX_PACKET], lens[i],
                               &output[out*FRAME], FRAME, 0);
#endif
         if (r < 0) { fprintf(stderr, "normal decode failed\n"); ret = 1; goto done; }
         out++;
      }
   }

#ifdef DISABLE_FLOAT_API
   {
      double clean = 0, rec = 0;
      int j;
      for (j = (5*FRAME)+1; j < (LOSS_START-5)*FRAME; j++) {
         double s = fabs((double)o16[j]-o16[j-1]);
         if (s > clean) clean = s;
      }
      for (j = (LOSS_START+loss_len-1)*FRAME;
           j < (LOSS_START+loss_len+4)*FRAME && j < out*FRAME; j++) {
         double s = fabs((double)o16[j]-o16[j-1]);
         if (s > rec) rec = s;
      }
      clean_level = clean/32768.0;
      recovery_step = rec/32768.0;
   }
#else
   clean_level = max_step(output, 5*FRAME, (LOSS_START-5)*FRAME);
   recovery_step = max_step(output, (LOSS_START+loss_len-1)*FRAME,
                            (LOSS_START+loss_len+4)*FRAME);
#endif

   ratio = recovery_step/(clean_level > 1e-9 ? clean_level : 1e-9);
   printf("FEC recovery: loss=%d perc=%d recovery_step=%.6f clean=%.6f ratio=%.2f\n",
          loss_len, loss_perc, recovery_step, clean_level, ratio);
   /* The recovery step must stay within ~4x the natural clean-frame step. */
   if (ratio > 4.0) {
      fprintf(stderr, "FAIL: recovery discontinuity %.2fx exceeds limit\n", ratio);
      ret = 1;
   }

done:
#ifdef DISABLE_FLOAT_API
   free(in16);
   free(o16);
#else
   free(output);
#endif
   free(input);
   opus_encoder_destroy(enc);
   opus_decoder_destroy(dec);
   return ret;
}

int main(void)
{
   static const int loss_lengths[] = {1, 2, 3, 5, 8};
   static const int strengths[] = {15, 30, 50};
   int l, s;
   for (l = 0; l < (int)(sizeof(loss_lengths)/sizeof(*loss_lengths)); l++)
      for (s = 0; s < (int)(sizeof(strengths)/sizeof(*strengths)); s++)
         if (run_case(loss_lengths[l], strengths[s]) != 0)
            return 1;
   printf("All FEC recovery pop tests passed.\n");
   return 0;
}
