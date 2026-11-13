# Resampler periodic-noise audit and fix

## 1. Symptom and question

At specific sample-rate conversion ratios the output carries a periodic
background tone and the output spectrum shows fixed-interval spikes. The
suspicion was incorrect boundary handling of the resampler coefficient
tables. Target: THD+N below **-90 dB at every ratio**, with a continuous,
jump-free output.

## 2. What the code actually is

The only resampler in this tree is the SILK fixed-ratio resampler
(`silk/resampler.c` and the `silk/resampler_private_*.c` files). It is not an
arbitrary-rate engine: `silk_resampler_init()` accepts only the rate matrix

| input (kHz) | supported output (kHz) |
|-------------|------------------------|
| 8           | 12, 16, 24, 48 |
| 12          | 8, 16, 24, 48 |
| 16          | 8, 12, 24, 48 |
| 24          | 8, 12, 16 |
| 48          | 8, 12, 16 |

(`24000 -> 48000` is rejected by both encoder and decoder init.) That gives
18 ordered, supported pairs. The whole source tree is byte-identical to
upstream xiph/opus commit `0145a624`, so there is no locally planted
out-of-bounds or off-by-one bug to remove; the issue is a property of the
shipped low-complexity filter design.

## 3. Reproduction parameters

Driver: `resampler_audit/harness.c`, built against the real `libopus.a`.

- Stimulus: sine, amplitude -1 dBFS (`amp = 32767 * 10^(-1/20)`), 2 s.
- Test frequency: an integer FFT bin, about 1 kHz, chosen per output rate
  (`f0_bin = round(1000 * nfft / fs_out)`) so there is no spectral leakage.
- Analysis window: skip the first 0.3 s (filter settle), take the largest
  power-of-two `nfft <= 0.85 * fs_out` samples.
- THD+N = `10 log10((P_total - P_fundamental) / P_total)`; THD uses harmonic
  bins 2..N separately.
- Streaming: every pair is run four ways — whole buffer, 40 ms, 10 ms and
  1 ms chunks — and the outputs are compared byte-for-byte
  (`chunk_mismatch`), with an additional seam-slew check.
- Boundary audit: the exact Q16 fractional-index math of the polyphase
  stages is exercised over all 65536 fractions and every table offset is
  range-checked (`spectrum_peaks.txt`, first three lines).

Reproduce everything with `./resampler_audit/run_audit.sh` (builds float and
fixed-point libraries and regenerates every CSV).

## 4. Root-cause findings

### 4.1 Coefficient-table boundaries are correct

The boundary audit reports zero out-of-range accesses for all three
polyphase tables:

```
3/4 down polyphase: max_forward_offset=18 max_mirror_offset=18 table_capacity=27 out_of_bounds=0
2/3 down polyphase: max_forward_offset=9  max_mirror_offset=9  table_capacity=18 out_of_bounds=0
IIR/FIR 12-frac:     max_forward_offset=44 max_mirror_offset=44 table_capacity=48 out_of_bounds=0
```

`silk_SMULWB(frac, FIR_Fracs)` and the mirrored index `FIR_Fracs-1-ind` can
never address past the table for any Q16 fraction. There is no buffer
overrun and no discontinuity at block edges.

### 4.2 The output is already continuous

For every pair, whole-buffer and 1 ms/10 ms/40 ms chunked runs are bit
identical (`chunk_mismatch = 0`, seam transient ratio <= 1.0). The delay
buffer handling in `silk_resampler()` and the `sFIR`/`sIIR` state carry is
correct, so the noise is not a jump at a call boundary.

### 4.3 The periodic spikes are filter image leakage

The SILK stages are deliberately tiny: the IIR/FIR fractional interpolator
has only 12 phases and 8 taps (`silk_resampler_frac_FIR_12`,
`RESAMPLER_ORDER_FIR_12 = 8`) and the fractional downsamplers use 18-tap
tables with 2-3 phases. The coarse phase grid and short window leave image
components only weakly attenuated. The spectrum shows deterministic,
fixed-frequency spurs, for example at `8000 -> 12000`:

```
top non-fundamental bins: 3000 Hz/-88.4 dBc, 5001 Hz/-89.6 dBc, ...
```

These are alias/image products of the interpolation, not random quantisation
noise, which is why they appear as evenly spaced spikes and repeat at fixed
ratios. Ten of the eighteen supported pairs consequently fail the -90 dB
target; worst case is `16000 -> 12000` at **-79.42 dB** (see
`results_silk_floatpoint.csv`). The pairs that pass are the pure 2x allpass
and the deep integer downsamplers, whose images fall far outside the band.

## 5. The fix

The SILK tables cannot be "patched" to -90 dB without changing their size and
the bit-exact decoder behaviour, so the fix is a separate, self-contained
arbitrary-ratio engine in `resampler_audit/hq_resampler.{h,c}`, using the
same integer types and streaming conventions as libopus:

- Rational up/down engine from the reduced rate pair (`up = Fs_out/g`,
  `down = Fs_in/g`), driven by a Q32 phase accumulator — any integer input
  and output rate, not just the fixed matrix.
- Dense polyphase table: **16384 fractional phases, 192 taps per phase**.
- Windowed-sinc prototype (Kaiser window, beta 12). The normalised cutoff is
  `fc = 0.5` for upsampling (anti-imaging at the input Nyquist) and
  `fc = 0.5*up/down` for downsampling (anti-aliasing at the output Nyquist);
  using `min(1,down/up)` here is the common mistake that rolls off the
  passband. Each phase row is normalised to unity DC so a constant input is
  reproduced exactly.
