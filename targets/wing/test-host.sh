#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
set -eu

wing=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
fx=$wing/../../src/effects
build=$(mktemp -d "${TMPDIR:-/tmp}/openaudiodsp-wing.XXXXXX")
trap 'rm -rf "$build"' EXIT

for header in matrix vector stats window filter; do touch "$build/$header.h"; done
cat > "$build/builtins.h" <<'EOF'
inline float fclipf(float x, float limit) { return x > limit ? limit : x < -limit ? -limit : x; }
EOF

for dsp in 1 2 3 4; do
    set --
    if [ "$dsp" = 4 ]; then
        set -- "$wing/overdrive.cpp" "$fx/fxBase.cpp" "$fx/fxOverdrive.cpp"
    fi
    "${CC:-cc}" -std=c99 -O2 -DWING_DSP="$dsp" -c "$wing/runtime.c" -o "$build/runtime.o"
    "${CXX:-c++}" -O2 -Dpm= -DWING_DSP="$dsp" -I"$build" -I"$wing" -I"$fx" \
        "$wing/audio.cpp" "$wing/audio_test.cpp" "$build/runtime.o" "$@" -lm -o "$build/dsp$dsp"
    "$build/dsp$dsp"
    printf 'DSP%s WING block and WDSP vectors passed\n' "$dsp"
done
