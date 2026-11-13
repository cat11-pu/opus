#!/bin/sh
# Reproduce the SILK resampler audit and the HQ replacement measurements.
# Builds libopus (float and fixed), compiles the harness, and regenerates:
#   results_sweep.csv          SILK baseline (float lib)
#   results_hq.csv             HQ polyphase engine (float lib)
#   results_hq_fixedpoint.csv  HQ engine against the FIXED_POINT lib
#   spectrum_peaks.txt         boundary audit + top spectral spurs
set -e
cd "$(dirname "$0")/.."

JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
COMMON="-DOPUS_BUILD -DUSE_ALLOCA -Iinclude -Isilk -Icelt"

echo "== float libopus =="
make -f Makefile.unix lib -j"$JOBS" >/dev/null
gcc -O2 -g $COMMON -Isilk/float \
    resampler_audit/harness.c resampler_audit/hq_resampler.c libopus.a -lm \
    -o resampler_audit/harness
(cd . && resampler_audit/harness)
cp resampler_audit/results_hq.csv resampler_audit/results_hq_floatpoint.csv
cp resampler_audit/results_sweep.csv resampler_audit/results_silk_floatpoint.csv

echo "== fixed-point libopus =="
make -f Makefile.unix clean >/dev/null
make -f Makefile.unix FIXED_POINT=1 lib -j"$JOBS" >/dev/null
gcc -O2 -g $COMMON -DFIXED_POINT=1 -DDISABLE_FLOAT_API -Isilk/fixed \
    resampler_audit/harness.c resampler_audit/hq_resampler.c libopus.a -lm \
    -o resampler_audit/harness_fixed
resampler_audit/harness_fixed
cp resampler_audit/results_hq.csv resampler_audit/results_hq_fixedpoint.csv

echo "== restore float lib =="
make -f Makefile.unix clean >/dev/null
make -f Makefile.unix lib -j"$JOBS" >/dev/null

echo
echo "SILK baseline failures (THD+N >= -90 dB):"
awk -F, 'NR>1 && $8 >= -90 {printf "  %s->%s %s  THD+N=%.2f dB\n",$2,$3,$4,$8}' \
    resampler_audit/results_silk_floatpoint.csv
echo "HQ engine failures:"
awk -F, 'NR>1 && $18=="FAIL"' resampler_audit/results_hq_floatpoint.csv || true
echo "HQ worst THD+N:"
awk -F, 'NR>1{print $8}' resampler_audit/results_hq_floatpoint.csv | sort -n | head -1

echo "== arbitrary (non-matrix) rates, 300 Hz in-band tone =="
gcc -O2 $COMMON -Isilk/float \
    resampler_audit/arbitrary_rates.c resampler_audit/hq_resampler.c libopus.a -lm \
    -o resampler_audit/arbitrary_rates
resampler_audit/arbitrary_rates | tee resampler_audit/results_arbitrary_rates.txt
