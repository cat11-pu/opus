#include "plc_engine.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PITCH_MIN 40                 /* 400 Hz */
#define PITCH_MAX 320                /* 50 Hz   */
#define VOICED_THRESHOLD 0.25f
#define ENERGY_DECAY 0.97f           /* per lost frame */
#define MAX_SCALE 2.0f
#define MIN_SCALE 0.5f
#define FADE_SAMPLES 40              /* unvoiced crossfade (2.5 ms) */

/* Worst-case concealed length before the state is recycled. */
#define SYNTH_CAP (PLC_HISTORY_SAMPLES + PLC_MAX_LOSS_RUN * PLC_FRAME_SAMPLES)

struct PlcEngine {
    plc_mode_t mode;
    float hist[PLC_HISTORY_SAMPLES];
    int   hist_count;
    short last_out[PLC_FRAME_SAMPLES];
    float prev_final;
    int   have_prev_final;
    float rms_ema;
    int   prev_lost;
    int   loss_count;

    /* Enhanced PLC synthesis state. A single continuous float stream is
       maintained across every concealed frame so there is never a frame
       boundary seam; read_pos walks it one frame at a time. */
    int   pitch;
    float pitch_gain;
    float synth[SYNTH_CAP];
    int   synth_len;
    int   read_pos;
    float noise_acc;
    unsigned int rng;
    PlcInfo info;
};

