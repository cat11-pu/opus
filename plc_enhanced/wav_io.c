#include "wav_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAV_SAMPLE_RATE 16000

static void put_le16(FILE *f, unsigned v)
{
    fputc(v & 0xff, f);
    fputc((v >> 8) & 0xff, f);
}

static void put_le32(FILE *f, unsigned long v)
{
    fputc(v & 0xff, f);
    fputc((v >> 8) & 0xff, f);
    fputc((v >> 16) & 0xff, f);
    fputc((v >> 24) & 0xff, f);
}

static unsigned long get_le32(FILE *f)
{
    unsigned long v = 0;
    int i;
    for (i = 0; i < 4; i++) v |= (unsigned long) (fgetc(f) & 0xff) << (8 * i);
    return v;
}

int write_wav(const char *path, const short *pcm, int samples)
{
    FILE *f = fopen(path, "wb");
    unsigned long data_bytes = (unsigned long) samples * 2;
    if (!f) return -1;
    fwrite("RIFF", 1, 4, f);
    put_le32(f, 36 + data_bytes);
    fwrite("WAVEfmt ", 1, 8, f);
    put_le32(f, 16);
    put_le16(f, 1);                 /* PCM */
    put_le16(f, 1);                 /* mono */
    put_le32(f, WAV_SAMPLE_RATE);
    put_le32(f, WAV_SAMPLE_RATE * 2);
    put_le16(f, 2);
    put_le16(f, 16);
    fwrite("data", 1, 4, f);
    put_le32(f, data_bytes);
    fwrite(pcm, 2, (size_t) samples, f);
    fclose(f);
    return 0;
}

short *read_wav(const char *path, int *samples)
{
    FILE *f = fopen(path, "rb");
    char tag[4];
    short *pcm;
    int i, n;
    if (!f) return NULL;
    if (fread(tag, 1, 4, f) != 4 || memcmp(tag, "RIFF", 4) != 0) { fclose(f); return NULL; }
    get_le32(f);
    if (fread(tag, 1, 4, f) != 4 || memcmp(tag, "WAVE", 4) != 0) { fclose(f); return NULL; }
    for (;;) {
        if (fread(tag, 1, 4, f) != 4) { fclose(f); return NULL; }
        if (memcmp(tag, "data", 4) == 0) break;
        fseek(f, (long) get_le32(f), SEEK_CUR);
    }
    n = (int) (get_le32(f) / 2);
    pcm = (short *) malloc((size_t) n * sizeof(short));
    if (!pcm) { fclose(f); return NULL; }
    if (fread(pcm, 2, (size_t) n, f) != (size_t) n) {
        free(pcm); fclose(f); return NULL;
    }
    for (i = 0; i < n; i++) {
        unsigned lo = (unsigned) (pcm[i] & 0xff);
        unsigned hi = (unsigned) (pcm[i] >> 8);
        pcm[i] = (short) (lo | (hi << 8));
    }
    *samples = n;
    fclose(f);
    return pcm;
}
