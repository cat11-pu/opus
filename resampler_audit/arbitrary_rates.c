/* Spot-check the HQ engine on rates outside the SILK matrix, including
 * non-integer-kHz and 96 kHz pairs. Tone frequency must sit inside the
 * output Nyquist; 300 Hz is used so the low-rate pairs stay in-band. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "hq_resampler.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double measure(int fs_in, int fs_out, double f0) {
    hq_resampler_state st;
    if (hq_resampler_init(&st, fs_in, fs_out)) return 999.0;
    int n = fs_in * 4;
    opus_int16 *in = malloc(n * sizeof(opus_int16));
    for (int i = 0; i < n; i++)
        in[i] = (opus_int16)lround(20000.0 * sin(2.0 * M_PI * f0 * i / fs_in));
    long cap = (long)n * fs_out / fs_in + 256;
    opus_int16 *out = malloc(cap * sizeof(opus_int16));
    long produced = 0;
    for (long pos = 0; pos < n; ) {
        int chunk = n - pos < fs_in ? (int)(n - pos) : fs_in;
        produced += hq_resampler_process(&st, out + produced, in + pos, chunk);
        pos += chunk;
    }
    double sig = 0.0, err = 0.0;
    for (long m = fs_out; m < produced - fs_out; m++) {
        double ideal = 20000.0 * sin(2.0 * M_PI * f0 * m / fs_out);
        sig += ideal * ideal;
        err += ((double)out[m] - ideal) * ((double)out[m] - ideal);
    }
    free(in); free(out);
    return sig > 0.0 ? 10.0 * log10(err / sig) : 998.0;
}

int main(void) {
    const int pairs[][2] = {
        {44100, 48000}, {48000, 44100}, {22050, 48000}, {11025, 16000},
        {32000, 96000}, {96000, 24000}, {7000, 13000}
    };
    for (unsigned i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        double r = measure(pairs[i][0], pairs[i][1], 300.0);
        printf("%5d -> %5d : THD+N = %7.2f dB %s\n",
               pairs[i][0], pairs[i][1], r, r < -90.0 ? "PASS" : "FAIL");
    }
    return 0;
}
