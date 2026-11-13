#include "plc_engine.h"
#include "wav_io.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SR PLC_SAMPLE_RATE
#define N  PLC_FRAME_SAMPLES
#define DURATION_SEC 10
#define TOTAL_FRAMES (DURATION_SEC * (SR / N))

/* A pop/click is a large inter-sample jump that does not exist in the clean
   reference: we require the processed jump to exceed both an absolute
   threshold and 2x the reference jump at the same position. */
#define CLICK_ABS_THRESHOLD 0.25f
#define CLICK_RATIO 2.0f

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *) a, y = *(const float *) b;
    return (x > y) - (x < y);
}

typedef enum { SIG_SPEECHLIKE = 0, SIG_PURE_VOICED, SIG_NOISE, SIG_SIG_COUNT } sig_t;
typedef enum { LOSS_RANDOM = 0, LOSS_BURSTY } loss_t;

static unsigned int rng_state = 0xC0FFEEu;

static float urand(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return (rng_state & 0xFFFFFF) / (float) 0x1000000;
}

static float grand(void)
{
    float u = urand(), v = urand();
    if (u < 1e-6f) u = 1e-6f;
    return sqrtf(-2.f * logf(u)) * cosf(2.f * (float) M_PI * v);
}

static short f2s(float v)
{
    if (v > 32767.f) v = 32767.f;
    if (v < -32768.f) v = -32768.f;
    return (short) lroundf(v);
}

/* Speech-like source: time-varying harmonic train + amplitude/AM + noise floor. */
static void gen_signal(sig_t sig, short *buf, int total_samples)
{
    float lp = 0.f;
    int i;
    for (i = 0; i < total_samples; i++) {
        float t = (float) i / SR;
        float f0, x = 0.f, amp, env;
        switch (sig) {
        case SIG_PURE_VOICED: {
            /* Gliding voiced tone, 120 -> 180 Hz. */
            float f = 120.f + 60.f * (t / DURATION_SEC);
            x = 0.7f * sinf(2.f * (float) M_PI * f * t);
            break;
        }
        case SIG_NOISE: {
            float w = grand();
            lp = 0.4f * w + 0.6f * lp;
            x = 0.6f * lp;
            break;
        }
        default: {
            /* Pitch contour + harmonics + syllabic envelope + fricative noise. */
            f0 = 110.f + 40.f * sinf(2.f * (float) M_PI * 0.3f * t);
            env = 0.55f + 0.45f * sinf(2.f * (float) M_PI * 2.0f * t);
            if (env < 0.15f) env = 0.15f;
            x = env * (0.6f * sinf(2.f * (float) M_PI * f0 * t)
                       + 0.3f * sinf(2.f * (float) M_PI * 2 * f0 * t)
                       + 0.15f * sinf(2.f * (float) M_PI * 3 * f0 * t));
            x += 0.05f * grand();
            break;
        }
        }
        amp = 0.5f * 32767.f;
        buf[i] = f2s(amp * x);
    }
}

/* Deterministic, reproducible loss masks. Bursty uses a 2-state Gilbert
   model so runs of consecutive loss occur. */
static int *make_mask(loss_t kind, float rate, int frames)
{
    int *mask = (int *) calloc((size_t) frames, sizeof(int));
    int f;
    if (kind == LOSS_RANDOM) {
        for (f = 0; f < frames; f++) mask[f] = urand() < rate;
    } else {
        /* p_enter / p_leave tuned to hit ~rate overall. */
        float p_enter = rate * 0.45f;
        float p_leave = (1.f - rate) * 0.45f;
        int lost = 0;
        for (f = 0; f < frames; f++) {
            if (lost) lost = urand() >= p_leave;
            else      lost = urand() < p_enter;
            mask[f] = lost;
        }
    }
    return mask;
}

typedef struct {
    double seg_snr_db;       /* segmental SNR over impacted frames only */
    double overall_snr_db;
    double env_log_err;      /* mean |log RMS ratio| on impacted frames (dB) */
    double rms_corr;         /* frame-RMS correlation, whole clip */
    double rms_bias;         /* mean relative RMS error (robust for constant) */
    long   clicks;           /* inter-sample jumps over threshold */
    long   clips;            /* saturated samples */
    long   boundary_breaks;  /* discontinuities at concealment boundaries */
    float  max_jump;         /* largest normalized inter-sample jump */
} Metrics;

