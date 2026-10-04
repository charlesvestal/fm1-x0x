// SPDX-License-Identifier: GPL-3.0-only
// The browser build's user break slots (web/emu/x0x_web.c web_slot_*): a WAV loaded into slot 1,
// chosen as loop A on the BREAK part's LOOPS page from the panel and played, must sound different
// from the same gestures with the slot left empty (loop A then stays a built-in loop).
//   node tests/host/emu_slots_test.mjs build/emu/x0x.wasm LOOP.wav
import fs from "fs";
const [wasmPath, wavPath] = process.argv.slice(2);
const bytes = fs.readFileSync(wasmPath), wav = fs.readFileSync(wavPath);

function mono16(w) {                                   // a 16-bit WAV's first channel
  const dv = new DataView(w.buffer, w.byteOffset);
  let off = 12, rate = 0, ch = 1, pcm = null;
  while (off < w.length - 8) {
    const id = w.toString("ascii", off, off + 4), sz = dv.getUint32(off + 4, true);
    if (id === "fmt ") { ch = dv.getUint16(off + 10, true); rate = dv.getUint32(off + 12, true); }
    if (id === "data") pcm = new Int16Array(w.buffer.slice(w.byteOffset + off + 8, w.byteOffset + off + 8 + sz));
    off += 8 + sz + (sz & 1);
  }
  return { pcm: ch === 1 ? pcm : pcm.filter((_, i) => i % ch === 0), rate };
}

async function run(load) {
  let mem;
  const clock = { clock_time_get: (a, b, o) => { new DataView(mem.buffer).setBigUint64(o, 0n, true); return 0; } };
  const { instance } = await WebAssembly.instantiate(bytes, { wasi_snapshot_preview1: clock });
  const ex = instance.exports;
  mem = ex.memory;
  ex._initialize();
  ex.web_boot();
  let sig = 0, peak = 0, rec = false;
  const r = (ms) => {
    for (let d = 0; d < ms * 44.1; d += 128) {
      ex.web_render(128);
      if (rec) for (const v of new Float32Array(mem.buffer, ex.web_out_l(), 128)) { peak = Math.max(peak, Math.abs(v)); sig = (sig * 31 + Math.round(v * 1000)) % 1000000007; }
    }
  };
  const tap = (b) => { ex.web_buttons(1 << b); r(40); ex.web_buttons(0); r(40); };
  const turn = (role, n) => { for (let i = 0; i < n; i++) { ex.web_enc(role, 1); r(80); } };
  const tapkey = (k) => { ex.web_keys(1 << k); r(60); ex.web_keys(0); r(30); };
  r(200);
  if (load) {
    const { pcm, rate } = mono16(wav);
    new Uint8Array(mem.buffer, ex.web_slot_namebuf(), 9).set([84, 69, 83, 84, 0, 0, 0, 0, 0]);   // "TEST"
    ex.web_slot_clear(0);
    new Int16Array(mem.buffer, ex.web_slot_pcm(), pcm.length).set(pcm);
    if (ex.web_slot_zone(0, 0, pcm.length, rate) !== 0) throw new Error("zone refused");
    ex.web_slots_changed();
  }
  turn(1, 4);                                          // ALGORITHM to BREAK
  tap(4); tap(4); tap(4);                              // EDIT to the LOOPS page
  turn(3, 2);                                          // KNOB 1: loop A two on (the first user loop)
  for (const k of [0, 7, 14, 21]) tapkey(k);           // white keys 1, 5, 9, 13
  tap(10);
  rec = true;
  r(4000);
  return { sig, peak };
}

const a = await run(true), b = await run(false);
console.log(`emu slots: with the loop ${a.sig} (peak ${a.peak.toFixed(3)}), without ${b.sig} (peak ${b.peak.toFixed(3)})`);
process.exit(a.peak > 0.05 && b.peak > 0.05 && a.sig !== b.sig ? 0 : 1);
