#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "SigProc_FIX.h"
#include "resampler_private.h"
#include "hq_resampler.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const int RATES[5] = {8000, 12000, 16000, 24000, 48000};

/* Minimal iterative radix-2 FFT, in-place, n must be a power of two. */
static void fft(double *re, double *im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * M_PI / len;
        double wr = cos(ang), wi = sin(ang);
        for (int i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (int k = 0; k < len / 2; k++) {
                double ur = re[i + k], ui = im[i + k];
                double vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                double vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr; im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
                double ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

static int next_pow2_le(double x) {
    int n = 1;
    while ((n << 1) <= (int)x) n <<= 1;
    return n;
}

typedef struct {
    double thdn_db;       /* 10 log10((E_total - E_fund) / E_total) */
    double thd_db;        /* harmonics 2..N only, relative to fund */
    double fund_dbfs;
    double max_slew;
    double ideal_slew;
    int    fund_bin;
    double peak_hz[8];
    double peak_dbc[8];
} analysis_t;

static void analyze(const opus_int16 *out, long out_len, int fs_out,
                    double f0, int f0_bin, int nfft, analysis_t *a) {
    long start = (long)(0.3 * fs_out);
    double *re = calloc(nfft, sizeof(double));
    double *im = calloc(nfft, sizeof(double));
    double e_tot = 0.0;
    for (int i = 0; i < nfft; i++) {
        re[i] = out[start + i];
        e_tot += re[i] * re[i];
    }
    fft(re, im, nfft);

    double e_fund = (re[f0_bin] * re[f0_bin] + im[f0_bin] * im[f0_bin]) * 2.0 / nfft;
    double e_harm = 0.0;
    for (int h = 2; h * f0_bin < nfft / 2; h++) {
        int b = h * f0_bin;
        e_harm += (re[b] * re[b] + im[b] * im[b]) * 2.0 / nfft;
    }
    a->thdn_db = 10.0 * log10((e_tot - e_fund) / e_tot);
    a->thd_db = 10.0 * log10(e_harm / e_fund);
    a->fund_dbfs = 10.0 * log10(e_fund / (nfft * (32768.0 * 32768.0 / 2.0)));
    a->fund_bin = f0_bin;

    /* Top bins excluding the fundamental neighbourhood (±2 bins). */
    int used[8] = {0};
    for (int p = 0; p < 8; p++) {
        double best = -1.0;
        int best_b = -1;
        for (int b = 1; b < nfft / 2; b++) {
            if (abs(b - f0_bin) <= 2) continue;
            int skip = 0;
            for (int q = 0; q < p; q++) if (used[q] == b) skip = 1;
            if (skip) continue;
            double e = re[b] * re[b] + im[b] * im[b];
            if (e > best) { best = e; best_b = b; }
        }
        used[p] = best_b;
        a->peak_hz[p] = (double)best_b * fs_out / nfft;
        a->peak_dbc[p] = 10.0 * log10((best * 2.0 / nfft) / e_fund);
    }

    a->max_slew = 0.0;
    for (long i = 1; i < out_len; i++) {
        double d = (double)out[i] - out[i - 1];
        if (fabs(d) > a->max_slew) a->max_slew = fabs(d);
    }
    double amp = 32767.0 * pow(10.0, -1.0 / 20.0);
    /* Largest |d/dt| of the test sine in output-sample units. A genuine
     * discontinuity would exceed this by orders of magnitude; normal
     * resampled samples stay close to it regardless of output rate. */
    a->ideal_slew = amp * 2.0 * M_PI * f0 / fs_out;
    free(re); free(im);
}

static long run_stream(silk_resampler_state_struct *st, const opus_int16 *in,
                       long in_len, int fs_in, int fs_out, int chunk_ms,
                       opus_int16 *out) {
    int chunk = chunk_ms <= 0 ? in_len : chunk_ms * fs_in / 1000;
    long produced = 0;
    long cap = (long)(2.5 * in_len * fs_out / fs_in) + 4096;
    for (long pos = 0; pos < in_len; ) {
        int n = (int)((in_len - pos) < chunk ? (in_len - pos) : chunk);
        long before = produced;
        silk_resampler(st, out + produced, in + pos, n);
        /* silk_resampler() returns void on this version: produced count is
         * the rate ratio of consumed samples (both rates are integer kHz). */
        produced += (long)n * fs_out / fs_in;
        (void)before;
        pos += n;
        if (produced > cap) { fprintf(stderr, "output buffer overrun\n"); exit(2); }
    }
    return produced;
}

static long run_hq(hq_resampler_state *st, const opus_int16 *in, long in_len,
                   int fs_in, int fs_out, int chunk_ms, opus_int16 *out) {
    int chunk = chunk_ms <= 0 ? in_len : chunk_ms * fs_in / 1000;
    long produced = 0;
    long cap = (long)(2.5 * in_len * fs_out / fs_in) + 4096;
    for (long pos = 0; pos < in_len; ) {
        int n = (int)((in_len - pos) < chunk ? (in_len - pos) : chunk);
        produced += hq_resampler_process(st, out + produced, in + pos, n);
        pos += n;
        if (produced > cap) { fprintf(stderr, "hq output overrun\n"); exit(2); }
    }
    return produced;
}

/* Maximum absolute sample-to-sample jump at chunk seams relative to the
 * largest jump inside a whole-buffer (seam-free) run. A value of 0 means the
 * chunked stream is bit-identical, already covered by chunk_mismatch; this
 * metric additionally documents there is no transient at block edges. */
static double seam_transient(const opus_int16 *whole, long whole_len,
                             const opus_int16 *chunked, long chunk_len,
                             int chunk_out_samples) {
    long n = whole_len < chunk_len ? whole_len : chunk_len;
    double max_inner = 1.0;
    for (long i = 1; i < n; i++) {
        double d = fabs((double)whole[i] - whole[i - 1]);
        if (d > max_inner) max_inner = d;
    }
    double max_seam = 0.0;
    for (long i = chunk_out_samples; i < n; i += chunk_out_samples) {
        if (i < n) {
            double d = fabs((double)chunked[i] - chunked[i - 1]);
            if (d > max_seam) max_seam = d;
        }
    }
    return max_seam / max_inner;
}

/* Replicate the fixed-point fractional index math of both FIR stages over
 * every possible Q16 fraction to prove the coefficient table bounds. */
static void boundary_audit(FILE *fp) {
    struct { int fracs; int half_order; const char *name; int table_cap; } cases[] = {
        {3, RESAMPLER_DOWN_ORDER_FIR0 / 2, "3/4 down polyphase", 3 * (RESAMPLER_DOWN_ORDER_FIR0 / 2)},
        {2, RESAMPLER_DOWN_ORDER_FIR0 / 2, "2/3 down polyphase", 2 * (RESAMPLER_DOWN_ORDER_FIR0 / 2)},
        {12, RESAMPLER_ORDER_FIR_12 / 2, "IIR/FIR 12-frac", 12 * (RESAMPLER_ORDER_FIR_12 / 2)},
    };
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        int max_idx = 0, max_mirror = 0, oob = 0;
        for (unsigned frac = 0; frac < 65536u; frac++) {
            int ind = silk_SMULWB((opus_int32)frac, cases[c].fracs);
            int mirror = cases[c].fracs - 1 - ind;
            int idx = cases[c].half_order * ind;
            int midx = cases[c].half_order * mirror;
            if (idx > max_idx) max_idx = idx;
            if (midx > max_mirror) max_mirror = midx;
            if (ind < 0 || mirror < 0 ||
                idx + cases[c].half_order > cases[c].table_cap ||
                midx + cases[c].half_order > cases[c].table_cap) oob++;
        }
        fprintf(fp, "%s: max_forward_offset=%d max_mirror_offset=%d table_capacity=%d out_of_bounds=%d\n",
                cases[c].name, max_idx, max_mirror, cases[c].table_cap, oob);
    }
}

int main(void) {
    FILE *csv = fopen("resampler_audit/results_sweep.csv", "w");
    FILE *pk = fopen("resampler_audit/spectrum_peaks.txt", "w");
    FILE *hqcsv = fopen("resampler_audit/results_hq.csv", "w");
    if (!csv || !pk || !hqcsv) { perror("open"); return 1; }
    fprintf(csv, "engine,fs_in,fs_out,method,fir_fracs,f0_hz,nfft,thdn_db,thd_db,fund_dbfs,max_slew,ideal_slew,slew_ratio,chunk_mismatch,seam_transient,len_whole,len_1ms,pass_-90dB\n");
    fprintf(hqcsv, "engine,fs_in,fs_out,method,fir_fracs,f0_hz,nfft,thdn_db,thd_db,fund_dbfs,max_slew,ideal_slew,slew_ratio,chunk_mismatch,seam_transient,len_whole,len_1ms,pass_-90dB\n");

    boundary_audit(pk);

    const int chunks[4] = {0, 40, 10, 1}; /* 0 = whole buffer */
    double duration = 2.0;
    double amp = 32767.0 * pow(10.0, -1.0 / 20.0); /* -1 dBFS */

    for (int ai = 0; ai < 5; ai++) {
        for (int bi = 0; bi < 5; bi++) {
            if (ai == bi) continue;
            int fs_in = RATES[ai], fs_out = RATES[bi];
            int for_enc;
            {
                silk_resampler_state_struct probe;
                int ok_enc = silk_resampler_init(&probe, fs_in, fs_out, 1) == 0;
                int ok_dec = silk_resampler_init(&probe, fs_in, fs_out, 0) == 0;
                if (!ok_enc && !ok_dec) continue; /* pair not supported by the rate matrix */
                for_enc = ok_enc ? 1 : 0;
            }

            int nfft = next_pow2_le(0.85 * fs_out);
            int f0_bin = (int)lround(1000.0 * nfft / fs_out);
            double f0 = (double)f0_bin * fs_out / nfft;

            long in_len = (long)(duration * fs_in);
            opus_int16 *in = malloc(in_len * sizeof(opus_int16));
            for (long i = 0; i < in_len; i++)
                in[i] = (opus_int16)lround(amp * sin(2.0 * M_PI * f0 * i / fs_in));

            long cap = (long)(2.5 * in_len * fs_out / fs_in) + 4096;
            opus_int16 *outs[4];
            long lens[4];
            for (int m = 0; m < 4; m++) outs[m] = malloc(cap * sizeof(opus_int16));

            silk_resampler_state_struct st;
            int ret = silk_resampler_init(&st, fs_in, fs_out, for_enc);
            if (ret) { fprintf(stderr, "init failed %d->%d\n", fs_in, fs_out); return 2; }
            const char *method =
                st.resampler_function == 0 ? "copy" :
                st.resampler_function == 1 ? "up2_HQ" :
                st.resampler_function == 2 ? "IIR_FIR" : "down_FIR";

            for (int m = 0; m < 4; m++) {
                silk_resampler_state_struct s2;
                silk_resampler_init(&s2, fs_in, fs_out, for_enc);
                lens[m] = run_stream(&s2, in, in_len, fs_in, fs_out, chunks[m], outs[m]);
            }

            int mismatch = 0;
            for (int m = 1; m < 4; m++) {
                if (lens[m] != lens[0]) mismatch++;
                long common = lens[m] < lens[0] ? lens[m] : lens[0];
                if (memcmp(outs[m], outs[0], common * sizeof(opus_int16))) mismatch++;
            }

            analysis_t a;
            analyze(outs[0], lens[0], fs_out, f0, f0_bin, nfft, &a);
            double slew_ratio = a.max_slew / a.ideal_slew;
            int pass = (a.thdn_db < -90.0 && mismatch == 0 && slew_ratio < 5.0);
            double silk_seam = seam_transient(outs[0], lens[0], outs[3], lens[3], fs_out);

            fprintf(csv, "SILK,%d,%d,%s,%d,%.2f,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.3f,%d,%.3f,%ld,%ld,%s\n",
                    fs_in, fs_out, method, st.FIR_Fracs, f0, nfft,
                    a.thdn_db, a.thd_db, a.fund_dbfs, a.max_slew, a.ideal_slew,
                    slew_ratio, mismatch, silk_seam, lens[0], lens[3], pass ? "PASS" : "FAIL");

            /* ---- HQ arbitrary-ratio engine, same stimuli ---- */
            opus_int16 *hq[4];
            long hq_lens[4];
            for (int m = 0; m < 4; m++) hq[m] = malloc(cap * sizeof(opus_int16));
            hq_resampler_state hst;
            if (hq_resampler_init(&hst, fs_in, fs_out)) { fprintf(stderr, "hq init failed\n"); return 2; }
            for (int m = 0; m < 4; m++) {
                hq_resampler_state h2;
                hq_resampler_init(&h2, fs_in, fs_out);
                hq_lens[m] = run_hq(&h2, in, in_len, fs_in, fs_out, chunks[m], hq[m]);
            }
            int hq_mismatch = 0;
            for (int m = 1; m < 4; m++) {
                if (hq_lens[m] != hq_lens[0]) hq_mismatch++;
                long common = hq_lens[m] < hq_lens[0] ? hq_lens[m] : hq_lens[0];
                if (memcmp(hq[m], hq[0], common * sizeof(opus_int16))) hq_mismatch++;
            }
            analysis_t ha;
            analyze(hq[0], hq_lens[0], fs_out, f0, f0_bin, nfft, &ha);
            double hq_slew = ha.max_slew / ha.ideal_slew;
            int hq_pass = (ha.thdn_db < -90.0 && hq_mismatch == 0 && hq_slew < 5.0);
            double hq_seam = seam_transient(hq[0], hq_lens[0], hq[3], hq_lens[3], fs_out);
            fprintf(hqcsv, "HQ,%d,%d,polyphase_%dph_%dtap,0,%.2f,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.3f,%d,%.3f,%ld,%ld,%s\n",
                    fs_in, fs_out, HQ_RESAMPLER_PHASES, HQ_RESAMPLER_TAPS, f0, nfft,
                    ha.thdn_db, ha.thd_db, ha.fund_dbfs, ha.max_slew, ha.ideal_slew,
                    hq_slew, hq_mismatch, hq_seam, hq_lens[0], hq_lens[3],
                    hq_pass ? "PASS" : "FAIL");
            fprintf(pk, "HQ %d -> %d: THD+N=%.2f dB THD=%.2f dBc slew=%.3f mismatch=%d top:",
                    fs_in, fs_out, ha.thdn_db, ha.thd_db, hq_slew, hq_mismatch);
            for (int p = 0; p < 4; p++)
                fprintf(pk, " %.0f/%.1f;", ha.peak_hz[p], ha.peak_dbc[p]);
            fprintf(pk, "\n");
            for (int m = 0; m < 4; m++) free(hq[m]);

            fprintf(pk, "\n%d -> %d Hz (%s, f0=%.1f Hz, THD+N=%.2f dB, THD=%.2f dBc, slew_ratio=%.3f, chunk_mismatch=%d)\n",
                    fs_in, fs_out, method, f0, a.thdn_db, a.thd_db, slew_ratio, mismatch);
            fprintf(pk, "  top non-fundamental bins (Hz / dBc):");
            for (int p = 0; p < 8; p++)
                fprintf(pk, " %.0f/%.1f;", a.peak_hz[p], a.peak_dbc[p]);
            fprintf(pk, "\n");

            for (int m = 0; m < 4; m++) free(outs[m]);
            free(in);
        }
    }
    fclose(csv);
    fclose(pk);
    fclose(hqcsv);
    printf("done\n");
    return 0;
}