static void run_mode(plc_mode_t mode, const short *ref, const int *mask,
                     short *out, int frames, Metrics *m, const char *wavpath)
{
    PlcEngine *plc = plc_create(mode);
    double err_power = 0., ref_power = 0.;
    double seg_snr_acc = 0.;
    int seg_snr_n = 0;
    double env_acc = 0.;
    int env_n = 0;
    double *rms_ref = (double *) calloc((size_t) frames, sizeof(double));
    double *rms_out = (double *) calloc((size_t) frames, sizeof(double));
    long clicks = 0, clips = 0, breaks = 0;
    float max_jump = 0.f;
    int f, i;

    for (f = 0; f < frames; f++) {
        const short *rf = ref + (size_t) f * N;
        short *of = out + (size_t) f * N;
        if (mask[f]) {
            plc_conceal(plc, of);
        } else {
            plc_accept_frame(plc, rf);
            plc_last_output(plc, of);
        }
    }

    for (f = 0; f < frames; f++) {
        double fe = 0., fr = 0.;
        int impacted = mask[f] || (f > 0 && mask[f - 1]);
        for (i = 0; i < N; i++) {
            double a = out[(size_t) f * N + i];
            double b = ref[(size_t) f * N + i];
            double e = a - b;
            err_power += e * e;
            ref_power += b * b;
            fe += e * e;
            fr += b * b + 1.;
            rms_ref[f] += b * b;
            rms_out[f] += a * a;
        }
        rms_ref[f] = sqrt(rms_ref[f] / N);
        rms_out[f] = sqrt(rms_out[f] / N);
        if (impacted) {
            seg_snr_acc += 10. * log10((fr + 1.) / (fe + 1.));
            seg_snr_n++;
            if (rms_ref[f] > 50. && rms_out[f] > 50.) {
                env_acc += fabs(20. * log10(rms_out[f] / rms_ref[f]));
                env_n++;
            }
        }
    }

    /* Establish the reference signal's own jump distribution so the click
       threshold adapts to material with naturally large steps (noise) versus
       small steps (voiced). */
    {
        int cap = frames * N;
        float *rj = (float *) malloc((size_t) cap * sizeof(float));
        int rn = 0;
        for (i = 1; i < cap; i++)
            rj[rn++] = fabsf((float) ref[i] - ref[i - 1]) / 32768.f;
        /* Full sort (ascending) to obtain stable high-percentile steps. */
        qsort(rj, (size_t) rn, sizeof(float), cmp_float);
        float ref_p99 = rn > 0 ? rj[(rn * 99) / 100] : 0.f;
        float ref_p999 = rn > 10 ? rj[(rn * 999) / 1000] : ref_p99;
        /* For material with naturally large steps (noise), only flag jumps
           beyond its 99.9th-percentile step; for smooth material keep the
           conservative absolute threshold. */
        float adaptive = ref_p99 > CLICK_ABS_THRESHOLD
                         ? ref_p999 : CLICK_ABS_THRESHOLD;
        free(rj);
        for (i = 1; i < frames * N; i++) {
            float jump = fabsf((float) out[i] - out[i - 1]) / 32768.f;
            float refjump = fabsf((float) ref[i] - ref[i - 1]) / 32768.f;
            if (jump > max_jump) max_jump = jump;
            if (jump > adaptive && jump > CLICK_RATIO * (refjump + 0.02f))
                clicks++;
            if (out[i] >= 32767 || out[i] <= -32768) clips++;
        }
        /* Boundary discontinuities use the same adaptive threshold. */
        for (f = 1; f < frames; f++) {
            if (mask[f] != mask[f - 1]) {
                float j = fabsf((float) out[(size_t) f * N] -
                                out[(size_t) f * N - 1]) / 32768.f;
                float rj2 = fabsf((float) ref[(size_t) f * N] -
                                  ref[(size_t) f * N - 1]) / 32768.f;
                if (j > adaptive && j > CLICK_RATIO * (rj2 + 0.02f))
                    breaks++;
            }
        }
    }
    {
        double mr = 0., mo = 0., cov = 0., vr = 0., vo = 0.;
        double bias_acc = 0.;
        int k;
        for (k = 0; k < frames; k++) { mr += rms_ref[k]; mo += rms_out[k]; }
        mr /= frames; mo /= frames;
        for (k = 0; k < frames; k++) {
            cov += (rms_ref[k] - mr) * (rms_out[k] - mo);
            vr += (rms_ref[k] - mr) * (rms_ref[k] - mr);
            vo += (rms_out[k] - mo) * (rms_out[k] - mo);
            if (rms_ref[k] > 50.)
                bias_acc += fabs(rms_out[k] - rms_ref[k]) / rms_ref[k];
        }
        m->rms_corr = (vr > 1. && vo > 1.) ? cov / sqrt(vr * vo) : 0.;
        m->rms_bias = bias_acc / frames;
        free(rms_ref);
        free(rms_out);
    }
    m->env_log_err = env_n ? env_acc / env_n : 0.;
    m->seg_snr_db = seg_snr_n ? seg_snr_acc / seg_snr_n : 0.;
    m->overall_snr_db = 10. * log10((ref_power + 1.) / (err_power + 1.));
    m->clicks = clicks;
    m->clips = clips;
    m->boundary_breaks = breaks;
    m->max_jump = max_jump;

    if (wavpath) write_wav(wavpath, out, frames * N);
    plc_destroy(plc);
}

