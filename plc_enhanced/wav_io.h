#ifndef WAV_IO_H
#define WAV_IO_H

#include <stddef.h>

/* Minimal 16-bit mono PCM WAV I/O, no external dependencies. */
int write_wav(const char *path, const short *pcm, int samples);
short *read_wav(const char *path, int *samples);

#endif
