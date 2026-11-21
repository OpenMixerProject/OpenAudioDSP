#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
set -eu

wing=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$wing/../..
if [ ! -f "$root/include/openaudiodsp/eq.h" ]; then
    echo 'WING strips require the shared kernels from OpenAudioDSP PR #1' >&2
    exit 1
fi
build=$(mktemp -d "${TMPDIR:-/tmp}/openaudiodsp-wing.XXXXXX")
trap 'rm -rf "$build"' EXIT

# The host cannot run ADI EQ; abort rather than silently emulating it.
cat > "$build/filter.h" <<'EOF'
#include <stdlib.h>
static inline void biquad_trans(float *x, const float *c, float *s, int n, int bands) { abort(); }
EOF
for kernel in gate compressor eq; do
    "${CC:-cc}" -O2 -Dpm= -I"$root/include" -I"$build" \
        -c "$root/src/mixer/$kernel.c" -o "$build/$kernel.o"
done

for dsp in 1 2 3 4; do
    set --
    if [ "$dsp" != 4 ]; then
        set -- "$wing/strips.cpp" "$build/gate.o" "$build/compressor.o" "$build/eq.o"
    fi
    "${CC:-cc}" -std=c99 -O2 -DWING_DSP="$dsp" -c "$wing/runtime.c" -o "$build/runtime.o"
    "${CXX:-c++}" -O2 -Dpm= -DWING_TEST_HOST -DWING_DSP="$dsp" -I"$root/include" \
        "$wing/audio.cpp" "$wing/audio_test.cpp" "$build/runtime.o" "$@" -o "$build/dsp$dsp"
    "$build/dsp$dsp"
    printf 'DSP%s WING block and WDSP vectors passed\n' "$dsp"
done
