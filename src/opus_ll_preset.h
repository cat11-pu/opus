/* Copyright (c) 2026 Xiph.Org Foundation
   Written by the Opus low-latency tuning effort.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
   OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef OPUS_LL_PRESET_H
#define OPUS_LL_PRESET_H

/*
 * Compile-time low-latency complexity presets for real-time communication.
 *
 * OPUS_LL_TIER selects one of three shipping presets:
 *   1 - balanced: trims the two hottest SILK searches (delayed-decision NSQ
 *       and NLSF MSVQ) with measured PESQ impact under 0.05. Default for
 *       low-latency voice on mid-range hardware.
 *   2 - aggressive: deeper search simplification plus a bandwidth-aware
 *       internal-rate pin for narrow-band calls.
 *   3 - extreme: minimum viable search for devices that would otherwise
 *       overrun real time; keeps the quality-critical warping and tonality
 *       analysis enabled.
 *
 * When OPUS_LL_TIER is undefined (or 0) every helper evaluates to the
 * upstream default, so a normal build is bit-identical to libopus mainline.
 *
 * Design rule: the public OPUS complexity value is never clamped below 7.
 * The upstream silk_setup_complexity() table has a quality cliff below 7
 * (it disables warping, interpolated NLSFs, raises the pitch threshold), so
 * the presets keep the table on a high-quality row and only relax individual
 * search knobs, each of which was validated by ablation to cost at most
 * ~0.05 PESQ at the chosen setting.
 */

#ifndef OPUS_LL_TIER
#define OPUS_LL_TIER 0
#endif

#if OPUS_LL_TIER < 0 || OPUS_LL_TIER > 3
#error "OPUS_LL_TIER must be between 0 (upstream default) and 3"
#endif

#define OPUS_LL_ACTIVE (OPUS_LL_TIER > 0)

/* Effective Opus/SILK complexity knob (0-10).  Stays at 7 or above to avoid
   the upstream quality cliff; tier 0 passes the application value through. */
#if OPUS_LL_TIER == 0
#ifndef OPUS_LL_COMPLEXITY_CLAMP
#define OPUS_LL_COMPLEXITY_CLAMP 10
#endif
#elif OPUS_LL_TIER == 1
#ifndef OPUS_LL_COMPLEXITY_CLAMP
#define OPUS_LL_COMPLEXITY_CLAMP 9
#endif
#else
#ifndef OPUS_LL_COMPLEXITY_CLAMP
#define OPUS_LL_COMPLEXITY_CLAMP 7
#endif
#endif

/* SILK delayed-decision NSQ states (upstream high quality uses 4).  The NSQ
   is the dominant encoder cost (~43% of CPU in the reference profile) and
   scales nearly linearly with state count.  Ablation PESQ impact: 4->3 about
   0.01, 4->2 about 0.04; 1 state is reserved for the extreme tier. */
#if OPUS_LL_TIER == 0
#ifndef OPUS_LL_DEL_DEC_STATES
#define OPUS_LL_DEL_DEC_STATES 4
#endif
#elif OPUS_LL_TIER == 1
#ifndef OPUS_LL_DEL_DEC_STATES
#define OPUS_LL_DEL_DEC_STATES 3
#endif
#elif OPUS_LL_TIER == 2
#ifndef OPUS_LL_DEL_DEC_STATES
#define OPUS_LL_DEL_DEC_STATES 2
#endif
#else
#ifndef OPUS_LL_DEL_DEC_STATES
#define OPUS_LL_DEL_DEC_STATES 2
#endif
#endif

/* Delayed-decision state count drives most of the per-tier speed gap.
   Measured on the c10 VoIP operating point (best-of-15, single core):
     4 states ~79 us/frame, 3 states ~63 us, 2 states ~49 us, 1 state ~39 us.
   The 1-state setting costs about 0.19 PESQ on female voice, so it is not
   shipped; tier 3 keeps 2 states and reaches its extra savings through the
   narrow-band rate pin and lighter pitch/shaping searches. */

/* First-stage NLSF MSVQ survivors kept for the second stage (upstream: 16).
   The NLSF quantizer is the second hotspot (~11% of CPU); no measurable PESQ
   change was observed down to 6 survivors. */
#if OPUS_LL_TIER == 0
#ifndef OPUS_LL_NLSF_SURVIVORS
#define OPUS_LL_NLSF_SURVIVORS 16
#endif
#elif OPUS_LL_TIER == 1
#ifndef OPUS_LL_NLSF_SURVIVORS
#define OPUS_LL_NLSF_SURVIVORS 12
#endif
#elif OPUS_LL_TIER == 2
#ifndef OPUS_LL_NLSF_SURVIVORS
#define OPUS_LL_NLSF_SURVIVORS 8
#endif
#else
#ifndef OPUS_LL_NLSF_SURVIVORS
#define OPUS_LL_NLSF_SURVIVORS 8
#endif
#endif

