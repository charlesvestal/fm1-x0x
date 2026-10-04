// SPDX-License-Identifier: GPL-3.0-only
// The browser build (build/emu/x0x.wasm) against the host simulator: boot, half a second, PLAY
// for 40 ms, three seconds of the factory loop; the audio must match the simulator's WAV to its
// 16-bit rounding, and the screen and the lights must be drawn.
//   node tests/host/emu_test.mjs build/emu/x0x.wasm HOST.wav
import fs from "fs";
const [wasmPath, wavPath] = process.argv.slice(2);
let mem;
const clock = { clock_time_get: (id, p, out) => { new DataView(mem.buffer).setBigUint64(out, 0n, true); return 0; } };
const { instance } = await WebAssembly.instantiate(fs.readFileSync(wasmPath), { wasi_snapshot_preview1: clock });
const ex = instance.exports;
mem = ex.memory;
ex._initialize();
ex.web_boot();
const out = [];
const render = (ms) => {
  const n = Math.round(ms * 44.1);
  for (let d = 0; d < n; d += 128) {
    const k = Math.min(128, n - d);
    ex.web_render(k);
    out.push(...new Float32Array(mem.buffer, ex.web_out_l(), k));
  }
};
render(500);
ex.web_buttons(1 << 10);
render(40);
ex.web_buttons(0);
render(3000);
const wav = fs.readFileSync(wavPath);
const host = new Int16Array(wav.buffer, wav.byteOffset + 44, (wav.length - 44) >> 1).filter((_, i) => i % 2 === 0);
const n = Math.min(host.length, out.length);
let diff = 0, peak = 0;
for (let i = 0; i < n; i++) { diff = Math.max(diff, Math.abs(host[i] / 32767 - out[i])); peak = Math.max(peak, Math.abs(out[i])); }
const fb = new Uint16Array(mem.buffer, ex.web_fb(), 240 * 240);
const drawn = fb.some((p) => p !== 0), lit = ex.web_lit_keys() !== 0;
console.log(`emu: ${n} samples, peak ${peak.toFixed(3)}, max diff from the host ${diff.toExponential(2)}, screen ${drawn}, lights ${lit}`);
process.exit(n > 150000 && peak > 0.05 && diff < 1e-4 && drawn && lit ? 0 : 1);
