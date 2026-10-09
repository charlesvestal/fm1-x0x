#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build the X0X host simulator (the whole app on this machine) into build/host/x0x_host.
# Same sources as the firmware, -ffp-contract=off like the device (no fused multiply-add).
set -e
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
mkdir -p build/gen build/host
[ -f build/gen/felucca_font.h ] || python3 tools/gen_font.py build/gen/felucca_font.h >/dev/null
[ -f build/gen/x0x_drum_samples.h ] || python3 tools/gen_drum_samples.py build/gen/x0x_drum_samples.h >/dev/null
if [ -n "${X0X_BREAK_BANK:-}" ]; then python3 tools/gen_break_bank.py "$X0X_BREAK_BANK" build/gen/x0x_break_bank.h >/dev/null
else sh tools/gen_builtin_break.sh build/gen >/dev/null; fi
U="firmware/src/dsp/drum909.c firmware/src/dsp/drum808.c firmware/src/dsp/bass303.c firmware/src/dsp/breaks.c
   firmware/src/dsp/fxbus.c firmware/src/dsp/master.c firmware/src/seq/sequencer.c firmware/src/seq/tb3po.c
   firmware/src/seq/pattern.c firmware/src/seq/motion.c firmware/src/app/engine.c"
# shellcheck disable=SC2086
$CC -O2 -ffp-contract=off -std=c99 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
    -DX0X_HOST ${X0X_HOST_DEFS:-} -Ifirmware/src -Ifirmware/src/dsp -Ibuild/gen -o ${X0X_HOST_OUT:-build/host/x0x_host} host/x0x_host.c $U -lm
echo "host: build/host/x0x_host"