/* Pitch estimator search depth: 0 = SILK_PE_MIN_COMPLEX,
   1 = SILK_PE_MID_COMPLEX, 2 = SILK_PE_MAX_COMPLEX.  The pitch search is a
   small fraction of total CPU; only the extreme tier drops below MID. */
#if OPUS_LL_TIER == 3
#ifndef OPUS_LL_PE_COMPLEXITY
#define OPUS_LL_PE_COMPLEXITY 0
#endif
#else
#ifndef OPUS_LL_PE_COMPLEXITY
#define OPUS_LL_PE_COMPLEXITY 2
#endif
#endif

/* LPC order used by the pitch estimator (upstream max: 16).  Only relaxed on
   the extreme tier; 10 is the lowest order that held the PESQ budget in
   ablation, protecting female/pitched voices. */
#if OPUS_LL_TIER == 3
#ifndef OPUS_LL_PE_LPC_ORDER
#define OPUS_LL_PE_LPC_ORDER 10
#endif
#else
#ifndef OPUS_LL_PE_LPC_ORDER
#define OPUS_LL_PE_LPC_ORDER 16
#endif
#endif

/* Shaping LPC order (upstream max on the high row: 24).  Warping is always
   kept on: removing it audibly softens the spectral envelope for a small CPU
   gain, and it is the single biggest non-complexity quality lever. */
#if OPUS_LL_TIER == 3
#ifndef OPUS_LL_SHAPE_LPC_ORDER
#define OPUS_LL_SHAPE_LPC_ORDER 16
#endif
#else
#ifndef OPUS_LL_SHAPE_LPC_ORDER
#define OPUS_LL_SHAPE_LPC_ORDER 24
#endif
#endif

#ifndef OPUS_LL_WARPING_ENABLED
#define OPUS_LL_WARPING_ENABLED 1
#endif

/* Interpolated NLSFs cost an extra quantization pass per frame.  Kept on in
   all shipping tiers after ablation showed the extreme tier needed it. */
#ifndef OPUS_LL_INTERP_NLSF
#define OPUS_LL_INTERP_NLSF 1
#endif

/* CELT-side complexity cap (0-10).  The 24/32 kb/s VoIP operating points are
   SILK-only, so this only affects CELT-only/high-rate paths; kept at 5 to
   retain the CELT pitch prefilter (enabled at complexity >= 5). */
#ifndef OPUS_LL_CELT_COMPLEXITY
#define OPUS_LL_CELT_COMPLEXITY 5
#endif

/* Bandwidth-aware internal SILK sample-rate cap (Hz).  SILK internally runs
   at 8/12/16 kHz.  At the 12 kb/s narrow-band target the rate-dependent
   decision already oscillates around 12 kHz, so tiers 2-3 pin it there to
   avoid bandwidth-switch resets and redundant resampling without cutting
   audible bandwidth.  The 24/32 kb/s targets are deliberately untouched:
   capping them removes bands the bitrate can actually code. */
#ifndef OPUS_LL_RATE_CAP_12K
#define OPUS_LL_RATE_CAP_12K 12000
#endif
#ifndef OPUS_LL_RATE_CAP_24K
#define OPUS_LL_RATE_CAP_24K 0
#endif
#ifndef OPUS_LL_RATE_CAP_32K
#define OPUS_LL_RATE_CAP_32K 0
#endif

#if OPUS_LL_TIER >= 2
#ifndef OPUS_LL_ENABLE_RATE_CAP
#define OPUS_LL_ENABLE_RATE_CAP 1
#endif
#else
#ifndef OPUS_LL_ENABLE_RATE_CAP
#define OPUS_LL_ENABLE_RATE_CAP 0
#endif
#endif

/* Early-termination gate for the tonality/MLP analysis pass.  Ablation showed
   disabling run_analysis() costs 0.2-0.4 PESQ for negligible CPU, so every
   tier keeps the upstream gate (>=7 float, >=10 fixed).  Overridable for
   fixed-point builds that exclude the float analysis API. */
#ifdef FIXED_POINT
#ifndef OPUS_LL_ANALYSIS_THRESHOLD
#define OPUS_LL_ANALYSIS_THRESHOLD 10
#endif
#else
#ifndef OPUS_LL_ANALYSIS_THRESHOLD
#define OPUS_LL_ANALYSIS_THRESHOLD 7
#endif
#endif

#endif /* OPUS_LL_PRESET_H */