int main(int argc, char **argv)
{
    const float rate = argc > 1 ? (float) atof(argv[1]) : 0.20f;
    if (argc > 2) rng_state = (unsigned int) strtoul(argv[2], NULL, 0);
    const int total = TOTAL_FRAMES * N;
    short *ref = (short *) malloc((size_t) total * sizeof(short));
    short *out_rep = (short *) malloc((size_t) total * sizeof(short));
    short *out_enh = (short *) malloc((size_t) total * sizeof(short));
    const char *sig_names[SIG_SIG_COUNT] = { "speechlike", "pure_voiced", "noise" };
    const char *loss_names[2] = { "random", "bursty" };
    sig_t s;
    loss_t k;
    int loss_count_total = 0;

    printf("PLC comparison  rate=%.0f%%  %d s @ %d Hz, frame %d samples\n",
           rate * 100., DURATION_SEC, SR, N);
    printf("%-12s %-8s %-8s | %-22s | %-18s | %-24s\n",
           "signal", "loss", "mode",
           "segSNR  ovSNR  envDB", "rmsCorr rmsBias",
           "clicks clips bdry maxJump");
    printf("--------------------------------------------------------------------------------------------------\n");

    for (s = 0; s < SIG_SIG_COUNT; s++) {
        gen_signal(s, ref, total);
        {
            char p0[128];
            snprintf(p0, sizeof p0, "ref_%s.wav", sig_names[s]);
            write_wav(p0, ref, total);
        }
        for (k = 0; k < 2; k++) {
            int *mask = make_mask(k, rate, TOTAL_FRAMES);
            Metrics mr, me;
            int f, lost = 0;
            for (f = 0; f < TOTAL_FRAMES; f++) lost += mask[f];
            loss_count_total = lost;
            {
                char p1[128], p2[128];
                snprintf(p1, sizeof p1, "out_%s_%s_repeat.wav",
                         sig_names[s], loss_names[k]);
                snprintf(p2, sizeof p2, "out_%s_%s_enhanced.wav",
                         sig_names[s], loss_names[k]);
                run_mode(PLC_MODE_REPEAT, ref, mask, out_rep, TOTAL_FRAMES, &mr, p1);
                run_mode(PLC_MODE_ENHANCED, ref, mask, out_enh, TOTAL_FRAMES, &me, p2);
            }
            printf("%-12s %-8s %-8s | %6.2f %6.2f %6.2f | %7.3f %6.3f | %6ld %5ld %4ld %7.3f\n",
                   sig_names[s], loss_names[k], "repeat",
                   mr.seg_snr_db, mr.overall_snr_db, mr.env_log_err,
                   mr.rms_corr, mr.rms_bias,
                   mr.clicks, mr.clips, mr.boundary_breaks, mr.max_jump);
            printf("%-12s %-8s %-8s | %6.2f %6.2f %6.2f | %7.3f %6.3f | %6ld %5ld %4ld %7.3f\n",
                   sig_names[s], loss_names[k], "enhanced",
                   me.seg_snr_db, me.overall_snr_db, me.env_log_err,
                   me.rms_corr, me.rms_bias,
                   me.clicks, me.clips, me.boundary_breaks, me.max_jump);
            free(mask);
        }
    }
    printf("--------------------------------------------------------------------------------------------------\n");
    printf("lost frames per mask: %d / %d (%.1f%%)\n",
           loss_count_total, TOTAL_FRAMES,
           100. * loss_count_total / TOTAL_FRAMES);
    free(ref); free(out_rep); free(out_enh);
    return 0;
}
