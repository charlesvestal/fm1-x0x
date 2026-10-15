# espeak-ng for the screen reader

[espeak-ng](https://github.com/espeak-ng/espeak-ng) compiled to WebAssembly, English only, for
`web/reader/tts.js`: the reader's voice, offline, no CDN.

- `espeak.js`, `espeak.wasm`: Emscripten build (MODULARIZE, `createEspeak`), loaded by `../tts_worker.js`.
  The English data (phondata, phontab, phonindex, intonations, en_dict, voices `en` / `en-US`) is
  embedded in the wasm. No klatt, mbrola, sonic, pcaudio, speechPlayer or threads.
- `tts_wrap.c`: the four calls the worker uses (`tts_init`, `tts_synth(text, wpm, pitch)`,
  `tts_samples`, `tts_length`, `tts_rate`): 16-bit mono at 22050 Hz.

Built from espeak-ng commit `ba90c8e9f440ad544f674a790bb5f53878b6ffc5` with

    tools/build_espeak_web.sh [espeak-ng checkout]   # default: clones that commit into build/espeak-src

(needs cmake, a C compiler and emcc). Output is bit-identical to the native `espeak-ng -v en-us` of the same commit.

Licence: espeak-ng is GPL-3.0-or-later (`LICENSE`, its COPYING); the wrapper too. The source is the
commit above plus `tts_wrap.c` and the build script.
