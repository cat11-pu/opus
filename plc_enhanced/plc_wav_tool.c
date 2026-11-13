#include "plc_engine.h"
#include "wav_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Usage: plc_wav_tool <in.wav> <loss_rate 0..1> <random|bursty> <repeat|enhanced> <out.wav> */
int main(int argc, char **argv)
{
    int n, frames, f;
    short *pcm, *out;
    int *mask;
    float rate;
    unsigned int rng = 0xABCDEF01u;
    PlcEngine *plc;
    plc_mode_t mode;

    if (argc != 6) {
        fprintf(stderr,
                "usage: %s in.wav rate random|bursty repeat|enhanced out.wav\n",
                argv[0]);
        return 1;
    }
    pcm = read_wav(argv[1], &n);
    if (!pcm) { perror("read"); return 1; }
    rate = (float) atof(argv[2]);
    frames = n / PLC_FRAME_SAMPLES;
    mask = (int *) calloc((size_t) frames, sizeof(int));
    if (!strcmp(argv[3], "bursty")) {
        float p_enter = rate * 0.45f, p_leave = (1.f - rate) * 0.45f;
        int lost = 0;
        for (f = 0; f < frames; f++) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            float u = (rng & 0xFFFFFF) / (float) 0x1000000;
            lost = lost ? (u >= p_leave) : (u < p_enter);
            mask[f] = lost;
        }
    } else {
        for (f = 0; f < frames; f++) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            mask[f] = ((rng & 0xFFFFFF) / (float) 0x1000000) < rate;
        }
    }
    mode = !strcmp(argv[4], "enhanced") ? PLC_MODE_ENHANCED : PLC_MODE_REPEAT;
    plc = plc_create(mode);
    out = (short *) malloc((size_t) frames * PLC_FRAME_SAMPLES * sizeof(short));
    for (f = 0; f < frames; f++) {
        if (mask[f]) plc_conceal(plc, out + (size_t) f * PLC_FRAME_SAMPLES);
        else {
            plc_accept_frame(plc, pcm + (size_t) f * PLC_FRAME_SAMPLES);
            plc_last_output(plc, out + (size_t) f * PLC_FRAME_SAMPLES);
        }
    }
    write_wav(argv[5], out, frames * PLC_FRAME_SAMPLES);
    printf("wrote %s (%d frames, %d lost)\n", argv[5], frames,
           (int) (frames * rate));
    free(pcm); free(out); free(mask);
    plc_destroy(plc);
    return 0;
}
