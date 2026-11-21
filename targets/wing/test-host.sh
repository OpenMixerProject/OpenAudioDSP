#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
set -eu

wing=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/openaudiodsp-wing.XXXXXX")
trap 'rm -rf "$build"' EXIT

for dsp in 1 2 3 4; do
    "${CC:-cc}" -std=c99 -O2 -DWING_DSP="$dsp" -c "$wing/runtime.c" -o "$build/runtime.o"
    "${CXX:-c++}" -O2 -DWING_DSP="$dsp" "$wing/audio.cpp" "$wing/audio_test.cpp" "$build/runtime.o" -o "$build/dsp$dsp"
    "$build/dsp$dsp"
    printf 'DSP%s WING block and WDSP vectors passed\n' "$dsp"
done
