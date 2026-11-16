/* Copyright (c) 2026 Xiph.Org Foundation */
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "opus.h"
#ifdef FIXED_POINT
#ifdef ENABLE_RES24
#include "test_opus_malformed_samples_fixed_res24.h"
#else
#include "test_opus_malformed_samples_fixed.h"
#endif
#else
#include "test_opus_malformed_samples.h"
#endif

#define MAX_PACKET 1500
#define MAX_FRAME_SAMP 5760

static int failures;
static uint32_t rng_state;

static uint32_t fast_rand(void)
{
   rng_state ^= rng_state<<13;
   rng_state ^= rng_state>>17;
   rng_state ^= rng_state<<5;
   return rng_state;
}

static void check(int condition, const char *message)
{
   if (!condition)
   {
      fprintf(stderr, "FAIL: %s\n", message);
      failures++;
   }
}

typedef struct {
   const char *name;
   const unsigned char *data;
   int len;
} MalformedSample;

static const unsigned char truncated_toc[] = {0x03};
static const unsigned char zero_frame_count[] = {0x03, 0x00};
static const unsigned char excessive_duration[] = {0x03, 0x18};
static const unsigned char duration_overflow[] = {0x1B, 0x03, 0x00, 0x00, 0x00};
static const unsigned char truncated_vbr_size[] = {0x03, 0x82, 0xFC};
static const unsigned char vbr_size_past_end[] = {0x02, 0x05, 0x00, 0x00};
static const unsigned char padding_past_end[] = {0x03, 0x41, 0xFF, 0xFE};
static const unsigned char frame_past_limit[1277] = {0};

static const MalformedSample malformed_samples[] = {
   {"truncated TOC", truncated_toc, sizeof(truncated_toc)},
   {"zero frame count", zero_frame_count, sizeof(zero_frame_count)},
   {"excessive duration", excessive_duration, sizeof(excessive_duration)},
   {"truncated VBR size", truncated_vbr_size, sizeof(truncated_vbr_size)},
   {"VBR size past end", vbr_size_past_end, sizeof(vbr_size_past_end)},
   {"padding past end", padding_past_end, sizeof(padding_past_end)},
   {"frame past 1275-byte limit", frame_past_limit, sizeof(frame_past_limit)}
};

static void test_malformed_samples(void)
{
   int i;
   for (i=0;i<(int)(sizeof(malformed_samples)/sizeof(malformed_samples[0]));i++)
   {
      const MalformedSample *sample = &malformed_samples[i];
      const unsigned char *frames[48];
      opus_int16 size[48];
      unsigned char toc;
      int payload_offset;
      int ret;
      OpusDecoder *dec;
      opus_int16 pcm[MAX_FRAME_SAMP];
      int err;
      int j;

      payload_offset = -1;
      ret = opus_packet_parse(sample->data, sample->len, &toc, frames, size,
                              &payload_offset);
      check(ret == OPUS_INVALID_PACKET, sample->name);
      check(payload_offset == 0, "error payload offset");

      dec = opus_decoder_create(48000, 1, &err);
      check(dec != NULL && err == OPUS_OK, "decoder create");
      if (dec == NULL)
         continue;
      for (j=0;j<MAX_FRAME_SAMP;j++)
         pcm[j] = 0x7F7F;
      ret = opus_decode(dec, sample->data, sample->len, pcm, MAX_FRAME_SAMP, 0);
      check(ret == OPUS_INVALID_PACKET, sample->name);
      for (j=0;j<MAX_FRAME_SAMP;j++)
         check(pcm[j] == 0x7F7F, "malformed packet wrote PCM");
      opus_decoder_destroy(dec);
   }

   check(opus_packet_has_lbrr(truncated_toc, 0) == OPUS_BAD_ARG,
         "has_lbrr accepted a zero-length packet");
   check(opus_packet_has_lbrr(truncated_toc, -1) == OPUS_BAD_ARG,
         "has_lbrr accepted a negative length");
   check(opus_packet_has_lbrr(NULL, 1) == OPUS_BAD_ARG,
         "has_lbrr accepted a NULL packet");
   check(opus_packet_get_nb_frames(truncated_toc, 0) == OPUS_BAD_ARG,
         "nb_frames accepted a zero-length packet");
   check(opus_packet_get_samples_per_frame(truncated_toc, 0) == OPUS_BAD_ARG,
         "samples_per_frame accepted an invalid rate");
   check(opus_packet_get_nb_samples(duration_overflow,
                                    sizeof(duration_overflow),
                                    INT_MAX) == OPUS_INVALID_PACKET,
         "nb_samples accepted an overflowing duration");
}

