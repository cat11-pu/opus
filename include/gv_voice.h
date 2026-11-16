/* Low-latency game voice mode: narrow interface over libopus.
 *
 * Fixed pipeline contract:
 *   - 48 kHz, mono, 20 ms frames (960 samples per frame).
 *   - End-to-end codec+jitter latency budget: <= 40 ms.
 *       20 ms frame + 20 ms (exactly one frame) jitter/FEC lookahead.
 *   - In-band FEC (SILK LBRR) recovers single losses with one frame of
 *     lookahead; PLC covers bursts of up to 3 consecutive losses with
 *     intelligible output.
 *   - The receiver ALWAYS emits exactly 960 samples per 20 ms slot
 *     (decode / FEC / PLC), so the output timeline is sample-continuous
 *     across loss and recovery.
 *
 * This is a narrow interface: no resampling, no multichannel, no
 * multistream, no custom modes. Anything outside the contract above is
 * rejected at create/config time.
 */

#ifndef GV_VOICE_H
#define GV_VOICE_H

#include "opus.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GV_SAMPLE_RATE   48000
#define GV_CHANNELS      1
#define GV_FRAME_MS      20
#define GV_FRAME_SAMPLES 960   /* GV_SAMPLE_RATE * GV_FRAME_MS / 1000 */

/* Jitter buffer depth in frames. 1 frame of lookahead is what makes
 * in-band FEC usable while keeping total latency <= 40 ms. */
#define GV_JITTER_DEPTH  2     /* current frame + 1 lookahead frame */

/* After this many consecutive PLC frames the decoder fades to comfort
 * noise instead of extrapolating speech (intelligibility is only
 * guaranteed up to 3). */
#define GV_PLC_FADE_AFTER 3

typedef struct GVEnc GVEnc;
typedef struct GVDec GVDec;

/* Wire packet. seq is a uint16 sequence number incremented per frame;
 * wrap-around at 65535 -> 0 is legal and handled. len <= 4000. */
typedef struct {
    opus_uint16 seq;
    opus_uint16 len;
    const unsigned char *data;
} GVPacket;

/* ---------------- Encoder ---------------- */

/* bitrate_bps: recommended 24000..40000 for game voice.
 * loss_perc_hint: expected uplink loss in percent (0..100); passed to
 * OPUS_SET_PACKET_LOSS_PERC so the encoder sizes its LBRR redundancy.
 * Returns NULL on invalid arguments or allocation failure. */
GVEnc *gv_enc_create(opus_int32 bitrate_bps, int loss_perc_hint);
void   gv_enc_destroy(GVEnc *enc);

/* Encodes exactly one 20 ms frame (960 int16 samples, interleaved mono).
 * out must hold at least max_out bytes; returns payload length (>0),
 * 0 for DTX (send nothing), or a negative GV_ERR_* code. */
int gv_enc_encode(GVEnc *enc, const opus_int16 *pcm,
                  unsigned char *out, int max_out);

/* ---------------- Decoder / jitter buffer ---------------- */

GVDec *gv_dec_create(void);
void   gv_dec_destroy(GVDec *dec);

/* Inserts a received packet into the jitter buffer. Packets may arrive
 * out of order within the GV_JITTER_DEPTH window. Duplicates and
 * packets too late for their slot are silently dropped (counted in
 * stats). Returns 0 on accept, 1 on drop, < 0 on error. */
int gv_dec_push(GVDec *dec, const GVPacket *pkt);

/* Emits exactly one 20 ms frame (960 int16 samples) for the next slot,
 * in this priority order:
 *   1. normal decode, if the packet for this slot is present;
 *   2. FEC decode from the NEXT slot's packet (decode_fec=1), if this
 *      slot is missing but the following packet carries LBRR data;
 *   3. PLC (opus_decode with NULL), for bursts beyond FEC reach.
 * Because exactly one frame is emitted per call, the output is
 * sample-continuous across loss and recovery. Call once per 20 ms of
 * playback clock. Returns the frame source (GV_SRC_*). */
#define GV_SRC_DECODE 0
#define GV_SRC_FEC    1
#define GV_SRC_PLC    2
#define GV_SRC_DTX    3   /* sender was in DTX; comfort noise */
int gv_dec_pull(GVDec *dec, opus_int16 *pcm_out);

/* Hard resync: drops all buffered packets and resets the Opus decoder
 * state. Use on stream join or after a detected clock/seq discontinuity
 * larger than the jitter window. */
void gv_dec_reset(GVDec *dec);

/* ---------------- Loss statistics ---------------- */

typedef struct {
    /* Cumulative counters (monotonic until gv_dec_reset_stats). */
    opus_uint32 packets_received;  /* accepted into jitter buffer   */
    opus_uint32 packets_lost;      /* slots emitted without a packet */
    opus_uint32 packets_late;      /* arrived after slot was emitted */
    opus_uint32 packets_duplicate; /* same seq pushed twice          */
    opus_uint32 fec_recovered;     /* slots filled by in-band FEC    */
    opus_uint32 plc_frames;        /* slots filled by PLC            */
    opus_uint32 dtx_frames;        /* slots with no TX (silence)     */

    /* Burst analysis: a loss event is a maximal run of consecutive
     * lost slots. */
    opus_uint32 loss_events;       /* number of loss bursts          */
    opus_uint32 max_burst;         /* longest burst, in frames       */
    opus_uint32 cur_burst;         /* ongoing burst length (0 = none)*/
    opus_uint32 bursts_le3;        /* bursts of 1..3 (intelligible)  */
    opus_uint32 bursts_gt3;        /* bursts of 4+  (degraded)       */

    /* Derived quality indicators. */
    float loss_rate;               /* packets_lost / slots, 0..1     */
    float fec_recovery_rate;       /* fec_recovered / recoverable    */
} GVLossStats;

void gv_dec_get_stats(GVDec *dec, GVLossStats *stats);
void gv_dec_reset_stats(GVDec *dec);

/* ---------------- Errors ---------------- */

#define GV_ERR_OK          0
#define GV_ERR_BAD_ARG    -1
#define GV_ERR_ALLOC_FAIL -2
#define GV_ERR_BAD_STATE  -3

#ifdef __cplusplus
}
#endif

#endif /* GV_VOICE_H */