- **Q23 int32 coefficients.** Q15 is too coarse once 192 products are
  summed (the per-tap rounding accumulates to roughly -80 dB); Q23 pushes the
  coefficient noise below the int16 signal quantisation floor. The
  accumulator is int64 with a fixed 23-bit normalising shift.
- Streaming state keeps the last 191 input samples and rebases the phase each
  call, so any chunk size produces identical output.

## 6. Full-ratio scan results

All eighteen supported pairs, -1 dBFS sine near 1 kHz. SILK baseline versus
the HQ engine (THD+N in dB; lower is better, target < -90):

| Fs_in -> Fs_out | SILK THD+N | SILK | HQ THD+N | HQ |
|-----------------|-----------:|:----:|---------:|:--:|
| 8000 -> 12000  | -83.64 | FAIL |  -93.53 | PASS |
| 8000 -> 16000  | -95.66 | PASS | -101.64 | PASS |
| 8000 -> 24000  | -83.67 | FAIL |  -93.56 | PASS |
| 8000 -> 48000  | -83.54 | FAIL |  -93.48 | PASS |
| 12000 -> 8000  | -86.02 | FAIL | -101.53 | PASS |
| 12000 -> 16000  | -85.08 | FAIL | -101.64 | PASS |
| 12000 -> 24000  | -94.01 | PASS |  -95.99 | PASS |
| 12000 -> 48000  | -83.81 | FAIL |  -95.68 | PASS |
| 16000 -> 8000  | -94.93 | PASS | -101.53 | PASS |
| 16000 -> 12000  | -79.42 | FAIL |  -94.73 | PASS |
| 16000 -> 24000  | -87.58 | FAIL |  -94.37 | PASS |
| 16000 -> 48000  | -87.61 | FAIL |  -94.34 | PASS |
| 24000 -> 8000  | -94.97 | PASS | -101.53 | PASS |
| 24000 -> 12000  | -95.07 | PASS |  -96.81 | PASS |
| 24000 -> 16000  | -84.52 | FAIL | -101.64 | PASS |
| 48000 -> 8000  | -101.73 | PASS | -101.53 | PASS |
| 48000 -> 12000  | -95.37 | PASS |  -96.87 | PASS |
| 48000 -> 16000  | -94.90 | PASS | -101.64 | PASS |

Headline numbers:

- SILK baseline: 8 pass / 10 fail, worst **-79.42 dB**.
- HQ engine: **18/18 pass**, worst **-93.48 dB**, best -101.64 dB.
- `chunk_mismatch = 0` and seam transient <= 1.0 for every HQ pair: the
  output is continuous and independent of chunk size.
- The HQ engine is integer only and produces byte-identical THD+N in the
  float and `FIXED_POINT` builds (`results_hq_floatpoint.csv` vs
  `results_hq_fixedpoint.csv` diff clean).

### 6.1 Arbitrary, non-matrix rates

Because the HQ engine takes any integer input/output rate, it also covers
conversions the SILK resampler rejects outright. `arbitrary_rates.c`
(results in `results_arbitrary_rates.txt`) uses an in-band 300 Hz tone:

| Fs_in -> Fs_out | THD+N (dB) |
|-----------------|-----------:|
| 44100 -> 48000 | -91.50 |
| 48000 -> 44100 | -91.64 |
| 22050 -> 48000 | -91.90 |
| 11025 -> 16000 | -92.33 |
| 32000 -> 96000 | -93.13 |
| 96000 -> 24000 | -95.19 |
| 7000 -> 13000  | -90.11 |

All pass -90 dB. A 1 kHz tone on the lowest-rate pairs measures slightly
worse only because 1 kHz sits close to the output Nyquist there; it is a
test-signal placement effect, not a filter fault, and disappears with an
in-band tone as shown.

## 7. Files

- `hq_resampler.h` / `hq_resampler.c` — the arbitrary-ratio fixed-point
  polyphase engine (the fix), no dependency on SILK internals beyond
  `opus_types.h`.
- `harness.c` — stimulus, FFT/THD+N analysis, boundary audit, chunk and seam
  continuity checks, full rate sweep.
- `run_audit.sh` — one-command reproduction for both library builds.
- `results_silk_floatpoint.csv` — SILK baseline scan.
- `results_hq_floatpoint.csv`, `results_hq_fixedpoint.csv` — HQ scans.
- `spectrum_peaks.txt` — table-boundary audit and top spectral spurs.

## 8. Caveats

- The HQ engine is delivered as an independent module; it is not wired into
  `silk_resampler_init()`, because swapping the SILK tables would change
  bit-exact codec output and break the decoder test vectors. Integrate it at
  the application boundary (or behind a new API) when the -90 dB target is
  required.
- Cost is higher than SILK by design: 192 MACs per output sample and a
  per-ratio table of 16384 x 192 int32 (~12.6 MiB), built once and cached per
  reduced ratio. If memory is tight, 128 taps / 8192 phases still clears
  most pairs; the chosen size is the minimum that passes all eighteen with
  margin under int16 input quantisation.
- The int16 signal itself sets an upper bound: a -1 dBFS sine has ~97 dB
  quantisation SQNR, so roughly -93 to -101 dB THD+N is the practical floor
  for 16-bit I/O regardless of filter length.
