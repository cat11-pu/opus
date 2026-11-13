/* High-quality arbitrary-ratio polyphase resampler.
 *
 * Independent of the SILK fixed-rate resampler: dense phase grid (512
 * phases) plus a 64-tap windowed-sinc prototype give >100 dB image
 * rejection so that THD+N stays below -90 dB for every rate pair.
 * Integer-only (Q15 / Q32) arithmetic, same fixed-point conventions as
 * the rest of libopus; streaming state is carried across calls so the
 * output is continuous for any chunk size. */

#ifndef HQ_RESAMPLER_H
#define HQ_RESAMPLER_H

#include "opus_types.h"

#define HQ_RESAMPLER_PHASES   16384  /* dense fractional-delay grid     */
#define HQ_RESAMPLER_TAPS     192    /* even; taps per output sample   */
#define HQ_RESAMPLER_HALF     (HQ_RESAMPLER_TAPS / 2)

typedef struct hq_resampler_state hq_resampler_state;

struct hq_resampler_state {
    opus_int32       up;                 /* numerator of Fs_out/Fs_in   */
    opus_int32       down;               /* denominator of Fs_out/Fs_in */
    opus_uint64      pos_Q32;            /* output instant in input-tap
                                          * units, fractional Q32       */
    const opus_int32 *table;             /* Q23 polyphase table         */
    opus_int32       hist[HQ_RESAMPLER_TAPS];
    int              nhist;
};

/* Returns 0 on success, -1 on bad arguments. */
int hq_resampler_init(hq_resampler_state *st, opus_int32 fs_in, opus_int32 fs_out);

/* Resamples in[] (in_len samples) into out[]. Output capacity must be at
 * least (opus_int64)in_len * up / down + 2 samples. Returns the number of
 * output samples written. State is preserved for the next call. */
opus_int32 hq_resampler_process(hq_resampler_state *st,
                                opus_int16 *out, const opus_int16 *in,
                                opus_int32 in_len);

/* Coefficient fractional precision. Q15 is insufficient once 192 taps are
 * summed (the per-tap rounding error accumulates), so the table uses Q23. */
#define HQ_COEF_QBITS       23
#define HQ_COEF_SCALE       (1 << HQ_COEF_QBITS)

/* Build a ratio-specific polyphase table (Q23), HQ_RESAMPLER_PHASES rows of
 * HQ_RESAMPLER_TAPS coefficients. Exposed for the audit harness. */
void hq_resampler_build_table(opus_int32 table[HQ_RESAMPLER_PHASES][HQ_RESAMPLER_TAPS],
                              opus_int32 up, opus_int32 down);

#endif /* HQ_RESAMPLER_H */
