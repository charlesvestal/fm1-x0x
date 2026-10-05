#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# X0X host tests: the maths, the sequencer + TB-3PO (against schwung-tb3po), each engine
# against its reference implementation, and the whole firmware app in the simulator
# (tests/scenarios/*.x0x: UI, patterns, persistence, MIDI clock), with screenshots and
# audio in build/scenarios/. Needs the reference sources (see tests/host/run_*.sh).
#   tests/run_tests.sh
set -u
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
FAIL=0
OUT=build/host
mkdir -p "$OUT" build/scenarios
run() {
    name="$1"; shift
    if "$@" > "$OUT/$name.log" 2>&1; then
        echo "  ok   $name"
    else
        echo "  FAIL $name (see $OUT/$name.log)"
        tail -15 "$OUT/$name.log" | sed 's/^/       /'
        FAIL=1
    fi
}
W="-O2 -ffp-contract=off -Wall -Wextra -Wdouble-promotion -Werror"
run fastmath sh -c "$CC -O2 -ffp-contract=off -Wall -Wextra -Werror -o $OUT/fastmath_test tests/host/fastmath_test.c -lm && $OUT/fastmath_test"   # its reference is double on purpose
TB3PO_REF="${TB3PO_REF:-../schwung-tb3po/src/dsp/tb3po.c}"
seq_test() {
    if [ -f "$TB3PO_REF" ]; then
        ref="$(cd "$(dirname "$TB3PO_REF")" && pwd)/$(basename "$TB3PO_REF")"
        $CC -O2 -ffp-contract=off -Wall -Wextra "-DREF_TB3PO=\"$ref\"" -o $OUT/seq_test tests/host/seq_test.c \
            firmware/src/seq/sequencer.c firmware/src/seq/tb3po.c firmware/src/seq/pattern.c || return 1
    else
        echo "(no schwung-tb3po checkout at $TB3PO_REF: the TB-3PO comparison is skipped)"
        $CC -O2 -ffp-contract=off -Wall -Wextra -o $OUT/seq_test tests/host/seq_test.c \
            firmware/src/seq/sequencer.c firmware/src/seq/tb3po.c firmware/src/seq/pattern.c || return 1
    fi
    $OUT/seq_test
}
run sequencer seq_test
run encoder sh -c "$CC -O2 -w -Ifirmware/hal -o $OUT/encoder_test tests/host/encoder_test.c && $OUT/encoder_test"
run bass303_lite sh -c "$CC -std=c99 -O2 -ffp-contract=off -w -Ifirmware/src/dsp -Ifirmware/src -o $OUT/bass303_lite_test tests/host/bass303_lite_test.c firmware/src/dsp/bass303.c -lm && $OUT/bass303_lite_test"
run uac sh -c "$CC -O2 -w -Ifirmware/src -Ifirmware/hal -o $OUT/uac_test tests/host/uac_test.c -lm && $OUT/uac_test"
run trs sh -c "$CC -O2 -w -Ifirmware/src -Ifirmware/hal -o $OUT/trs_test tests/host/trs_test.c && $OUT/trs_test"
run undo sh -c "$CC -O2 -Wall -Wextra -Wno-unused-function -o $OUT/undo_test tests/host/undo_test.c && $OUT/undo_test"
run motion sh -c "$CC $W -o $OUT/motion_test tests/host/motion_test.c firmware/src/seq/motion.c && $OUT/motion_test"
run master sh -c "$CC $W -Wno-double-promotion -o $OUT/master_test tests/host/master_test.c firmware/src/dsp/master.c -lm && $OUT/master_test"
for t in drum909 drum808 bass303 breaks; do
    [ -f "tests/host/run_$t.sh" ] && run "$t" sh "tests/host/run_$t.sh"
done
run storage sh -c "$CC -O2 -o $OUT/storage_test tests/storage_test.c && $OUT/storage_test"
# the update path, against the firmware package (Felucca's tests; needs ./build.sh)
if [ -f build/x0x.fwsc ]; then
    run ota-entry sh -c "$CC -o $OUT/ota_test tests/ota_test.c && $OUT/ota_test build/x0x.fwsc"
    if [ -z "${AC79_SDK:-}" ]; then
        echo "  skip update-loader (needs AC79_SDK, as the build)"
    else
    run update-loader sh -c "head -c 200000 build/x0x.bin > $OUT/old_app.bin && \
        python3 tools/fm1pkg_make.py $OUT/old_app.bin build/loader/ota.bin $OUT/old.fwsc >/dev/null && \
        $CC -o $OUT/ldr_test tests/ldr_test.c && $OUT/ldr_test $OUT/old.fwsc build/x0x.fwsc"
    fi
    run installer python3 tests/install_test.py
else
    echo "  skip update-path tests (no build/x0x.fwsc: run ./build.sh)"
fi
run upload-images sh -c "python3 tests/host/make_test_loops.py build/test_loops && \
    python3 tools/upload_breaks.py --dry-run build/test_upload build/test_loops/loop*.wav"
run host-build sh host/build_host.sh
if command -v emcc >/dev/null 2>&1 && command -v node >/dev/null 2>&1; then   # the browser build, when it can be built
    printf 'wav emu_ref.wav\nwait 500\ntapkey b0\ntapkey w0\ntapkey w8\ntap PLAY\nwait 2960\nwavstop\n' > build/host/emu_ref.x0x
    run emu sh -c "sh web/emu/build.sh >/dev/null 2>&1 && build/host/x0x_host build/host/emu_ref.x0x build/host >/dev/null && \
        node tests/host/emu_test.mjs build/emu/x0x.wasm build/host/emu_ref.wav"
    run emu-slots node tests/host/emu_slots_test.mjs build/emu/x0x.wasm build/test_loops/loop1.wav
fi
for s in tests/scenarios/*.x0x; do
    n=$(basename "$s" .x0x)
    mkdir -p "build/scenarios/$n"
    run "scenario-$n" build/host/x0x_host "$s" "build/scenarios/$n"
done
[ $FAIL -eq 0 ] && echo "all tests passed" || echo "TESTS FAILED"
exit $FAIL