static unsigned int rng_next(unsigned int *s)
{
    unsigned int x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static float rng_gaussian(unsigned int *s)
{
    float u = (rng_next(s) & 0xFFFFFF) / (float) 0x1000000;
    float v = (rng_next(s) & 0xFFFFFF) / (float) 0x1000000;
    if (u < 1e-6f) u = 1e-6f;
    return sqrtf(-2.0f * logf(u)) * cosf(2.0f * (float) M_PI * v);
}

PlcEngine *plc_create(plc_mode_t mode)
{
    PlcEngine *st = (PlcEngine *) calloc(1, sizeof(*st));
    if (!st) return NULL;
    st->mode = mode;
    st->rng = 0x12345678u;
    return st;
}

void plc_destroy(PlcEngine *st)
{
    free(st);
}

void plc_reset(PlcEngine *st)
{
    plc_mode_t mode = st->mode;
    unsigned int rng = st->rng;
    memset(st, 0, sizeof(*st));
    st->mode = mode;
    st->rng = rng;
}

static float frame_rms(const float *x, int n)
{
    float acc = 0.f;
    int i;
    for (i = 0; i < n; i++) acc += x[i] * x[i];
    return sqrtf(acc / n);
}

static short f2s(float v)
{
    if (v > 32767.f) v = 32767.f;
    if (v < -32768.f) v = -32768.f;
    return (short) lroundf(v);
}

/* Normalized autocorrelation pitch search over the most recent window. */
static void estimate_pitch(PlcEngine *st)
{
    /* A shorter window tracks gliding pitch better: a long window smears the
       autocorrelation as the fundamental moves across it. Use 1.5 periods at
       the longest expected pitch, bounded by available history. */
    int win_cap = (3 * PITCH_MAX) / 2;
    int win = st->hist_count < win_cap ? st->hist_count : win_cap;
    const float *x = st->hist + (st->hist_count - win);
    int lag, best_lag = 0, i;
    float best_gain = 0.f, second_gain = 0.f, e0 = 0.f;
    for (i = 0; i < win; i++) e0 += x[i] * x[i];
    if (e0 < 1.f) {
        st->pitch = 0;
        st->pitch_gain = 0.f;
        return;
    }
    for (lag = PITCH_MIN; lag <= PITCH_MAX && lag < win; lag++) {
        float num = 0.f, el = 0.f, gain;
        for (i = lag; i < win; i++) {
            num += x[i] * x[i - lag];
            el  += x[i - lag] * x[i - lag];
        }
        if (el < 1e-9f) continue;
        gain = num / sqrtf(e0 * el);
        if (gain > best_gain) {
            second_gain = best_gain;
            best_gain = gain;
            best_lag = lag;
        } else if (gain > second_gain) {
            second_gain = gain;
        }
    }
    st->pitch_gain = best_gain;
    /* Voiced if the peak clears the absolute threshold, OR if a sharp,
       dominant periodic peak exists (handles low-energy frames near signal
       zero crossings where absolute correlation is weak). */
    if (best_gain >= VOICED_THRESHOLD ||
        (best_gain > 1.6f * second_gain && best_gain > 0.12f))
        st->pitch = best_lag;
    else
        st->pitch = 0;
}

/* Append one pitch period by copying the previous period. Within a single
   continuous stream this is sample-continuous; the only potential seam is
   where extrapolation first leaves the real history, which the entry blend
   in plc_conceal() handles. */
static void append_period_ola(PlcEngine *st)
{
    int p = st->pitch;
    int i;
    for (i = 0; i < p; i++) {
        if (st->synth_len >= SYNTH_CAP) break;
        st->synth[st->synth_len] = st->synth[st->synth_len - p];
        st->synth_len++;
    }
}

static void append_unvoiced(PlcEngine *st, int n)
{
    /* Shaped comfort noise: gaussian through a one-pole low-pass that mimics
       the spectral tilt of fricatives. Generated at unit variance, then
       scaled to the recent signal RMS so it lands at the right loudness
       before the fine envelope adjustment runs. */
    float scale = st->rms_ema > 1.f ? st->rms_ema : 1.f;
    int i;
    for (i = 0; i < n; i++) {
        if (st->synth_len >= SYNTH_CAP) break;
        st->noise_acc = 0.35f * rng_gaussian(&st->rng)
                      + 0.65f * st->noise_acc;
        st->synth[st->synth_len++] = st->noise_acc * scale;
    }
}

static void begin_loss(PlcEngine *st)
{
    estimate_pitch(st);
    memcpy(st->synth, st->hist,
           (size_t) st->hist_count * sizeof(float));
    st->synth_len = st->hist_count;
    st->read_pos = st->hist_count;
}

static void ensure_synth(PlcEngine *st, int need)
{
    while (st->synth_len < need) {
        if (st->pitch > 0) append_period_ola(st);
        else append_unvoiced(st, PLC_FRAME_SAMPLES);
        if (st->synth_len >= SYNTH_CAP - PITCH_MAX) break;
    }
}

/* Energy envelope: scale the frame toward the decaying target RMS. The first
   frame is protected (unity gain) because its head is a real-history blend. */
static void apply_envelope(PlcEngine *st, float *frame, int n, int protect)
{
    float raw = frame_rms(frame, n);
    float target = st->rms_ema * powf(ENERGY_DECAY, (float) (st->loss_count - 1));
    float scale, peak = 0.f;
    int i;

    if (st->loss_count > PLC_MAX_LOSS_RUN) {
        float mute = 1.f - (float) (st->loss_count - PLC_MAX_LOSS_RUN) /
                     (float) PLC_MAX_LOSS_RUN;
        if (mute < 0.f) mute = 0.f;
        target *= mute;
    }
    if (raw < 1e-6f || target < 1e-6f) {
        for (i = protect; i < n; i++) frame[i] = 0.f;
        st->info.energy_scale = 0.f;
        return;
    }
    scale = target / raw;
    if (scale > MAX_SCALE) scale = MAX_SCALE;
    if (scale < MIN_SCALE) scale = MIN_SCALE;
    for (i = 0; i < n; i++) {
        float a = fabsf(frame[i]);
        if (a > peak) peak = a;
    }
    if (peak * scale > 0.95f * 32768.f)
        scale = 0.95f * 32768.f / peak;
    for (i = protect; i < n; i++) frame[i] *= scale;
    st->info.energy_scale = scale;
}

void plc_conceal(PlcEngine *st, short *out)
{
    float gen[PLC_FRAME_SAMPLES];
    int i;

    st->loss_count++;
    if (st->loss_count == 1) begin_loss(st);

    if (st->mode == PLC_MODE_REPEAT) {
        int off = st->hist_count >= PLC_FRAME_SAMPLES
                  ? st->hist_count - PLC_FRAME_SAMPLES : 0;
        for (i = 0; i < PLC_FRAME_SAMPLES; i++)
            gen[i] = st->hist[off + i];
    } else {
        int protect = 0;
        ensure_synth(st, st->read_pos + PLC_FRAME_SAMPLES);
        for (i = 0; i < PLC_FRAME_SAMPLES; i++) {
            int idx = st->read_pos + i;
            gen[i] = idx < st->synth_len ? st->synth[idx] : 0.f;
        }
        st->read_pos += PLC_FRAME_SAMPLES;

        if (st->loss_count == 1) {
            /* Entry continuity. Voiced blends over the whole frame (integer
               pitch-lag error must not be exposed); unvoiced uses a short
               window because a long blend of random noise creates artifacts. */
            int fade = st->pitch > 0 ? PLC_FRAME_SAMPLES : FADE_SAMPLES;
            if (fade > st->hist_count) fade = st->hist_count;
            protect = fade;
            for (i = 0; i < fade; i++) {
                int hidx = st->hist_count - 1 - i;
                float w_h = 0.5f * (1.f + cosf((float) M_PI * i / fade));
                gen[i] = w_h * st->hist[hidx] + (1.f - w_h) * gen[i];
            }
        }
        apply_envelope(st, gen, PLC_FRAME_SAMPLES, protect);

        /* De-click safety net.
           - Voiced: tight step clamp removes periodic-copy seams.
           - Unvoiced: isolated-pulse removal that preserves the continuous
             random process while clamping single-sample pops, plus a loose
             step clamp for genuine discontinuities. */
        if (st->loss_count > 1 && st->have_prev_final)
            gen[0] = st->prev_final;
        {
            int k;
            if (st->pitch > 0) {
                float max_step = 0.12f * 32768.f;
                for (k = 1; k < PLC_FRAME_SAMPLES; k++) {
                    float d = gen[k] - gen[k - 1];
                    if (d > max_step) gen[k] = gen[k - 1] + max_step;
                    else if (d < -max_step) gen[k] = gen[k - 1] - max_step;
                }
            } else {
                float max_step = 0.20f * 32768.f;
                for (k = 1; k < PLC_FRAME_SAMPLES; k++) {
                    float d = gen[k] - gen[k - 1];
                    if (d > max_step) gen[k] = gen[k - 1] + max_step;
                    else if (d < -max_step) gen[k] = gen[k - 1] - max_step;
                }
            }
        }
    }

    for (i = 0; i < PLC_FRAME_SAMPLES; i++) {
        out[i] = f2s(gen[i]);
        st->last_out[i] = out[i];
    }
    st->prev_final = gen[PLC_FRAME_SAMPLES - 1];
    st->have_prev_final = 1;
    st->prev_lost = 1;
    st->info.pitch_period = st->pitch;
    st->info.pitch_gain = st->pitch_gain;
    st->info.loss_count = st->loss_count;
}

void plc_accept_frame(PlcEngine *st, const short *frame)
{
    float f[PLC_FRAME_SAMPLES];
    int i;

    for (i = 0; i < PLC_FRAME_SAMPLES; i++) f[i] = (float) frame[i];

    if (st->mode == PLC_MODE_ENHANCED && st->prev_lost) {
        /* Recovery crossfade. The concealed synth stream is continuous, so we
           blend its final `fade` samples (in order) against the real frame.
           f[0] is anchored to the last concealed sample for an exact
           boundary match, then the concealed weight decays to zero. */
        int fade = st->pitch > 0 ? PLC_FRAME_SAMPLES : FADE_SAMPLES;
        if (fade > PLC_FRAME_SAMPLES) fade = PLC_FRAME_SAMPLES;
        for (i = 0; i < fade; i++) {
            int cidx = PLC_FRAME_SAMPLES - fade + i;
            float w_c = 0.5f * (1.f + cosf((float) M_PI * i / fade));
            f[i] = w_c * (float) st->last_out[cidx]
                 + (1.f - w_c) * (float) frame[i];
        }
        /* Guarantee exact boundary continuity regardless of indexing. */
        f[0] = st->prev_final;
        /* Smooth the whole frame so no seam can survive at the blend's end
           or anywhere across the recovery transition. */
        {
            float rec_step = (st->pitch > 0 ? 0.12f : 0.20f) * 32768.f;
        for (i = 1; i < PLC_FRAME_SAMPLES; i++) {
            float d = f[i] - f[i - 1];
            if (d > rec_step) f[i] = f[i - 1] + rec_step;
            else if (d < -rec_step) f[i] = f[i - 1] - rec_step;
        }
        }
    }

    if (st->hist_count + PLC_FRAME_SAMPLES <= PLC_HISTORY_SAMPLES) {
        memcpy(st->hist + st->hist_count, f,
               PLC_FRAME_SAMPLES * sizeof(float));
        st->hist_count += PLC_FRAME_SAMPLES;
    } else {
        memmove(st->hist, st->hist + PLC_FRAME_SAMPLES,
                (PLC_HISTORY_SAMPLES - PLC_FRAME_SAMPLES) * sizeof(float));
        memcpy(st->hist + PLC_HISTORY_SAMPLES - PLC_FRAME_SAMPLES, f,
               PLC_FRAME_SAMPLES * sizeof(float));
        st->hist_count = PLC_HISTORY_SAMPLES;
    }

    {
        float r = frame_rms(f, PLC_FRAME_SAMPLES);
        st->rms_ema = st->rms_ema == 0.f ? r : 0.85f * st->rms_ema + 0.15f * r;
    }

    st->prev_lost = 0;
    st->loss_count = 0;
    st->pitch = 0;
    st->info.loss_count = 0;
    for (i = 0; i < PLC_FRAME_SAMPLES; i++)
        st->last_out[i] = f2s(f[i]);
    st->prev_final = f[PLC_FRAME_SAMPLES - 1];
    st->have_prev_final = 1;
}

void plc_last_output(const PlcEngine *st, short *out)
{
    memcpy(out, st->last_out, PLC_FRAME_SAMPLES * sizeof(short));
}

const PlcInfo *plc_get_info(const PlcEngine *st)
{
    return &st->info;
}
