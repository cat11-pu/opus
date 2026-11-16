#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${BUILD_DIR:-"$root/artifacts/opus_properties_build"}
reference_build_dir=${REFERENCE_BUILD_DIR:-"$root/artifacts/opus_properties_reference"}
out_dir=${OUT_DIR:-"$root/artifacts/opus_properties"}
cc=${CC:-cc}

asan_flags="-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all"
reference_flags="-O1 -g -fno-omit-frame-pointer"

cmake -S "$root" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER="$cc" \
    -DCMAKE_C_FLAGS="$asan_flags" \
    -DBUILD_SHARED_LIBS=OFF \
    -DOPUS_BUILD_SHARED_LIBRARY=OFF \
    -DOPUS_BUILD_PROGRAMS=OFF \
    -DOPUS_BUILD_TESTING=OFF \
    -DBUILD_TESTING=OFF

cmake --build "$build_dir" --target opus --parallel

cmake -S "$root" -B "$reference_build_dir" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER="$cc" \
    -DCMAKE_C_FLAGS="$reference_flags" \
    -DBUILD_SHARED_LIBS=ON \
    -DOPUS_BUILD_SHARED_LIBRARY=ON \
    -DOPUS_BUILD_PROGRAMS=OFF \
    -DOPUS_BUILD_TESTING=OFF \
    -DBUILD_TESTING=OFF

cmake --build "$reference_build_dir" --target opus --parallel

"$cc" $asan_flags \
    -I"$root/include" -I"$build_dir" \
    "$root/tests/test_opus_properties.c" "$build_dir/libopus.a" \
    -lm -ldl -fsanitize=address,undefined \
    -o "$build_dir/test_opus_properties"

case "$out_dir" in
    "$root"/artifacts/*) rm -rf -- "$out_dir" ;;
esac
mkdir -p "$out_dir"

if [ -n "${OPUS_REFERENCE_LIB:-}" ]; then
    reference_lib=$OPUS_REFERENCE_LIB
else
    reference_lib="$reference_build_dir/libopus.so"
fi

ASAN_OPTIONS=${ASAN_OPTIONS:-"detect_leaks=1:halt_on_error=1:abort_on_error=1:strict_string_checks=1"}
UBSAN_OPTIONS=${UBSAN_OPTIONS:-"halt_on_error=1:print_stacktrace=1"}
export ASAN_OPTIONS UBSAN_OPTIONS

exec "$build_dir/test_opus_properties" --output "$out_dir" \
    --reference "$reference_lib"
