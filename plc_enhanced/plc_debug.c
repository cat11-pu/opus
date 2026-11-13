#include "plc_engine.h"
#include "wav_io.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define N PLC_FRAME_SAMPLES

static unsigned int rng_state = 0xC0FFEEu;
static float urand(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return (rng_state & 0xFFFFFF) / (float) 0x1000000;
}

int main(int argc, char **argv)
{
    /* Replay the same pure_voiced random mask as plc_test and locate jumps. */
    int frames = 1000, total = frames * N, f, i;
    float rate = argc > 1 ? (float) atof(argv[1]) : 0.2f;
    int *mask = calloc(frames, sizeof(int));
    short *ref = malloc(total * sizeof(short));
    short *out = malloc(total * sizeof(short));
    PlcEngine *plc = plc_create(PLC_MODE_ENHANCED);

    /* Match gen_signal(SIG_PURE_VOICED). */
    for (i = 0; i < total; i++) {
        float t = (float) i / PLC_SAMPLE_RATE;
        float fr = 120.f + 60.f * (t / 10.f);
        ref[i] = (short) lroundf(0.5f * 32767.f *
                                 0.7f * sinf(2.f * 3.14159265f * fr * t));
    }
    for (f = 0; f < frames; f++) mask[f] = urand() < rate;

    for (f = 0; f < frames; f++) {
        if (mask[f]) {
            plc_conceal(plc, out + (size_t) f * N);
        } else {
            plc_accept_frame(plc, ref + (size_t) f * N);
            plc_last_output(plc, out + (size_t) f * N);
        }
    }
    write_wav("debug_pure_voiced_enhanced.wav", out, total);

    for (i = 1; i < total; i++) {
        float jump = fabsf((float) out[i] - out[i - 1]) / 32768.f;
        float rj = fabsf((float) ref[i] - ref[i - 1]) / 32768.f;
        if (jump > 0.25f && jump > 2.f * (rj + 0.02f)) {
            int fr_idx = i / N, pos = i % N;
            const char *where =
                pos == 0 ? (mask[fr_idx] ? "LOSS-ENTRY" : "RECOVERY")
                         : "WITHIN";
            printf("jump=%.3f refjump=%.3f frame=%d pos=%d %s mask[cur]=%d "
                   "mask[prev]=%d\n",
                   jump, rj, fr_idx, pos, where,
                   mask[fr_idx], fr_idx > 0 ? mask[fr_idx - 1] : 0);
        }
    }
    free(mask); free(ref); free(out);
    plc_destroy(plc);
    return 0;
}
