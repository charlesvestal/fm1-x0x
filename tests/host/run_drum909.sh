#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build the reference 9W9 engine (from its own sources) and the drum909/fxbus
# port with the host cc, compare them case by case, and write the demo.
#   tests/host/run_drum909.sh            (9W9 checkout: $NINEW9_DIR, see below)
# Exit status is non-zero on any failed comparison or any compiler warning in
# the port.
set -e
cd "$(dirname "$0")/../.."
: "${NINEW9_DIR:=../schwung-9W9}"          # fm1-parent/schwung-9W9 -> github.com/athousanddetails/schwung-9W9
[ -f "$NINEW9_DIR/src/dsp/er99_engine.c" ] || { echo "9W9 sources not found in $NINEW9_DIR (set NINEW9_DIR)"; exit 2; }
CC="${CC:-cc}"
OUT=build/drum909_test
mkdir -p "$OUT"

python3 tools/gen_drum_samples.py build/gen/x0x_drum_samples.h

# the reference: 9W9's engine as 9W9 builds it (libm, doubles)
"$CC" -O2 -ffp-contract=off -w -I"$NINEW9_DIR/src/dsp" -c "$NINEW9_DIR/src/dsp/er99_engine.c" -o "$OUT/er99_engine.o"
"$CC" -O2 -ffp-contract=off -w -I"$NINEW9_DIR/src/dsp" -c tests/host/drum909_ref.c -o "$OUT/drum909_ref.o"

# the same reference with its biquads in double: measures 9W9's own float noise
rm -rf "$OUT/ref_dbl" && cp -R "$NINEW9_DIR/src/dsp" "$OUT/ref_dbl"
perl -0pi -e 's/float x1, x2, y1, y2;/double x1, x2, y1, y2;/; s/const float y = f->b0 \* _in \+ f->b1 \* f->x1 \+ f->b2 \* f->x2\s*- f->a1 \* f->y1 - f->a2 \* f->y2;/const double y = (double)f->b0 * _in + (double)f->b1 * f->x1 + (double)f->b2 * f->x2 - (double)f->a1 * f->y1 - (double)f->a2 * f->y2;/; s/f->y2 = f->y1; f->y1 = y;\n    return y;/f->y2 = f->y1; f->y1 = y;\n    return (float)y;/' "$OUT/ref_dbl/webaudio.h"
grep -q "double x1, x2, y1, y2" "$OUT/ref_dbl/webaudio.h" && grep -q "return (float)y" "$OUT/ref_dbl/webaudio.h" || { echo "patching webaudio.h failed"; exit 2; }
"$CC" -O2 -ffp-contract=off -w -I"$OUT/ref_dbl" -c tests/host/drum909_refd.c -o "$OUT/drum909_refd.o"

# the port, warnings are errors
WARN="-Wall -Wextra -Wdouble-promotion -Werror"
for f in drum909 fxbus; do
    "$CC" -O2 -ffp-contract=off $WARN -std=c99 -Ifirmware/src/dsp -Ibuild/gen -c firmware/src/dsp/$f.c -o "$OUT/$f.o"
done
"$CC" -O2 -ffp-contract=off $WARN -Ifirmware/src -Ifirmware/src/dsp -Ibuild/gen -c tests/host/drum909_test.c -o "$OUT/drum909_test.o"
"$CC" -o "$OUT/drum909_test" "$OUT/drum909_test.o" "$OUT/drum909.o" "$OUT/fxbus.o" "$OUT/er99_engine.o" "$OUT/drum909_ref.o" "$OUT/drum909_refd.o" -lm

"$OUT/drum909_test" "$NINEW9_DIR/src" build/drum909_demo.wav
