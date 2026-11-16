#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "opus.h"
#include "test_opus_common.h"

#define TEST_FS 48000
#define TEST_FRAME 960
#define TEST_FRAMES 600
#define TEST_MAX_PACKET 1500

static void generate_voice(opus_int16 *pcm, int samples)
{
   int i;
   for (i = 0; i < samples; i++)
   {
      double t = (double)i / TEST_FS;
      double env = 0.35 + 0.25*sin(2*M_PI*1.7*t);
      double v = env*(0.62*sin(2*M_PI*137*t)
                    + 0.22*sin(2*M_PI*311*t)
                    + 0.16*sin(2*M_PI*683*t));
      pcm[i] = (opus_int16)lrint(12000*v);
   }
}

static double segment_snr(const opus_int16 *ref, const opus_int16 *test, int n)
{
   double signal = 0;
   double noise = 0;
   int i;
   for (i = 0; i < n; i++)
   {
      double x = ref[i];
      double e = x - test[i];
      signal += x*x;
      noise += e*e;
   }
   if (noise < 1e-9)
      return 99;
   return 10*log10(signal/noise);
}

static OpusEncoder *create_encoder(int bitrate, int loss, int redundancy)
{
   int err;
   OpusEncoder *enc = opus_encoder_create(TEST_FS, 1, OPUS_APPLICATION_VOIP, &err);
   if (enc == NULL || err != OPUS_OK)
      test_failed();
   if (opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate)) != OPUS_OK) test_failed();
   if (opus_encoder_ctl(enc, OPUS_SET_VBR(1)) != OPUS_OK) test_failed();
   if (opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(10)) != OPUS_OK) test_failed();
   if (opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(loss)) != OPUS_OK) test_failed();
   if (opus_encoder_ctl(enc, OPUS_SET_INBAND_REDUNDANCY(redundancy)) != OPUS_OK) test_failed();
   return enc;
}

static OpusDecoder *create_decoder(void)
{
   int err;
   OpusDecoder *dec = opus_decoder_create(TEST_FS, 1, &err);
   if (dec == NULL || err != OPUS_OK)
      test_failed();
   return dec;
}

static void test_api_edges(void)
{
   int err;
   opus_int32 value;
   opus_int32 *null_ptr = (opus_int32 *)NULL;
   OpusEncoder *enc = opus_encoder_create(TEST_FS, 1, OPUS_APPLICATION_VOIP, &err);
   if (enc == NULL || err != OPUS_OK)
      test_failed();

   if (opus_encoder_ctl(enc, OPUS_SET_INBAND_REDUNDANCY(-1)) != OPUS_BAD_ARG)
      test_failed();
   if (opus_encoder_ctl(enc, OPUS_SET_INBAND_REDUNDANCY(31)) != OPUS_BAD_ARG)
      test_failed();
   if (opus_encoder_ctl(enc, OPUS_SET_INBAND_REDUNDANCY(30)) != OPUS_OK)
      test_failed();
   if (opus_encoder_ctl(enc, OPUS_GET_INBAND_REDUNDANCY(&value)) != OPUS_OK
         || value != 30)
      test_failed();
   if (opus_encoder_ctl(enc, OPUS_GET_INBAND_REDUNDANCY(null_ptr)) != OPUS_BAD_ARG)
      test_failed();
   if (opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(0)) != OPUS_OK)
      test_failed();
   if (opus_encoder_ctl(enc, OPUS_GET_INBAND_REDUNDANCY(&value)) != OPUS_OK
         || value != 0)
      test_failed();

   opus_encoder_destroy(enc);
}

