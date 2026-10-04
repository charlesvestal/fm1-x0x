#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# X0X in the browser: the host simulator compiled to WebAssembly (Emscripten), plus its page.
#   [X0X_VERSION=0.4-beta] web/emu/build.sh  ->  build/emu/{index.html, worklet.js, x0x.wasm}
# Same sources and flags as host/build_host.sh (-ffp-contract=off, like the device).
set -e
cd "$(dirname "$0")/../.."
mkdir -p build/gen build/emu
[ -f build/gen/felucca_font.h ] || python3 tools/gen_font.py build/gen/felucca_font.h >/dev/null
[ -f build/gen/x0x_drum_samples.h ] || python3 tools/gen_drum_samples.py build/gen/x0x_drum_samples.h >/dev/null
sh tools/gen_builtin_break.sh build/gen >/dev/null
U="firmware/src/dsp/drum909.c firmware/src/dsp/drum808.c firmware/src/dsp/bass303.c firmware/src/dsp/breaks.c
   firmware/src/dsp/fxbus.c firmware/src/dsp/master.c firmware/src/seq/sequencer.c firmware/src/seq/tb3po.c
   firmware/src/seq/pattern.c firmware/src/seq/motion.c firmware/src/app/engine.c"
# shellcheck disable=SC2086
emcc -O2 -ffp-contract=off -std=gnu99 -Wall -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable \
    -DX0X_HOST -DX0X_WEB "-DX0X_VERSION=\"$(printf %s "${X0X_VERSION:-DEV}" | tr a-z A-Z)\"" -Ifirmware/src -Ifirmware/src/dsp -Ibuild/gen \
    --no-entry -sSTANDALONE_WASM -sSTACK_SIZE=1048576 -sINITIAL_MEMORY=33554432 -sFILESYSTEM=0 \
    -o build/emu/x0x.wasm web/emu/x0x_web.c $U
cp web/emu/index.html web/emu/worklet.js build/emu/
echo "emu: build/emu ($(wc -c < build/emu/x0x.wasm) B wasm)"
