#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Host test of firmware/src/dsp/breaks.c (the BREAK part): generator rules, audio slice check at
# 90 / 170 BPM, step gate, live keys, click bound (+ its positive control), build/breaks_demo.wav.
set -e
cd "$(dirname "$0")/../.."
mkdir -p build
CFLAGS="-O2 -ffp-contract=off -Wall -Wextra -Wdouble-promotion -Werror"
# the DSP file alone, strict C99
cc $CFLAGS -std=c99 -pedantic -Ifirmware/src -c firmware/src/dsp/breaks.c -o build/breaks_host.o
cc $CFLAGS tests/host/breaks_test.c -lm -o build/breaks_test
./build/breaks_test build/breaks_demo.wav
# positive control: with the crossfade compiled out the click check must fire
cc $CFLAGS -DBRK_FADE=1 -DBRK_CLICK_CONTROL tests/host/breaks_test.c -lm -o build/breaks_test_control
if ./build/breaks_test_control; then
    echo "FAIL: click check did not fire with the crossfade removed"; exit 1
else
    rc=$?
    [ "$rc" -eq 3 ] || { echo "FAIL: control exited $rc"; exit 1; }
    echo "control ok: click check fires without the crossfade"
fi
echo "breaks: all ok"
