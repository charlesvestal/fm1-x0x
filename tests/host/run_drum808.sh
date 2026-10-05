#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# drum808 (the port) against 8W8's sc808_engine.cpp, and the 808 demo.
#   tests/host/run_drum808.sh [-v]       -v prints every case, not just failures
# Builds the reference (8W8's engine, unmodified, double + libm) with the host C++
# compiler from $EIGHTW8_DIR, the port with the host C compiler and the firmware's
# warning flags (any warning in drum808.c / drum808_test.c fails the run), compares
# every sound and pot, and writes build/drum808_demo.wav. Exits non-zero on failure.
set -e
cd "$(dirname "$0")/../.."
: "${EIGHTW8_DIR:=/private/tmp/claude-501/-Volumes-ExtFS-charlesvestal-github-schwung-parent-schwung/e33a5e22-f1db-4de6-868c-2e6586090880/scratchpad/8w8}"
REF="$EIGHTW8_DIR/src/dsp"
[ -f "$REF/sc808_engine.cpp" ] || { echo "8W8 sources not found in $EIGHTW8_DIR (set EIGHTW8_DIR)"; exit 2; }
CC="${CC:-cc}"
CXX="${CXX:-c++}"
OUT=build/drum808_test
mkdir -p "$OUT"
WARN="-Wall -Wextra -Wdouble-promotion -Werror"

# the reference, as 8W8 builds it (C++14, double, libm), minus FMA contraction
"$CXX" -std=c++14 -O2 -ffp-contract=off -w -I"$REF" -c "$REF/sc808_engine.cpp" -o "$OUT/sc808_engine.o"
# the port, C99, warnings are errors
"$CC" -std=c99 -O2 -ffp-contract=off $WARN -DD8_TAIL_DB=0 -Ifirmware/src/dsp -c firmware/src/dsp/drum808.c -o "$OUT/drum808.o"
"$CC" -std=c99 -O2 -ffp-contract=off $WARN -Ifirmware/src/dsp -I"$REF" -c tests/host/drum808_test.c -o "$OUT/drum808_test.o"
"$CXX" -o "$OUT/drum808_test" "$OUT/drum808_test.o" "$OUT/drum808.o" "$OUT/sc808_engine.o" -lm

"$OUT/drum808_test" build/drum808_demo.wav "$@"
