#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# bass303 port vs schwung-303's Open303, at BASS303_OS = 1, 2 and 4 (1 is informational:
# reported against the same targets, not part of the exit status).
#   tests/host/run_bass303.sh [-v]        -v prints every note and glide
# Builds the reference (Open303 C++ + drive.h, unmodified, from ../schwung-303 or $SCHWUNG303)
# with the host C++ compiler, the port with the host C compiler and the firmware's warning
# flags (any warning in bass303.c / bass303_test.c fails the run), runs the comparison for
# each factor, and writes build/bass303_demo.wav (port, BASS303_OS default) and
# build/bass303_ref.wav (reference) for listening. Exits non-zero on any failure.
set -e
cd "$(dirname "$0")/../.."
REF="${SCHWUNG303:-../schwung-303}/src/dsp"
[ -f "$REF/open303/rosic_Open303.cpp" ] || { echo "schwung-303 not found at $REF (set SCHWUNG303)"; exit 2; }
OUT=build/bass303_host
mkdir -p "$OUT/ref"
CC="${CC:-cc}"
CXX="${CXX:-c++}"
CFLAGS="-std=c99 -O2 -ffp-contract=off -Wall -Wextra -Wdouble-promotion"

# --- the reference: Open303 driven the way plugin.cpp drives it, pots mapped as plugin.cpp maps
# them (n = pot / 127). Where the port's panel differs from plugin.cpp (Tune is centred on 440,
# Slide / AccDec are live without the Devilfish switch, drive mix is fixed at 1) the same
# physical values are handed to Open303 directly, so the engines are compared, not the panels.
cat > "$OUT/ref303.cpp" <<'EOF'
#include "open303/rosic_Open303.h"
#include "drive.h"
using namespace rosic;
struct Ref { Open303 e; drive::Drive d; int held = -1; int dtype = 1; float damt = 0.0f; };
extern "C" {
void *ref_create(void)
{
    Ref *r = new Ref();
    r->e.setSampleRate(44100.0);
    r->d.prepare(44100.0);
    /* plugin.cpp apply_devil_mods() with the switch off */
    r->e.setAmpDecay(1230.0); r->e.setAccentDecay(200.0); r->e.setFeedbackHighpass(150.0);
    r->e.setNormalAttack(3.0); r->e.setSlideTime(60.0); r->e.setTanhShaperDrive(36.9);
    return r;
}
void ref_destroy(void *p) { delete (Ref *)p; }
void ref_set(void *p, int i, int pot)
{
    Ref *r = (Ref *)p;
    double n = pot / 127.0;
    switch (i) {
    case 0: r->e.setCutoff(linToExp(n, 0, 1, 314.0, 2394.0)); break;
    case 1: r->e.setResonance(linToLin(n, 0, 1, 0.0, 100.0)); break;
    case 2: r->e.setEnvMod(linToLin(n, 0, 1, 0.0, 100.0)); break;
    case 3: r->e.setDecay(linToExp(n, 0, 1, 200.0, 2000.0)); break;
    case 4: r->e.setAccent(linToLin(n, 0, 1, 0.0, 100.0)); break;
    case 5: r->e.setWaveform(pot ? 1.0 : 0.0); break;
    case 6: r->e.setTuning(440.0 + (pot - 64) * 0.625); break;
    case 7: r->e.setVolume(linToLin(n, 0, 1, -60.0, 0.0)); break;
    case 8: r->damt = (float)n; break;
    case 9: r->dtype = pot; if (pot) r->d.set_model(pot - 1); break;
    case 10: r->e.setSlideTime(linToLin(n, 0, 1, 2.0, 360.0)); break;
    case 11: r->e.setAccentDecay(linToLin(n, 0, 1, 30.0, 3000.0)); break;
    }
}
/* the port's note API in MIDI terms: velocity >= 100 is accent, an overlapping note slides */
void ref_note_on(void *p, int note, int acc, int slide)
{
    Ref *r = (Ref *)p;
    int vel = acc ? 127 : 64;
    if (slide && r->held >= 0) {
        r->e.noteOn(note, vel, 0.0);
        r->e.noteOn(r->held, 0, 0.0);
    } else {
        if (r->held >= 0)
            r->e.noteOn(r->held, 0, 0.0);
        r->e.noteOn(note, vel, 0.0);
    }
    r->held = note;
}
void ref_note_off(void *p)
{
    Ref *r = (Ref *)p;
    if (r->held >= 0)
        r->e.noteOn(r->held, 0, 0.0);
    r->held = -1;
}
void ref_render(void *p, float *out, int n)
{
    Ref *r = (Ref *)p;
    for (int i = 0; i < n; i++)
        out[i] = (float)r->e.getSample();
    if (r->dtype)
        r->d.process(out, n, r->damt, 1.0f);
}
}
EOF

# reference objects (warnings are Open303's own, not checked)
for f in "$REF"/open303/*.cpp "$OUT/ref303.cpp"; do
    o="$OUT/ref/$(basename "$f" .cpp).o"
    [ -f "$o" ] && [ "$o" -nt "$f" ] && [ "$o" -nt "$0" ] && continue
    $CXX -std=c++17 -O2 -w -I"$REF" -c "$f" -o "$o"
done
[ -f "$OUT/ref/fft4g.o" ] || $CC -O2 -w -c "$REF/open303/fft4g.c" -o "$OUT/ref/fft4g.o"

FAIL=0
for OS in 1 2 4; do
    WARN="$OUT/warn_$OS.txt"
    $CC $CFLAGS -DBASS303_OS=$OS -c firmware/src/dsp/bass303.c -o "$OUT/bass303_$OS.o" 2> "$WARN" \
        && $CC $CFLAGS -DBASS303_OS=$OS -c tests/host/bass303_test.c -o "$OUT/test_$OS.o" 2>> "$WARN" \
        || { cat "$WARN"; echo "run_bass303: build FAILED"; exit 1; }
    if [ -s "$WARN" ]; then echo "warnings (BASS303_OS=$OS):"; cat "$WARN"; FAIL=1; fi
    # the same port with the control rate at every sample, symbols renamed k1_*
    K1=""
    for f in init set note_on note_off all_off render set_lite nparams param get; do K1="$K1 -Dbass303_$f=k1_bass303_$f"; done
    $CC $CFLAGS -DBASS303_OS=$OS -DBASS303_CTRL=1 $K1 -c firmware/src/dsp/bass303.c -o "$OUT/bass303_k1_$OS.o"
    rm -f "$OUT/bass303_test_$OS"     # macOS kills a binary relinked in place
    $CXX "$OUT/test_$OS.o" "$OUT/bass303_$OS.o" "$OUT/bass303_k1_$OS.o" "$OUT"/ref/*.o -o "$OUT/bass303_test_$OS"
    ARGS=""
    [ "$OS" = 2 ] && ARGS="--wav build"            # the default factor renders the demo
    [ "$OS" = 1 ] && ARGS="--info"                 # measured for comparison, not shipped
    "$OUT/bass303_test_$OS" $ARGS "$@" || FAIL=1
    echo
done
[ "$FAIL" = 0 ] && echo "run_bass303: PASS" || echo "run_bass303: FAIL"
exit $FAIL