static void test_recovery_and_bandwidth(int bitrate)
{
   int i;
   int nb_lbrr = 0;
   int nb_losses = 0;
   int nb_metrics = 0;
   int bytes = 0;
   int base_bytes = 0;
   double fec_snr = 0;
   double plc_snr = 0;
   OpusEncoder *enc;
   OpusEncoder *base_enc;
   OpusDecoder *ref_dec;
   OpusDecoder *fec_dec;
   OpusDecoder *plc_dec;
   opus_int16 *pcm;
   opus_int16 *ref;
   opus_int16 *fec;
   opus_int16 *plc;
   opus_int16 out[TEST_FRAME];
   unsigned char (*packet)[TEST_MAX_PACKET];
   unsigned char base_packet[TEST_MAX_PACKET];
   int len[TEST_FRAMES];
   int has_lbrr[TEST_FRAMES];
   int lost[TEST_FRAMES];

   pcm = (opus_int16 *)malloc(sizeof(*pcm)*TEST_FRAME*TEST_FRAMES);
   ref = (opus_int16 *)malloc(sizeof(*ref)*TEST_FRAME*TEST_FRAMES);
   fec = (opus_int16 *)malloc(sizeof(*fec)*TEST_FRAME*TEST_FRAMES);
   plc = (opus_int16 *)malloc(sizeof(*plc)*TEST_FRAME*TEST_FRAMES);
   packet = (unsigned char (*)[TEST_MAX_PACKET])malloc(
         sizeof(*packet)*TEST_FRAMES);
   if (pcm == NULL || ref == NULL || fec == NULL || plc == NULL
         || packet == NULL)
      test_failed();
   generate_voice(pcm, TEST_FRAME*TEST_FRAMES);

   enc = create_encoder(bitrate, 10, 30);
   for (i = 0; i < TEST_FRAMES; i++)
   {
      len[i] = opus_encode(enc, pcm+i*TEST_FRAME, TEST_FRAME,
            packet[i], TEST_MAX_PACKET);
      if (len[i] <= 0)
         test_failed();
      bytes += len[i];
      has_lbrr[i] = opus_packet_has_lbrr(packet[i], len[i]);
      if (has_lbrr[i] < 0)
         test_failed();
      nb_lbrr += has_lbrr[i] != 0;
   }
   if (nb_lbrr == 0)
      test_failed();
   opus_encoder_destroy(enc);

   base_enc = create_encoder(bitrate, 10, 0);
   for (i = 0; i < TEST_FRAMES; i++)
   {
      int ret = opus_encode(base_enc, pcm+i*TEST_FRAME, TEST_FRAME,
            base_packet, TEST_MAX_PACKET);
      if (ret <= 0)
         test_failed();
      base_bytes += ret;
   }
   opus_encoder_destroy(base_enc);
   if (bytes*100 > base_bytes*130)
      test_failed();

   ref_dec = create_decoder();
   fec_dec = create_decoder();
   plc_dec = create_decoder();

   for (i = 0; i < TEST_FRAMES; i++)
   {
      int ret = opus_decode(ref_dec, packet[i], len[i], ref+i*TEST_FRAME,
            TEST_FRAME, 0);
      if (ret != TEST_FRAME)
         test_failed();
   }

   memset(lost, 0, sizeof(lost));
   for (i = 1; i < TEST_FRAMES-1; i++)
   {
      if (i%10 == 4 && has_lbrr[i+1] && !lost[i-1])
      {
         lost[i] = 1;
         nb_losses++;
      }
   }
   if (nb_losses < 8)
      test_failed();

   for (i = 0; i < TEST_FRAMES; i++)
   {
      int ret;
      if (lost[i])
         continue;
      if (i > 0 && lost[i-1])
      {
         ret = opus_decode(fec_dec, packet[i], len[i], out, TEST_FRAME, 1);
         if (ret != TEST_FRAME)
            test_failed();
         memcpy(fec+(i-1)*TEST_FRAME, out, sizeof(*out)*TEST_FRAME);
      }
      ret = opus_decode(fec_dec, packet[i], len[i], out, TEST_FRAME, 0);
      if (ret != TEST_FRAME)
         test_failed();
      memcpy(fec+i*TEST_FRAME, out, sizeof(*out)*TEST_FRAME);
   }

   for (i = 0; i < TEST_FRAMES; i++)
   {
      int ret = lost[i]
            ? opus_decode(plc_dec, NULL, 0, out, TEST_FRAME, 0)
            : opus_decode(plc_dec, packet[i], len[i], out, TEST_FRAME, 0);
      if (ret != TEST_FRAME)
         test_failed();
      memcpy(plc+i*TEST_FRAME, out, sizeof(*out)*TEST_FRAME);
   }

   for (i = 1; i < TEST_FRAMES-1; i++)
   {
      if (lost[i])
      {
         fec_snr += segment_snr(ref+i*TEST_FRAME, fec+i*TEST_FRAME, TEST_FRAME);
         plc_snr += segment_snr(ref+i*TEST_FRAME, plc+i*TEST_FRAME, TEST_FRAME);
         nb_metrics++;
      }
   }
   fec_snr /= nb_metrics;
   plc_snr /= nb_metrics;
   if (!(fec_snr > plc_snr + 3))
      test_failed();

   printf("In-band redundancy %d bps: %d frames, %d with backup, "
          "%d single losses, bytes %d/%d (%.3fx), "
          "recovered SNR %.2f dB vs PLC %.2f dB\n",
          bitrate, TEST_FRAMES, nb_lbrr, nb_losses, bytes, base_bytes,
          (double)bytes/base_bytes, fec_snr, plc_snr);

   opus_decoder_destroy(ref_dec);
   opus_decoder_destroy(fec_dec);
   opus_decoder_destroy(plc_dec);
   free(pcm);
   free(ref);
   free(fec);
   free(plc);
   free(packet);
}

