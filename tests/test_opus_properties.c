/*
 * Opus decoder property test:
 *  - 200 malformed packets (truncated, bad frame length, duplicate TOC,
 *    oversized frame), alternated with a known-good anchor packet;
 *  - 50 valid packets, decoded sample-by-sample against a reference libopus.
 *
 * Every case runs in a separate child process. Address/leak sanitizer reports
 * therefore identify both the failing case and the packet offset recorded in
 * the manifest.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "opus.h"

#define VALID_CASE_COUNT 50
#define MALFORMED_CASE_COUNT 200
#define MAX_CASE_BYTES 65536
#define MAX_DECODE_SAMPLES 5760
#define MAX_DETAIL 192
#define MAX_PATH 1024

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
   int id;
   int fs;
   int channels;
   int application;
   int frame_units;
   int nframes;
   int bitrate;
   int complexity;
   unsigned char *packet;
   int len;
} ValidCase;

typedef struct {
   int id;
   const char *category;
   char detail[MAX_DETAIL];
   int fs;
   int channels;
   unsigned char *packet;
   int len;
   int fault_offset;
   const unsigned char *anchor;
   int anchor_len;
} MalformedCase;

typedef struct {
   void *handle;
   OpusDecoder *(*decoder_create)(opus_int32, int, int *);
   int (*decode)(OpusDecoder *, const unsigned char *, opus_int32,
         opus_int16 *, int, int);
   void (*decoder_destroy)(OpusDecoder *);
   const char *(*get_version_string)(void);
} RefApi;

typedef struct {
   int ok;
   int category_ok[4];
   int category_total[4];
   int failures;
   int crashes;
   int memory_errors;
   int leaks;
   int ubsan;
   int state_errors;
   int valid_ok;
   int valid_mismatch;
   int valid_errors;
   int accepted_malformed;
   int rejected_malformed;
} Results;

typedef enum {
   CASE_OK = 0,
   CASE_CRASH,
   CASE_MEMORY,
   CASE_LEAK,
   CASE_UBSAN,
   CASE_STATE,
   CASE_VALID_MISMATCH,
   CASE_VALID_ERROR,
   CASE_HARNESS
} CaseStatus;

static void fail(const char *message)
{
   fprintf(stderr, "test_opus_properties: %s\n", message);
   exit(2);
}

static void *xmalloc(size_t size)
{
   void *ptr = malloc(size);
   if (ptr == NULL && size != 0)
      fail("out of memory");
   return ptr;
}

static void *xcalloc(size_t count, size_t size)
{
   void *ptr = calloc(count, size);
   if (ptr == NULL && count != 0 && size != 0)
      fail("out of memory");
   return ptr;
}

static void make_dir(const char *path)
{
   if (mkdir(path, 0777) != 0 && errno != EEXIST) {
      char message[MAX_PATH + 64];
      snprintf(message, sizeof(message), "cannot create %s: %s", path,
            strerror(errno));
      fail(message);
   }
}

static void path_join(char *dst, size_t size, const char *a, const char *b)
{
   if (snprintf(dst, size, "%s/%s", a, b) >= (int)size)
      fail("path too long");
}

static int write_bytes(const char *path, const void *data, size_t size)
{
   FILE *file = fopen(path, "wb");
   if (file == NULL)
      return -1;
   if (size != 0 && fwrite(data, 1, size, file) != size) {
      fclose(file);
      return -1;
   }
   return fclose(file);
}

static uint32_t rng_next(uint32_t *state)
{
   *state = *state * 1664525u + 1013904223u;
   return *state;
}

static uint16_t clamp_s16(double value)
{
   if (value > 32760.0)
      value = 32760.0;
   if (value < -32760.0)
      value = -32760.0;
   return (uint16_t)(int16_t)lrint(value);
}

static void fill_pcm(opus_int16 *pcm, int samples, int channels, int fs,
      uint32_t seed)
{
   int i;
   int c;
   uint32_t state = seed | 1u;
   for (i = 0; i < samples; i++) {
      double t = (double)i / (double)fs;
      double noise = ((int)((rng_next(&state) >> 16) & 0x3fff) - 8192) * 0.32;
      for (c = 0; c < channels; c++) {
         double phase = 0.37 * c;
         double value = 8600.0 * sin(2.0 * M_PI * (317.0 + 23.0 * c) * t + phase)
               + 2400.0 * sin(2.0 * M_PI * 1291.0 * t + 0.6 + phase)
               + noise;
         pcm[i * channels + c] = (opus_int16)clamp_s16(value);
      }
   }
}

static void load_reference(RefApi *ref, const char *requested_path)
{
   const char *paths[3];
   int path_count = 0;
   int i;
   memset(ref, 0, sizeof(*ref));
   if (requested_path != NULL && requested_path[0] != '\0')
      paths[path_count++] = requested_path;
   paths[path_count++] = "libopus.so.0";
#if defined(__x86_64__)
   paths[path_count++] = "/usr/lib/x86_64-linux-gnu/libopus.so.0";
#endif
   for (i = 0; i < path_count && ref->handle == NULL; i++)
      ref->handle = dlopen(paths[i], RTLD_NOW | RTLD_LOCAL);
   if (ref->handle == NULL)
      fail("cannot load reference libopus (set OPUS_REFERENCE_LIB)");
   *(void **)(&ref->decoder_create) = dlsym(ref->handle, "opus_decoder_create");
   *(void **)(&ref->decode) = dlsym(ref->handle, "opus_decode");
   *(void **)(&ref->decoder_destroy) = dlsym(ref->handle, "opus_decoder_destroy");
   *(void **)(&ref->get_version_string) =
         dlsym(ref->handle, "opus_get_version_string");
   if (ref->decoder_create == NULL || ref->decode == NULL ||
         ref->decoder_destroy == NULL || ref->get_version_string == NULL)
      fail("reference libopus is missing the decoder API");
   if ((void *)(uintptr_t)&opus_decode ==
         (void *)(uintptr_t)ref->decode)
      fail("reference decoder resolved to the target decoder");
}

static int configured_encoder(int id, ValidCase *sample)
{
   static const int sample_rates[] = {8000, 12000, 16000, 24000, 48000};
   static const int frame_units[] = {1, 2, 4, 8, 16, 24};
   static const int applications[] = {
      OPUS_APPLICATION_AUDIO,
      OPUS_APPLICATION_VOIP,
      OPUS_APPLICATION_RESTRICTED_LOWDELAY
   };
   sample->id = id;
   sample->fs = sample_rates[id % 5];
   sample->channels = 1 + ((id / 5) % 2);
   sample->application = applications[(id / 7) % 3];
   sample->frame_units = frame_units[(id / 3) % 6];
   sample->bitrate = 7000 + (id * 997) % 57000;
   sample->complexity = id % 10;
   sample->nframes = 1 + ((id * 5 + 3) % (48 / sample->frame_units));
   sample->packet = NULL;
   sample->len = 0;
   return 0;
}

static void generate_valid_samples(ValidCase *samples)
{
   unsigned char encoded[4096];
   unsigned char repacketized[MAX_CASE_BYTES];
   int id;
   for (id = 0; id < VALID_CASE_COUNT; id++) {
      ValidCase *sample = &samples[id];
      OpusEncoder *encoder;
      OpusRepacketizer *repacketizer;
      opus_int16 *pcm;
      int frame_samples;
      int total_samples;
      int err = OPUS_OK;
      int frame;
      int ret;
      configured_encoder(id, sample);
      frame_samples = sample->fs * sample->frame_units / 400;
      total_samples = frame_samples * sample->nframes;
      encoder = opus_encoder_create(sample->fs, sample->channels,
            sample->application, &err);
      if (err != OPUS_OK || encoder == NULL)
         fail("target encoder creation failed");
      if (opus_encoder_ctl(encoder, OPUS_SET_BITRATE(sample->bitrate)) != OPUS_OK ||
            opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(sample->complexity)) != OPUS_OK ||
            opus_encoder_ctl(encoder, OPUS_SET_VBR(1)) != OPUS_OK ||
            opus_encoder_ctl(encoder, OPUS_SET_DTX(0)) != OPUS_OK ||
            opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(0)) != OPUS_OK ||
            opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(0)) != OPUS_OK)
         fail("target encoder configuration failed");
      repacketizer = opus_repacketizer_create();
      if (repacketizer == NULL)
         fail("repacketizer creation failed");
      opus_repacketizer_init(repacketizer);
      pcm = (opus_int16 *)xmalloc((size_t)total_samples *
            sample->channels * sizeof(*pcm));
      fill_pcm(pcm, total_samples, sample->channels, sample->fs,
            0x51ed270bu ^ (uint32_t)id);
      for (frame = 0; frame < sample->nframes; frame++) {
         ret = opus_encode(encoder,
               pcm + (size_t)frame * frame_samples * sample->channels,
               frame_samples, encoded, (opus_int32)sizeof(encoded));
         if (ret <= 0)
            fail("target encoder produced an invalid packet");
         if (opus_repacketizer_cat(repacketizer, encoded, ret) != OPUS_OK)
            fail("repacketizer rejected an encoded frame");
      }
      ret = opus_repacketizer_out_range(repacketizer, 0, sample->nframes,
            repacketized, (opus_int32)sizeof(repacketized));
      if (ret <= 0)
         fail("repacketizer output failed");
      sample->packet = (unsigned char *)xmalloc((size_t)ret);
      memcpy(sample->packet, repacketized, (size_t)ret);
      sample->len = ret;
      free(pcm);
      opus_repacketizer_destroy(repacketizer);
      opus_encoder_destroy(encoder);
   }
}

static unsigned char packet_header(const unsigned char *base, int code)
{
   return (unsigned char)((base[0] & 0xfc) | code);
}

static void six_bit_pattern(unsigned char *dst, int len, uint32_t seed)
{
   uint32_t state = seed | 1u;
   int i;
   for (i = 0; i < len; i++)
      dst[i] = (unsigned char)(rng_next(&state) >> 24);
}

static void encode_size(unsigned int size, unsigned char out[2], int *bytes)
{
   if (size < 252) {
      out[0] = (unsigned char)size;
      *bytes = 1;
   } else {
      out[0] = (unsigned char)(252 + (size & 3));
      out[1] = (unsigned char)((size - out[0]) >> 2);
      *bytes = 2;
   }
}

static void add_malformed(MalformedCase *cases, int *count,
      const char *category, const char *detail, const ValidCase *base,
      const unsigned char *packet, int len, int fault_offset)
{
   MalformedCase *entry = &cases[*count];
   if (*count >= MALFORMED_CASE_COUNT)
      fail("too many malformed cases");
   if (len <= 0 || len > MAX_CASE_BYTES)
      fail("malformed packet size outside test bounds");
   entry->id = *count;
   entry->category = category;
   snprintf(entry->detail, sizeof(entry->detail), "%s", detail);
   entry->fs = base->fs;
   entry->channels = base->channels;
   entry->packet = (unsigned char *)xmalloc((size_t)len);
   memcpy(entry->packet, packet, (size_t)len);
   entry->len = len;
   entry->fault_offset = fault_offset;
   entry->anchor = base->packet;
   entry->anchor_len = base->len;
   (*count)++;
}

static void generate_truncated(MalformedCase *cases, int *count,
      const ValidCase *samples, int local_id)
{
   const ValidCase *base = &samples[(local_id * 11 + 3) % VALID_CASE_COUNT];
   static const int fractions[] = {0, 0, 1, 2, 3};
   int target;
   int len;
   char detail[MAX_DETAIL];
   if (fractions[local_id % 5] == 0)
      target = 1;
   else if (fractions[local_id % 5] == 1)
      target = base->len > 2 ? 2 : 1;
   else if (fractions[local_id % 5] == 2)
      target = base->len / 4;
   else if (fractions[local_id % 5] == 3)
      target = base->len / 2;
   else
      target = base->len - 1;
   len = target;
   if (len < 1)
      len = 1;
   if (len >= base->len)
      len = base->len - 1;
   if (len < 1)
      len = 1;
   snprintf(detail, sizeof(detail), "cut to %d of %d bytes", len, base->len);
   add_malformed(cases, count, "truncated", detail, base,
         base->packet, len, len);
}

static void generate_bad_length(MalformedCase *cases, int *count,
      const ValidCase *samples, int local_id)
{
   const ValidCase *base = &samples[(local_id * 7 + 5) % VALID_CASE_COUNT];
   unsigned char packet[128];
   unsigned char size_bytes[2];
   int len = 0;
   int fault = 1;
   int sub = local_id % 10;
   int bytes = 0;
   char detail[MAX_DETAIL];
   memset(packet, 0, sizeof(packet));
   switch (sub) {
   case 0:
      packet[0] = packet_header(base->packet, 1);
      packet[1] = 0x42;
      len = 2;
      snprintf(detail, sizeof(detail), "code 1 with odd payload byte count");
      break;
   case 1:
      packet[0] = packet_header(base->packet, 2);
      packet[1] = 4;
      packet[2] = 0x11;
      len = 3;
      snprintf(detail, sizeof(detail), "code 2 size 4 exceeds 1-byte remainder");
      break;
   case 2:
      packet[0] = packet_header(base->packet, 2);
      encode_size(1275, size_bytes, &bytes);
      packet[1] = size_bytes[0];
      packet[2] = size_bytes[1];
      len = 3;
      snprintf(detail, sizeof(detail), "code 2 size 1275 has no payload");
      break;
   case 3:
      packet[0] = packet_header(base->packet, 3);
      packet[1] = 0;
      len = 2;
      snprintf(detail, sizeof(detail), "code 3 with zero frames");
      break;
   case 4:
      packet[0] = packet_header(base->packet, 3);
      packet[1] = 63;
      six_bit_pattern(packet + 2, 63, 0x1234u + (uint32_t)local_id);
      len = 65;
      snprintf(detail, sizeof(detail), "code 3 frame count 63 exceeds duration");
      break;
   case 5:
      packet[0] = packet_header(base->packet, 3);
      packet[1] = 0x83;
      packet[2] = 100;
      packet[3] = 100;
      six_bit_pattern(packet + 4, 10, 0x2345u + (uint32_t)local_id);
      len = 14;
      fault = 2;
      snprintf(detail, sizeof(detail), "code 3 VBR size pair exceeds remainder");
      break;
   case 6:
      packet[0] = packet_header(base->packet, 3);
      packet[1] = 0x43;
      packet[2] = 255;
      packet[3] = 255;
      len = 4;
      fault = 2;
      snprintf(detail, sizeof(detail), "code 3 padding count overruns packet");
      break;
   case 7:
      packet[0] = packet_header(base->packet, 3);
      packet[1] = 3;
      packet[2] = 0xaa;
      len = 3;
      snprintf(detail, sizeof(detail), "code 3 CBR payload not divisible by 3");
      break;
   case 8:
      packet[0] = packet_header(base->packet, 2);
      encode_size(252, size_bytes, &bytes);
      packet[1] = size_bytes[0];
      packet[2] = size_bytes[1];
      packet[3] = 0x55;
      len = 4;
      snprintf(detail, sizeof(detail), "code 2 two-byte size exceeds remainder");
      break;
   default:
      packet[0] = packet_header(base->packet, 3);
      packet[1] = 0x02;
      packet[2] = 255;
      packet[3] = 255;
      packet[4] = 255;
      packet[5] = 255;
      len = 6;
      fault = 2;
      snprintf(detail, sizeof(detail), "code 3 VBR two 1275-byte sizes, no data");
      break;
   }
   add_malformed(cases, count, "bad_frame_length", detail, base,
         packet, len, fault);
}

static void generate_duplicate_toc(MalformedCase *cases, int *count,
      const ValidCase *samples, int local_id)
{
   const ValidCase *base = &samples[(local_id * 13 + 9) % VALID_CASE_COUNT];
   unsigned char *packet;
   int len;
   int fault;
   int sub = local_id % 10;
   int repeats;
   int split;
   char detail[MAX_DETAIL];
   if (sub == 0) {
      len = base->len + 1;
      packet = (unsigned char *)xmalloc((size_t)len);
      packet[0] = base->packet[0];
      packet[1] = base->packet[0];
      memcpy(packet + 2, base->packet + 1, (size_t)base->len - 1);
      fault = 1;
      snprintf(detail, sizeof(detail), "TOC inserted at payload offset 1");
   } else if (sub <= 3) {
      repeats = sub == 1 ? 2 : (sub == 2 ? 4 : 8);
      len = base->len + repeats - 1;
      packet = (unsigned char *)xmalloc((size_t)len);
      memset(packet, base->packet[0], (size_t)repeats);
      memcpy(packet + repeats, base->packet + 1, (size_t)base->len - 1);
      fault = 1;
      snprintf(detail, sizeof(detail), "TOC repeated %d times", repeats);
   } else if (sub == 4) {
      len = base->len * 2;
      packet = (unsigned char *)xmalloc((size_t)len);
      memcpy(packet, base->packet, (size_t)base->len);
      memcpy(packet + base->len, base->packet, (size_t)base->len);
      fault = base->len;
      snprintf(detail, sizeof(detail), "two complete packets concatenated");
   } else if (sub == 5) {
      split = base->len / 2;
      len = base->len + 1;
      packet = (unsigned char *)xmalloc((size_t)len);
      memcpy(packet, base->packet, (size_t)split);
      packet[split] = base->packet[0];
      memcpy(packet + split + 1, base->packet + split,
            (size_t)(base->len - split));
      fault = split;
      snprintf(detail, sizeof(detail), "TOC inserted at payload offset %d",
            split);
   } else if (sub == 6) {
      len = 2;
      packet = (unsigned char *)xmalloc(2);
      packet[0] = base->packet[0];
      packet[1] = base->packet[0];
      fault = 1;
      snprintf(detail, sizeof(detail), "packet contains only two TOC bytes");
   } else if (sub == 7) {
      len = base->len + 1;
      packet = (unsigned char *)xmalloc((size_t)len);
      packet[0] = base->packet[0];
      memcpy(packet + 1, base->packet, (size_t)base->len);
      fault = 1;
      snprintf(detail, sizeof(detail), "valid TOC nested at payload offset 1");
   } else if (sub == 8) {
      len = base->len + 2;
      packet = (unsigned char *)xmalloc((size_t)len);
      packet[0] = base->packet[0];
      packet[1] = base->packet[0];
      packet[2] = base->packet[0];
      memcpy(packet + 3, base->packet + 1, (size_t)base->len - 1);
      fault = 1;
      snprintf(detail, sizeof(detail), "TOC repeated 3 times before payload");
   } else {
      len = base->len + 8;
      packet = (unsigned char *)xmalloc((size_t)len);
      memcpy(packet, base->packet, (size_t)base->len);
      memset(packet + base->len, base->packet[0], 8);
      fault = base->len;
      snprintf(detail, sizeof(detail), "TOC repeated in trailing bytes");
   }
   add_malformed(cases, count, "duplicate_toc", detail, base,
         packet, len, fault);
   free(packet);
}

static void generate_oversized(MalformedCase *cases, int *count,
      const ValidCase *samples, int local_id)
{
   const ValidCase *base = &samples[(local_id * 17 + 1) % VALID_CASE_COUNT];
   static const int sizes[] = {1276, 1500, 2048, 4096, 8192, 16384};
   unsigned char *packet;
   unsigned char size_bytes[2];
   int bytes = 0;
   int len;
   int fault = 1;
   int sub = local_id % 10;
   char detail[MAX_DETAIL];
   if (sub < 6) {
      int payload = sizes[sub];
      len = payload + 1;
      packet = (unsigned char *)xmalloc((size_t)len);
      packet[0] = packet_header(base->packet, 0);
      six_bit_pattern(packet + 1, payload, 0x3456u + (uint32_t)local_id);
      snprintf(detail, sizeof(detail), "code 0 frame payload is %d bytes",
            payload);
   } else if (sub == 6) {
      int payload = 2552;
      len = payload + 1;
      packet = (unsigned char *)xmalloc((size_t)len);
      packet[0] = packet_header(base->packet, 1);
      six_bit_pattern(packet + 1, payload, 0x4567u + (uint32_t)local_id);
      snprintf(detail, sizeof(detail), "code 1 carries two 1276-byte frames");
   } else if (sub == 7) {
      len = 1 + 1 + 2 + 2 + 1275 + 1275;
      packet = (unsigned char *)xmalloc((size_t)len);
      packet[0] = packet_header(base->packet, 3);
      packet[1] = 0x82;
      encode_size(1275, size_bytes, &bytes);
      packet[2] = size_bytes[0];
      packet[3] = size_bytes[1];
      encode_size(1275, size_bytes, &bytes);
      packet[4] = size_bytes[0];
      packet[5] = size_bytes[1];
      six_bit_pattern(packet + 6, 2550, 0x5678u + (uint32_t)local_id);
      fault = 2;
      snprintf(detail, sizeof(detail), "code 3 VBR has two 1275-byte frames");
   } else if (sub == 8) {
      int payload = 1275 * 48;
      len = 2 + payload;
      packet = (unsigned char *)xmalloc((size_t)len);
      packet[0] = 16 << 3;
      packet[1] = 48;
      six_bit_pattern(packet + 2, payload, 0x6789u + (uint32_t)local_id);
      snprintf(detail, sizeof(detail), "code 3 CBR has 48 x 1275-byte frames");
   } else {
      len = 3 + 1274;
      packet = (unsigned char *)xmalloc((size_t)len);
      packet[0] = packet_header(base->packet, 2);
      encode_size(1275, size_bytes, &bytes);
      packet[1] = size_bytes[0];
      packet[2] = size_bytes[1];
      six_bit_pattern(packet + 3, 1274, 0x789au + (uint32_t)local_id);
      snprintf(detail, sizeof(detail), "code 2 frame size 1275 has 1274 bytes");
   }
   add_malformed(cases, count, "oversized_frame", detail, base,
         packet, len, fault);
   free(packet);
}

static void generate_malformed(MalformedCase *cases, const ValidCase *samples)
{
   int count = 0;
   int i;
   for (i = 0; i < MALFORMED_CASE_COUNT; i++) {
      switch (i % 4) {
      case 0:
         generate_truncated(cases, &count, samples, i / 4);
         break;
      case 1:
         generate_bad_length(cases, &count, samples, i / 4);
         break;
      case 2:
         generate_duplicate_toc(cases, &count, samples, i / 4);
         break;
      default:
         generate_oversized(cases, &count, samples, i / 4);
         break;
      }
   }
   if (count != MALFORMED_CASE_COUNT)
      fail("not enough malformed cases were generated");
}

static void redirect_stderr(const char *path)
{
   int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
   if (fd < 0 || dup2(fd, STDERR_FILENO) < 0)
      _exit(120);
   if (fd != STDERR_FILENO)
      close(fd);
}

static int valid_child(const ValidCase *sample, const RefApi *ref,
      const char *result_path, const char *corpus_path, const char *log_path,
      const char *counterexample_dir)
{
   OpusDecoder *target_decoder = NULL;
   OpusDecoder *reference_decoder = NULL;
   opus_int16 *target_pcm = NULL;
   opus_int16 *reference_pcm = NULL;
   FILE *result;
   int max_samples = sample->fs * 120 / 1000;
   int target_ret;
   int reference_ret;
   int err = OPUS_OK;
   int first_diff = -1;
   int sample_index;
   (void)log_path;
   (void)corpus_path;
   redirect_stderr(log_path);
   result = fopen(result_path, "w");
   if (result == NULL)
      return 120;
   target_pcm = (opus_int16 *)xmalloc((size_t)max_samples *
         sample->channels * sizeof(*target_pcm));
   reference_pcm = (opus_int16 *)xmalloc((size_t)max_samples *
         sample->channels * sizeof(*reference_pcm));
   target_decoder = opus_decoder_create(sample->fs, sample->channels, &err);
   if (target_decoder == NULL || err != OPUS_OK) {
      fprintf(result, "status=target_decoder_error\nret=%d\n", err);
      fclose(result);
      return 3;
   }
   reference_decoder = ref->decoder_create(sample->fs, sample->channels, &err);
   if (reference_decoder == NULL || err != OPUS_OK) {
      fprintf(result, "status=reference_decoder_error\nret=%d\n", err);
      fclose(result);
      return 4;
   }
   target_ret = opus_decode(target_decoder, sample->packet, sample->len,
         target_pcm, max_samples, 0);
   reference_ret = ref->decode(reference_decoder, sample->packet, sample->len,
         reference_pcm, max_samples, 0);
   if (target_ret <= 0 || reference_ret <= 0) {
      fprintf(result, "status=valid_decode_error\ntarget=%d\nreference=%d\n",
            target_ret, reference_ret);
      opus_decoder_destroy(target_decoder);
      ref->decoder_destroy(reference_decoder);
      free(target_pcm);
      free(reference_pcm);
      fclose(result);
      return 5;
   }
   if (target_ret != reference_ret) {
      first_diff = 0;
   } else {
      int count = target_ret * sample->channels;
      for (sample_index = 0; sample_index < count; sample_index++) {
         if (target_pcm[sample_index] != reference_pcm[sample_index]) {
            first_diff = sample_index;
            break;
         }
      }
   }
   fprintf(result, "status=%s\ntarget_samples=%d\nreference_samples=%d\n",
         first_diff < 0 ? "ok" : "valid_mismatch", target_ret, reference_ret);
   if (first_diff >= 0) {
      char packet_path[MAX_PATH];
      char expected_path[MAX_PATH];
      char actual_path[MAX_PATH];
      char basename[64];
      int expected_value = first_diff < reference_ret * sample->channels ?
            reference_pcm[first_diff] : 0;
      int actual_value = first_diff < target_ret * sample->channels ?
            target_pcm[first_diff] : 0;
      snprintf(basename, sizeof(basename), "valid_%03d", sample->id);
      path_join(packet_path, sizeof(packet_path), counterexample_dir, basename);
      strncat(packet_path, ".opus", sizeof(packet_path) - strlen(packet_path) - 1);
      snprintf(basename, sizeof(basename), "valid_%03d_reference", sample->id);
      path_join(expected_path, sizeof(expected_path), counterexample_dir, basename);
      strncat(expected_path, ".pcm", sizeof(expected_path) - strlen(expected_path) - 1);
      snprintf(basename, sizeof(basename), "valid_%03d_target", sample->id);
      path_join(actual_path, sizeof(actual_path), counterexample_dir, basename);
      strncat(actual_path, ".pcm", sizeof(actual_path) - strlen(actual_path) - 1);
      if (write_bytes(packet_path, sample->packet, (size_t)sample->len) != 0 ||
            write_bytes(expected_path, reference_pcm,
               (size_t)reference_ret * sample->channels *
               sizeof(*reference_pcm)) != 0 ||
            write_bytes(actual_path, target_pcm,
               (size_t)target_ret * sample->channels *
               sizeof(*target_pcm)) != 0)
         fprintf(result, "dump_error=1\n");
      fprintf(result, "first_sample=%d\nchannel=%d\nexpected=%d\nactual=%d\n",
            first_diff / sample->channels, first_diff % sample->channels,
            expected_value, actual_value);
   }
   opus_decoder_destroy(target_decoder);
   ref->decoder_destroy(reference_decoder);
   free(target_pcm);
   free(reference_pcm);
   fclose(result);
   return first_diff < 0 ? 0 : 6;
}

static int malformed_child(const MalformedCase *test, const char *result_path,
      const char *log_path)
{
   OpusDecoder *decoder;
   opus_int16 *pcm;
   int max_samples = test->fs * 120 / 1000;
   int before_ret;
   int malformed_ret;
   int after_ret;
   int err = OPUS_OK;
   FILE *result;
   redirect_stderr(log_path);
   result = fopen(result_path, "w");
   if (result == NULL)
      return 120;
   decoder = opus_decoder_create(test->fs, test->channels, &err);
   if (decoder == NULL || err != OPUS_OK) {
      fprintf(result, "status=decoder_error\nret=%d\n", err);
      fclose(result);
      return 3;
   }
   pcm = (opus_int16 *)xmalloc((size_t)max_samples *
         test->channels * sizeof(*pcm));
   before_ret = opus_decode(decoder, test->anchor, test->anchor_len,
         pcm, max_samples, 0);
   malformed_ret = opus_decode(decoder, test->packet, test->len,
         pcm, max_samples, 0);
   after_ret = opus_decode(decoder, test->anchor, test->anchor_len,
         pcm, max_samples, 0);
   if (before_ret <= 0 || after_ret <= 0) {
      fprintf(result,
            "status=state_error\nbefore=%d\nmalformed=%d\nafter=%d\n",
            before_ret, malformed_ret, after_ret);
      opus_decoder_destroy(decoder);
      free(pcm);
      fclose(result);
      return 7;
   }
   fprintf(result, "status=ok\nbefore=%d\nmalformed=%d\nafter=%d\n",
         before_ret, malformed_ret, after_ret);
   opus_decoder_destroy(decoder);
   free(pcm);
   fclose(result);
   return 0;
}

static CaseStatus status_from_wait(int wait_status, const char *log_path,
      const char *result_path, int *exit_code)
{
   char buffer[16384];
   size_t used = 0;
   FILE *log;
   FILE *result;
   *exit_code = 0;
   if (WIFEXITED(wait_status))
      *exit_code = WEXITSTATUS(wait_status);
   else if (WIFSIGNALED(wait_status))
      *exit_code = 128 + WTERMSIG(wait_status);
   log = fopen(log_path, "r");
   if (log != NULL) {
      used = fread(buffer, 1, sizeof(buffer) - 1, log);
      fclose(log);
   }
   buffer[used] = '\0';
   if (strstr(buffer, "LeakSanitizer") != NULL ||
         strstr(buffer, "detected memory leaks") != NULL)
      return CASE_LEAK;
   if (strstr(buffer, "AddressSanitizer") != NULL ||
         strstr(buffer, "SEGV") != NULL ||
         strstr(buffer, "heap-buffer-overflow") != NULL ||
         strstr(buffer, "stack-buffer-overflow") != NULL ||
         strstr(buffer, "global-buffer-overflow") != NULL ||
         strstr(buffer, "use-after-free") != NULL)
      return CASE_MEMORY;
   if (strstr(buffer, "runtime error:") != NULL ||
         strstr(buffer, "UndefinedBehaviorSanitizer") != NULL)
      return CASE_UBSAN;
   result = fopen(result_path, "r");
   if (result != NULL) {
      used = fread(buffer, 1, sizeof(buffer) - 1, result);
      fclose(result);
      buffer[used] = '\0';
      if (strstr(buffer, "status=valid_mismatch") != NULL)
         return CASE_VALID_MISMATCH;
      if (strstr(buffer, "status=valid_decode_error") != NULL ||
            strstr(buffer, "status=target_decoder_error") != NULL)
         return CASE_VALID_ERROR;
      if (strstr(buffer, "status=state_error") != NULL)
         return CASE_STATE;
      if (strstr(buffer, "status=decoder_error") != NULL)
         return CASE_HARNESS;
      if (*exit_code != 0)
         return CASE_CRASH;
      if (strstr(buffer, "status=ok") != NULL)
         return CASE_OK;
   }
   return *exit_code == 0 ? CASE_OK : CASE_CRASH;
}

static const char *status_name(CaseStatus status)
{
   switch (status) {
   case CASE_OK:
      return "ok";
   case CASE_CRASH:
      return "crash";
   case CASE_MEMORY:
      return "memory_error";
   case CASE_LEAK:
      return "leak";
   case CASE_UBSAN:
      return "ubsan";
   case CASE_STATE:
      return "state_error";
   case CASE_VALID_MISMATCH:
      return "valid_mismatch";
   case CASE_VALID_ERROR:
      return "valid_decode_error";
   default:
      return "harness_error";
   }
}

static int category_index(const char *category)
{
   if (strcmp(category, "truncated") == 0)
      return 0;
   if (strcmp(category, "bad_frame_length") == 0)
      return 1;
   if (strcmp(category, "duplicate_toc") == 0)
      return 2;
   if (strcmp(category, "oversized_frame") == 0)
      return 3;
   return -1;
}

static int read_result_int(const char *path, const char *key)
{
   char line[256];
   size_t key_len = strlen(key);
   FILE *file = fopen(path, "r");
   int value = -999999;
   if (file == NULL)
      return value;
   while (fgets(line, sizeof(line), file) != NULL) {
      if (strncmp(line, key, key_len) == 0)
         value = atoi(line + key_len);
   }
   fclose(file);
   return value;
}

static void write_failure(const char *dir, const char *name,
      const char *category, int id, int offset, const char *detail,
      const char *status, int exit_code, const char *log_path,
      const char *packet_path)
{
   char path[MAX_PATH];
   FILE *file;
   snprintf(path, sizeof(path), "%s/failure_%03d.txt", dir, id);
   file = fopen(path, "w");
   if (file == NULL)
      return;
   fprintf(file, "id=%d\ncategory=%s\nfault_offset=%d\nstatus=%s\n",
         id, category, offset, status);
   fprintf(file, "detail=%s\nexit_code=%d\nlog=%s\npacket=%s\n",
         detail, exit_code, log_path, packet_path);
   fclose(file);
   (void)name;
}

static void run_valid_cases(ValidCase *samples, const RefApi *ref,
      const char *out_dir, Results *results)
{
   char corpus_dir[MAX_PATH];
   char results_dir[MAX_PATH];
   char logs_dir[MAX_PATH];
   char counter_dir[MAX_PATH];
   char manifest_path[MAX_PATH];
   FILE *manifest;
   int id;
   path_join(corpus_dir, sizeof(corpus_dir), out_dir, "corpus");
   path_join(results_dir, sizeof(results_dir), out_dir, "results");
   path_join(logs_dir, sizeof(logs_dir), out_dir, "logs");
   path_join(counter_dir, sizeof(counter_dir), out_dir, "counterexamples");
   path_join(manifest_path, sizeof(manifest_path), out_dir, "valid_manifest.tsv");
   manifest = fopen(manifest_path, "w");
   if (manifest == NULL)
      fail("cannot create valid manifest");
   fprintf(manifest,
         "id\tfs\tchannels\tapplication\tframe_units\tframes\tbitrate\t"
         "complexity\tbytes\tstatus\tfirst_sample\tlog\n");
   for (id = 0; id < VALID_CASE_COUNT; id++) {
      char packet_name[64];
      char packet_path[MAX_PATH];
      char result_path[MAX_PATH];
      char log_path[MAX_PATH];
      int wait_status;
      int exit_code;
      int first_sample = -1;
      pid_t pid;
      CaseStatus status;
      snprintf(packet_name, sizeof(packet_name), "valid_%03d.opus", id);
      path_join(packet_path, sizeof(packet_path), corpus_dir, packet_name);
      if (write_bytes(packet_path, samples[id].packet,
            (size_t)samples[id].len) != 0)
         fail("cannot write valid corpus packet");
      snprintf(packet_name, sizeof(packet_name), "valid_%03d.txt", id);
      path_join(result_path, sizeof(result_path), results_dir, packet_name);
      path_join(log_path, sizeof(log_path), logs_dir, packet_name);
      fflush(NULL);
      pid = fork();
      if (pid < 0)
         fail("fork failed");
      if (pid == 0) {
         fflush(NULL);
         exit(valid_child(&samples[id], ref, result_path, packet_path,
               log_path, counter_dir));
      }
      if (waitpid(pid, &wait_status, 0) < 0)
         fail("waitpid failed");
      status = status_from_wait(wait_status, log_path, result_path,
            &exit_code);
      first_sample = read_result_int(result_path, "first_sample=");
      fprintf(manifest,
            "%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%s\t%d\t%s\n",
            id, samples[id].fs, samples[id].channels, samples[id].application,
            samples[id].frame_units, samples[id].nframes, samples[id].bitrate,
            samples[id].complexity, samples[id].len, status_name(status),
            first_sample, log_path);
      if (status == CASE_OK) {
         results->valid_ok++;
      } else {
         results->failures++;
         if (status == CASE_VALID_MISMATCH) {
            results->valid_mismatch++;
            write_failure(counter_dir, "valid", "valid_packet", id, 0,
                  "target PCM differs from reference libopus", status_name(status),
                  exit_code, log_path, packet_path);
         } else if (status == CASE_VALID_ERROR) {
            results->valid_errors++;
            write_failure(counter_dir, "valid", "valid_packet", id, 0,
                  "valid packet did not decode", status_name(status),
                  exit_code, log_path, packet_path);
         } else {
            results->valid_errors++;
            write_failure(counter_dir, "valid", "valid_packet", id, 0,
                  "valid packet crashed or failed sanitizer",
                  status_name(status), exit_code, log_path, packet_path);
         }
      }
      fprintf(stdout, "valid %03d/%03d %-20s fs=%d ch=%d bytes=%d\n",
            id + 1, VALID_CASE_COUNT, status_name(status), samples[id].fs,
            samples[id].channels, samples[id].len);
   }
   fclose(manifest);
}

static void run_malformed_cases(MalformedCase *cases, const char *out_dir,
      Results *results)
{
   char corpus_dir[MAX_PATH];
   char results_dir[MAX_PATH];
   char logs_dir[MAX_PATH];
   char counter_dir[MAX_PATH];
   char manifest_path[MAX_PATH];
   FILE *manifest;
   int id;
   path_join(corpus_dir, sizeof(corpus_dir), out_dir, "corpus");
   path_join(results_dir, sizeof(results_dir), out_dir, "results");
   path_join(logs_dir, sizeof(logs_dir), out_dir, "logs");
   path_join(counter_dir, sizeof(counter_dir), out_dir, "counterexamples");
   path_join(manifest_path, sizeof(manifest_path), out_dir,
         "malformed_manifest.tsv");
   manifest = fopen(manifest_path, "w");
   if (manifest == NULL)
      fail("cannot create malformed manifest");
   fprintf(manifest,
         "id\tcategory\tfs\tchannels\tbytes\tfault_offset\tstatus\t"
         "malformed_ret\tafter_ret\tlog\tpacket\n");
   for (id = 0; id < MALFORMED_CASE_COUNT; id++) {
      char packet_name[96];
      char packet_path[MAX_PATH];
      char result_path[MAX_PATH];
      char log_path[MAX_PATH];
      int wait_status;
      int exit_code;
      int malformed_ret;
      int after_ret;
      pid_t pid;
      CaseStatus status;
      snprintf(packet_name, sizeof(packet_name), "malformed_%03d_%s.opus",
            id, cases[id].category);
      path_join(packet_path, sizeof(packet_path), corpus_dir, packet_name);
      if (write_bytes(packet_path, cases[id].packet,
            (size_t)cases[id].len) != 0)
         fail("cannot write malformed corpus packet");
      snprintf(packet_name, sizeof(packet_name), "malformed_%03d.txt", id);
      path_join(result_path, sizeof(result_path), results_dir, packet_name);
      path_join(log_path, sizeof(log_path), logs_dir, packet_name);
      fflush(NULL);
      pid = fork();
      if (pid < 0)
         fail("fork failed");
      if (pid == 0) {
         fflush(NULL);
         exit(malformed_child(&cases[id], result_path, log_path));
      }
      if (waitpid(pid, &wait_status, 0) < 0)
         fail("waitpid failed");
      status = status_from_wait(wait_status, log_path, result_path,
            &exit_code);
      malformed_ret = read_result_int(result_path, "malformed=");
      after_ret = read_result_int(result_path, "after=");
      fprintf(manifest, "%d\t%s\t%d\t%d\t%d\t%d\t%s\t%d\t%d\t%s\t%s\n",
            id, cases[id].category, cases[id].fs, cases[id].channels,
            cases[id].len, cases[id].fault_offset, status_name(status),
            malformed_ret, after_ret, log_path, packet_path);
      {
         int category = category_index(cases[id].category);
         if (category >= 0) {
            results->category_total[category]++;
            if (status == CASE_OK)
               results->category_ok[category]++;
         }
      }
      if (status == CASE_OK) {
         results->ok++;
         if (malformed_ret >= 0)
            results->accepted_malformed++;
         else
            results->rejected_malformed++;
      } else {
         results->failures++;
         if (status == CASE_MEMORY)
            results->memory_errors++;
         else if (status == CASE_LEAK)
            results->leaks++;
         else if (status == CASE_UBSAN)
            results->ubsan++;
         else if (status == CASE_STATE)
            results->state_errors++;
         else
            results->crashes++;
         write_failure(counter_dir, "malformed", cases[id].category, id,
               cases[id].fault_offset, cases[id].detail, status_name(status),
               exit_code, log_path, packet_path);
      }
      fprintf(stdout,
            "malformed %03d/%03d %-20s %-18s bytes=%6d offset=%6d ret=%d\n",
            id + 1, MALFORMED_CASE_COUNT, cases[id].category,
            status_name(status), cases[id].len, cases[id].fault_offset,
            malformed_ret);
   }
   fclose(manifest);
}

static void write_summary(const char *out_dir, const RefApi *ref,
      const Results *results)
{
   char path[MAX_PATH];
   FILE *file;
   path_join(path, sizeof(path), out_dir, "summary.txt");
   file = fopen(path, "w");
   if (file == NULL)
      fail("cannot create summary");
   fprintf(file, "Opus decoder property test\n");
   fprintf(file, "target_version=%s\n", opus_get_version_string());
   fprintf(file, "reference_version=%s\n", ref->get_version_string());
   fprintf(file, "malformed_cases=%d\n", MALFORMED_CASE_COUNT);
   fprintf(file, "  passed=%d\n", results->ok);
   fprintf(file, "  truncated=%d/%d\n",
         results->category_ok[0], results->category_total[0]);
   fprintf(file, "  bad_frame_length=%d/%d\n",
         results->category_ok[1], results->category_total[1]);
   fprintf(file, "  duplicate_toc=%d/%d\n",
         results->category_ok[2], results->category_total[2]);
   fprintf(file, "  oversized_frame=%d/%d\n",
         results->category_ok[3], results->category_total[3]);
   fprintf(file, "  accepted_by_decoder=%d\n", results->accepted_malformed);
   fprintf(file, "  rejected_by_decoder=%d\n", results->rejected_malformed);
   fprintf(file, "  crashes=%d\n", results->crashes);
   fprintf(file, "  memory_errors=%d\n", results->memory_errors);
   fprintf(file, "  leaks=%d\n", results->leaks);
   fprintf(file, "  ubsan_errors=%d\n", results->ubsan);
   fprintf(file, "  decoder_state_errors=%d\n", results->state_errors);
   fprintf(file, "valid_cases=%d\n", VALID_CASE_COUNT);
   fprintf(file, "  passed=%d\n", results->valid_ok);
   fprintf(file, "  pcm_mismatches=%d\n", results->valid_mismatch);
   fprintf(file, "  decode_errors=%d\n", results->valid_errors);
   fprintf(file, "failures=%d\n", results->failures);
   fprintf(file, "result=%s\n", results->failures == 0 ? "PASS" : "FAIL");
   fclose(file);

   path_join(path, sizeof(path), out_dir, "summary.md");
   file = fopen(path, "w");
   if (file == NULL)
      fail("cannot create markdown summary");
   fprintf(file, "# Opus decoder property test\n\n");
   fprintf(file, "- Target: `%s`\n", opus_get_version_string());
   fprintf(file, "- Reference: `%s`\n", ref->get_version_string());
   fprintf(file, "- Result: **%s**\n\n",
         results->failures == 0 ? "PASS" : "FAIL");
   fprintf(file, "| Check | Pass | Fail |\n|---|---:|---:|\n");
   fprintf(file, "| 200 malformed packets | %d | %d |\n", results->ok,
         MALFORMED_CASE_COUNT - results->ok);
   fprintf(file, "| Truncated (50) | %d | %d |\n",
         results->category_ok[0], results->category_total[0] - results->category_ok[0]);
   fprintf(file, "| Bad frame length (50) | %d | %d |\n",
         results->category_ok[1], results->category_total[1] - results->category_ok[1]);
   fprintf(file, "| Duplicate TOC (50) | %d | %d |\n",
         results->category_ok[2], results->category_total[2] - results->category_ok[2]);
   fprintf(file, "| Oversized frame (50) | %d | %d |\n",
         results->category_ok[3], results->category_total[3] - results->category_ok[3]);
   fprintf(file, "| Crash | - | %d |\n", results->crashes);
   fprintf(file, "| Out-of-bounds / memory | - | %d |\n",
         results->memory_errors);
   fprintf(file, "| Leak | - | %d |\n", results->leaks);
   fprintf(file, "| UBSan | - | %d |\n", results->ubsan);
   fprintf(file, "| Decoder state after malformed packet | - | %d |\n",
         results->state_errors);
   fprintf(file, "| 50 valid packets vs reference PCM | %d | %d |\n",
         results->valid_ok, results->valid_mismatch + results->valid_errors);
   fprintf(file, "\nMalformed packets accepted by the decoder: %d; rejected: %d.\n",
         results->accepted_malformed, results->rejected_malformed);
   fprintf(file, "\nFailures, when present, are in `counterexamples/` and "
         "carry the packet offset in `failure_*.txt`.\n");
   fclose(file);
}

static void write_counterexample_readme(const char *out_dir, const Results *results)
{
   char path[MAX_PATH];
   FILE *file;
   path_join(path, sizeof(path), out_dir, "counterexamples/README.txt");
   file = fopen(path, "w");
   if (file == NULL)
      return;
   if (results->failures == 0) {
      fprintf(file,
            "No failing counterexamples.\n\n"
            "All 200 malformed packets completed under ASan/UBSan/LSan, "
            "and all 50 valid packets matched reference libopus PCM "
            "sample-by-sample.\n");
   } else {
      fprintf(file,
            "Counterexamples are named by test id. For malformed packets the "
            "corresponding .opus file is in ../corpus/ and failure_<id>.txt "
            "records the byte offset that introduced the malformed frame.\n");
   }
   fclose(file);
}

int main(int argc, char **argv)
{
   const char *out_dir = "artifacts/opus_properties";
   const char *reference_path = getenv("OPUS_REFERENCE_LIB");
   ValidCase *valid_cases;
   MalformedCase *malformed_cases;
   RefApi ref;
   Results results;
   char path[MAX_PATH];
   int i;
   for (i = 1; i < argc; i++) {
      if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
         out_dir = argv[++i];
      } else if (strcmp(argv[i], "--reference") == 0 && i + 1 < argc) {
         reference_path = argv[++i];
      } else {
         fprintf(stderr,
               "usage: %s [--output DIR] [--reference LIBOPUS.SO]\n", argv[0]);
         return 2;
      }
   }
   memset(&results, 0, sizeof(results));
   make_dir(out_dir);
   path_join(path, sizeof(path), out_dir, "corpus");
   make_dir(path);
   path_join(path, sizeof(path), out_dir, "results");
   make_dir(path);
   path_join(path, sizeof(path), out_dir, "logs");
   make_dir(path);
   path_join(path, sizeof(path), out_dir, "counterexamples");
   make_dir(path);
   load_reference(&ref, reference_path);
   fprintf(stdout, "target: %s\nreference: %s\n", opus_get_version_string(),
         ref.get_version_string());
   valid_cases = (ValidCase *)xcalloc(VALID_CASE_COUNT, sizeof(*valid_cases));
   malformed_cases = (MalformedCase *)xcalloc(MALFORMED_CASE_COUNT,
         sizeof(*malformed_cases));
   generate_valid_samples(valid_cases);
   generate_malformed(malformed_cases, valid_cases);
   run_valid_cases(valid_cases, &ref, out_dir, &results);
   run_malformed_cases(malformed_cases, out_dir, &results);
   write_summary(out_dir, &ref, &results);
   write_counterexample_readme(out_dir, &results);
   for (i = 0; i < VALID_CASE_COUNT; i++)
      free(valid_cases[i].packet);
   for (i = 0; i < MALFORMED_CASE_COUNT; i++)
      free(malformed_cases[i].packet);
   free(valid_cases);
   free(malformed_cases);
   dlclose(ref.handle);
   fprintf(stdout, "summary: %s/summary.md\n", out_dir);
   return results.failures == 0 ? 0 : 1;
}
