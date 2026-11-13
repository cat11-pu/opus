#ifndef PLC_ENGINE_H
#define PLC_ENGINE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sample format: 16-bit signed PCM, mono, interleaved. */
#define PLC_SAMPLE_RATE 16000
#define PLC_FRAME_SAMPLES 160      /* 10 ms @ 16 kHz */
#define PLC_HISTORY_FRAMES 16      /* 160 ms of history */
#define PLC_HISTORY_SAMPLES (PLC_HISTORY_FRAMES * PLC_FRAME_SAMPLES)
#define PLC_MAX_LOSS_RUN 12       /* beyond this, fade to comfort noise */

typedef enum {
    PLC_MODE_REPEAT = 0,   /* baseline: repeat last frame */
    PLC_MODE_ENHANCED = 1  /* pitch extrapolation + crossfade + energy match */
} plc_mode_t;

typedef struct PlcEngine PlcEngine;

PlcEngine *plc_create(plc_mode_t mode);
void       plc_destroy(PlcEngine *st);
void       plc_reset(PlcEngine *st);

/* Feed one correctly received frame (PLC_FRAME_SAMPLES samples). */
void plc_accept_frame(PlcEngine *st, const short *frame);

/* After accept_frame(), copy the (possibly recovery-blended) output that the
   engine actually placed on the wire for that frame into out. */
void plc_last_output(const PlcEngine *st, short *out);

/* Produce one concealment frame for a lost packet. */
void plc_conceal(PlcEngine *st, short *out);

/* Read-only diagnostics, populated on every conceal() call. */
typedef struct {
    int   pitch_period;     /* samples, 0 if unvoiced */
    float pitch_gain;       /* best normalized autocorrelation */
    float energy_scale;     /* applied RMS scale */
    int   loss_count;       /* consecutive loss index (1-based) */
} PlcInfo;
const PlcInfo *plc_get_info(const PlcEngine *st);

#ifdef __cplusplus
}
#endif

#endif /* PLC_ENGINE_H */