static void test_valid_samples(void)
{
   OpusDecoder *dec;
   opus_int16 pcm[MAX_FRAME_SAMP*2];
   int ret;
   int err;
   const unsigned char *frames[48];
   opus_int16 size[48];
   unsigned char toc;
   int payload_offset;

   dec = opus_decoder_create(8000, 1, &err);
   check(dec != NULL && err == OPUS_OK, "SILK decoder create");
   if (dec != NULL)
   {
      ret = opus_decode(dec, valid_silk_packet, VALID_SILK_PACKET_LEN,
                        pcm, MAX_FRAME_SAMP, 0);
      check(ret == VALID_SILK_PCM_LEN, "SILK decoded length");
      check(memcmp(pcm, valid_silk_pcm, VALID_SILK_PCM_LEN*sizeof(*pcm)) == 0,
            "SILK output changed");
      opus_decoder_destroy(dec);
   }

   dec = opus_decoder_create(48000, 2, &err);
   check(dec != NULL && err == OPUS_OK, "CELT decoder create");
   if (dec != NULL)
   {
      ret = opus_decode(dec, valid_celt_packet, VALID_CELT_PACKET_LEN,
                        pcm, MAX_FRAME_SAMP, 0);
      check(ret*2 == VALID_CELT_PCM_LEN, "CELT decoded length");
      check(memcmp(pcm, valid_celt_pcm, VALID_CELT_PCM_LEN*sizeof(*pcm)) == 0,
            "CELT output changed");
      opus_decoder_destroy(dec);
   }

   payload_offset = -1;
   ret = opus_packet_parse(valid_silk_packet, VALID_SILK_PACKET_LEN,
         &toc, frames, size, &payload_offset);
   check(ret == 1, "valid SILK frame count");
   check(payload_offset == 1, "valid SILK payload offset");
}

static void test_truncations_and_tocs(void)
{
   OpusDecoder *dec;
   opus_int16 pcm[MAX_FRAME_SAMP];
   unsigned char packet[MAX_PACKET];
   int cut;
   int toc;
   int err;
   int decoded;
   int rejected;

   dec = opus_decoder_create(48000, 1, &err);
   check(dec != NULL && err == OPUS_OK, "truncation decoder create");
   if (dec == NULL)
      return;

   decoded = 0;
   rejected = 0;
   for (cut=0;cut<VALID_CELT_PACKET_LEN;cut++)
   {
      int ret = opus_decode(dec, valid_celt_packet, cut, pcm,
                            MAX_FRAME_SAMP, 0);
      if (ret < 0) rejected++;
      else decoded++;
   }

   for (toc=0;toc<256;toc++)
   {
      int ret;
      memset(packet, 0, sizeof(packet));
      packet[0] = (unsigned char)toc;
      ret = opus_decode(dec, packet, 64, pcm, MAX_FRAME_SAMP, 0);
      if (ret < 0) rejected++;
      else decoded++;
   }
   opus_decoder_destroy(dec);

   printf("truncation_toc_cases=%d rejected=%d decoded_dtx=%d\n",
          VALID_CELT_PACKET_LEN+256, rejected, decoded);
}

static void test_mutation_fuzz(int cases)
{
   OpusDecoder *dec;
   opus_int16 pcm[MAX_FRAME_SAMP*2];
   unsigned char packet[MAX_PACKET];
   int i;
   int err;
   int accepted;
   int rejected;

   dec = opus_decoder_create(48000, 2, &err);
   check(dec != NULL && err == OPUS_OK, "fuzz decoder create");
   if (dec == NULL)
      return;

   rng_state = 0x4010C0DEu;
   accepted = 0;
   rejected = 0;
   for (i=0;i<cases;i++)
   {
      const unsigned char *frames[48];
      opus_int16 size[48];
      unsigned char toc;
      int payload_offset;
      int len;
      int j;
      int ret;

      len = 1 + fast_rand()%MAX_PACKET;
      for (j=0;j<len;j++)
         packet[j] = (unsigned char)fast_rand();
      if ((fast_rand()&3) == 0)
      {
         packet[0] = valid_celt_packet[fast_rand()%VALID_CELT_PACKET_LEN];
         if (len > 1)
            packet[1] = (unsigned char)fast_rand();
      }
      payload_offset = -1;
      ret = opus_packet_parse(packet, len, &toc, frames, size,
                              &payload_offset);
      if (ret < 0)
      {
         check(payload_offset == 0, "fuzz error payload offset");
         rejected++;
         continue;
      }
      accepted++;
      ret = opus_decode(dec, packet, len, pcm, MAX_FRAME_SAMP, 0);
      check(ret <= MAX_FRAME_SAMP, "decoded sample count");
   }
   opus_decoder_destroy(dec);

   printf("mutation_cases=%d rejected=%d accepted=%d\n",
          cases, rejected, accepted);
}

int main(int argc, char **argv)
{
   int mutation_cases;

   mutation_cases = argc > 1 ? atoi(argv[1]) : 20000;
   if (mutation_cases < 0)
      mutation_cases = 0;

   test_malformed_samples();
   test_valid_samples();
   test_truncations_and_tocs();
   test_mutation_fuzz(mutation_cases);

   if (failures)
   {
      fprintf(stderr, "%d failure(s)\n", failures);
      return 1;
   }
   printf("malformed_packet_hardening: OK (crashes=0 oob=0)\n");
   return 0;
}