static void test_no_loss_and_burst(void)
{
   int i;
   int ret;
   OpusEncoder *enc;
   OpusDecoder *dec;
   opus_int16 pcm[TEST_FRAME];
   opus_int16 out[TEST_FRAME];
   unsigned char packet[4][TEST_MAX_PACKET];
   int len[4];

   generate_voice(pcm, TEST_FRAME);
   enc = create_encoder(32000, 0, 30);
   for (i = 0; i < 4; i++)
   {
      len[i] = opus_encode(enc, pcm, TEST_FRAME, packet[i], TEST_MAX_PACKET);
      if (len[i] <= 0)
         test_failed();
      if (opus_packet_has_lbrr(packet[i], len[i]) != 0)
         test_failed();
   }
   opus_encoder_destroy(enc);

   dec = create_decoder();
   ret = opus_decode(dec, packet[1], len[1], out, TEST_FRAME, 1);
   if (ret != TEST_FRAME)
      test_failed();
   opus_decoder_destroy(dec);

   enc = create_encoder(32000, 20, 30);
   for (i = 0; i < 4; i++)
   {
      len[i] = opus_encode(enc, pcm, TEST_FRAME, packet[i], TEST_MAX_PACKET);
      if (len[i] <= 0)
         test_failed();
   }
   if (!opus_packet_has_lbrr(packet[3], len[3]))
      test_failed();
   opus_encoder_destroy(enc);
   dec = create_decoder();
   ret = opus_decode(dec, packet[0], len[0], out, TEST_FRAME, 0);
   if (ret != TEST_FRAME)
      test_failed();
   ret = opus_decode(dec, NULL, 0, out, TEST_FRAME, 0);
   if (ret != TEST_FRAME)
      test_failed();
   ret = opus_decode(dec, NULL, 0, out, TEST_FRAME, 0);
   if (ret != TEST_FRAME)
      test_failed();
   ret = opus_decode(dec, packet[3], len[3], out, TEST_FRAME, 1);
   if (ret != TEST_FRAME)
      test_failed();
   ret = opus_decode(dec, packet[3], len[3], out, TEST_FRAME, 0);
   if (ret != TEST_FRAME)
      test_failed();
   opus_decoder_destroy(dec);
}

int main(void)
{
   static const int bitrates[] = {24000, 32000, 48000};
   int i;
   iseed = 0;
   fprintf(stderr, "Testing %s in-band redundancy.\n",
         opus_get_version_string());
   test_api_edges();
   test_no_loss_and_burst();
   for (i = 0; i < (int)(sizeof(bitrates)/sizeof(bitrates[0])); i++)
      test_recovery_and_bandwidth(bitrates[i]);
   fprintf(stderr, "All in-band redundancy tests passed.\n");
   return 0;
}
