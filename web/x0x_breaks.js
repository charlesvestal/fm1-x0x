// SPDX-License-Identifier: GPL-3.0-only
// X0X break loops in the browser: tools/upload_breaks.py and its helpers (tools/sampleio.py,
// Felucca's encoder; tools/import_breaks.py's bar guess) as plain functions, so the page
// (web/x0x_breaks.html) builds the same slot images the command line does, byte for byte
// (web/test_breaks.mjs checks it against upload_breaks.py --dry-run).
//
// The float arithmetic follows sampleio.py's order of operations: keep it when touching these.

export const SLOT_SIZE = 0x14000, SLOT_DATA_OFF = 512, SLOT_RATE = 22050, SLOT_ZONES = 16;
export const SLOT_HDR_LEN = 32 + SLOT_ZONES * 28;
export const SLOT_MAX_DATA = SLOT_SIZE - SLOT_DATA_OFF;
export const ROOT_BASE = 36;              // loop i is zone root 36 + i: the slot sorts its zones by root
export const NSLOTS = 3;

const IMA_STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
  97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
  724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
  4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
  18500, 20350, 22385, 24623, 27086, 29794, 32767];
const IMA_IDX = [-1, -1, -1, -1, 2, 4, 6, 8];

// import_breaks.bars_of: the bar count (1, 2 or 4) that puts the loop's tempo nearest 120 inside 80-180
export function barsOf(seconds) {
  let best = null;
  for (const b of [1, 2, 4]) {
    const bpm = b * 240.0 / seconds;
    if (bpm >= 80 && bpm <= 180 && (best === null || Math.abs(bpm - 120) < Math.abs(best[1] - 120))) best = [b, bpm];
  }
  return best || [1, 240.0 / seconds];
}

// sampleio.resample: a moving average over the ratio, then linear interpolation
export function resample(x, sr, to) {
  if (sr === to) return x;
  if (sr > to) {
    const k = Math.max(1, Math.round(sr / to));
    if (k === 2) {
      const y = new Array(x.length);
      for (let i = 0; i + 1 < x.length; i++) y[i] = (x[i] + x[i + 1]) / 2;
      if (x.length) y[x.length - 1] = x[x.length - 1] / 2;
      x = y;
    } else if (k > 1) {
      const y = new Array(x.length);
      for (let i = 0; i < x.length; i++) {
        let acc = x[i];                           // reduce(add, x[i:i + k]): left to right
        for (let j = i + 1; j < i + k && j < x.length; j++) acc += x[j];
        y[i] = acc / k;
      }
      x = y;
    }
  }
  const step = sr / to, out = [], last = x.length - 1;
  let p = 0.0;
  while (p < last) {
    const i = Math.trunc(p), f = p - i;
    out.push(x[i] * (1 - f) + x[i + 1] * f);
    p += step;
  }
  return out;
}

function peak(x, floor = 1e-9) {
  let m = floor;
  for (const v of x) { const a = Math.abs(v); if (a > m) m = a; }
  return m;
}

// sampleio.to_int16: peak normalised to 30000
export function toInt16(x) {
  const pk = peak(x), out = new Int16Array(x.length);
  for (let i = 0; i < x.length; i++) out[i] = Math.max(-32768, Math.min(32767, Math.trunc(x[i] / pk * 30000)));
  return out;
}

// upload_breaks.cut: mono floats at their rate -> int16 at the slot's rate (one bar if bars)
export function cut(x, sr, bars) {
  if (bars) {
    const n = barsOf(x.length / sr)[0];
    x = x.slice(0, Math.round(x.length / n));
  }
  const y = resample(x, sr, SLOT_RATE);
  const pk = peak(y);
  return toInt16(y.map((v) => v * (0.95 / pk)));
}

// sampleio.ima_encode: 4-bit IMA ADPCM, low nibble first, from predictor 0 / index 0
export function imaEncode(s) {
  let pred = 0, idx = 0;
  const out = new Uint8Array((s.length + 1) >> 1);
  for (let n = 0; n < s.length; n++) {
    const step = IMA_STEP[idx];
    let diff = s[n] - pred, code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    let vd = step >> 3;
    if (diff >= step) { code |= 4; diff -= step; vd += step; }
    if (diff >= step >> 1) { code |= 2; diff -= step >> 1; vd += step >> 1; }
    if (diff >= step >> 2) { code |= 1; vd += step >> 2; }
    pred = code & 8 ? pred - vd : pred + vd;
    if (pred > 32767) pred = 32767; else if (pred < -32768) pred = -32768;
    idx += IMA_IDX[code & 7];
    if (idx < 0) idx = 0; else if (idx > 88) idx = 88;
    out[n >> 1] |= n & 1 ? code << 4 : code;
  }
  return out;
}

let CRC_T = null;
export function crc32(b) {                    // zlib.crc32
  if (!CRC_T) {
    CRC_T = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
      CRC_T[n] = c >>> 0;
    }
  }
  let c = 0xFFFFFFFF;
  for (let i = 0; i < b.length; i++) c = CRC_T[(c ^ b[i]) & 0xFF] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
}

function keySplit(roots) {
  const n = roots.length;
  return roots.map((r, j) => [j === 0 ? 0 : ((roots[j - 1] + r) >> 1) + 1, j === n - 1 ? 127 : (r + roots[j + 1]) >> 1]);
}

// sampleio.user_slot for loops (each its own zone, roots ROOT_BASE + i): -> {hdr, data}
export function userSlot(name, loops) {
  const parts = loops.map((s) => imaEncode(s));
  const total = parts.reduce((a, p) => a + p.length, 0);
  if (total > SLOT_MAX_DATA) throw new Error(`too long: ${total} B of ADPCM, a slot holds ${SLOT_MAX_DATA} B`);
  if (loops.length < 1 || loops.length > SLOT_ZONES) throw new Error(`1..${SLOT_ZONES} loops per slot`);
  const data = new Uint8Array(total);
  const zs = [];
  let off = 0;
  loops.forEach((s, i) => {
    data.set(parts[i], off);
    zs.push({ off, n: s.length, root: ROOT_BASE + i });
    off += parts[i].length;
  });
  const split = keySplit(zs.map((z) => z.root));
  const rate = Math.round(SLOT_RATE / 44100 * 65536);
  const hdr = new Uint8Array(SLOT_HDR_LEN), v = new DataView(hdr.buffer);
  v.setUint32(0, 0x504D5346, true);
  v.setUint16(4, 1, true);
  hdr[6] = zs.length;
  const nm = name.toUpperCase().replace(/[^\x20-\x7E]/g, "").slice(0, 8);
  for (let i = 0; i < nm.length; i++) hdr[8 + i] = nm.charCodeAt(i);
  v.setUint32(16, data.length, true);
  v.setUint32(20, crc32(data), true);
  zs.forEach((z, i) => {
    const o = 32 + i * 28;
    v.setUint32(o, z.off, true);
    v.setUint32(o + 4, z.n, true);
    v.setUint32(o + 8, 0, true);               // loop start
    v.setUint32(o + 12, z.n - 1, true);        // loop end
    v.setUint32(o + 16, rate, true);
    v.setInt16(o + 20, z.root * 16, true);
    v.setInt16(o + 22, 0, true);               // predictor and index at the loop start: from 0
    hdr[o + 24] = 0;
    hdr[o + 25] = split[i][0];
    hdr[o + 26] = split[i][1];
  });
  return { hdr, data };
}

// upload_breaks.pack: loops [{label, s}] in order into at most nslots slots; -> {slots, skipped}
export function pack(loops, nslots) {
  const slots = [], skipped = [];
  let cur = [], used = 0;
  for (const l of loops) {
    const need = (l.s.length + 1) >> 1;
    if (need > SLOT_MAX_DATA) { skipped.push(l.label); continue; }
    if (used + need > SLOT_MAX_DATA || cur.length >= SLOT_ZONES) {
      if (cur.length) slots.push(cur);
      cur = []; used = 0;
    }
    if (slots.length >= nslots) { skipped.push(l.label); continue; }
    cur.push(l);
    used += need;
  }
  if (cur.length) slots.push(cur);
  return { slots, skipped };
}

// fm1_sample_upload.pack7: groups of up to 7 bytes, each preceded by their top bits
export function pack7(b) {
  const out = [];
  for (let i = 0; i < b.length; i += 7) {
    const g = b.slice(i, i + 7);
    let m = 0;
    g.forEach((x, j) => { m |= ((x >> 7) & 1) << j; });
    out.push(m);
    for (const x of g) out.push(x & 0x7F);
  }
  return out;
}

// a 16-bit / 24-bit / float WAV -> {rate, x: mono floats} (sampleio.read_any_wav; the page decodes
// other formats with the browser's decoder)
export function readWav(buf) {
  const d = new DataView(buf.buffer || buf, buf.byteOffset || 0, buf.byteLength);
  const tag4 = (o) => String.fromCharCode(d.getUint8(o), d.getUint8(o + 1), d.getUint8(o + 2), d.getUint8(o + 3));
  if (d.byteLength < 12 || tag4(0) !== "RIFF" || tag4(8) !== "WAVE") throw new Error("not a WAV file");
  const ch = {};
  for (let i = 12; i + 8 <= d.byteLength;) {
    const id = tag4(i), n = d.getUint32(i + 4, true);
    if (!(id in ch)) ch[id] = [i + 8, n];
    i += 8 + n + (n & 1);
  }
  if (!ch["fmt "]) throw new Error("WAV without a fmt chunk");
  const fo = ch["fmt "][0];
  let tag = d.getUint16(fo, true);
  const nch = d.getUint16(fo + 2, true), sr = d.getUint32(fo + 4, true), bits = d.getUint16(fo + 14, true);
  if (tag === 0xFFFE) tag = d.getUint16(fo + 24, true);
  const bps = bits >> 3;
  if ((tag !== 1 && tag !== 3) || !bps || !nch) throw new Error(`unsupported WAV (tag ${tag}, ${bits} bit)`);
  const [o, n] = ch.data || [0, 0];
  const frame = bps * nch, nfr = Math.floor(Math.min(n, d.byteLength - o) / frame);
  const val = (p) => {
    if (tag === 3) return bps === 4 ? d.getFloat32(p, true) : d.getFloat64(p, true);
    if (bps === 2) return d.getInt16(p, true) / 32768;
    if (bps === 3) return ((d.getUint8(p) << 8 | d.getUint8(p + 1) << 16 | d.getUint8(p + 2) << 24) >> 0) / 2147483648;
    if (bps === 4) return d.getInt32(p, true) / 2147483648;
    return (d.getUint8(p) - 128) / 128;
  };
  const x = new Array(nfr);
  for (let f = 0; f < nfr; f++) {
    const p = o + f * frame;
    if (nch === 1) x[f] = val(p);
    else if (nch === 2) x[f] = (val(p) + val(p + bps)) / 2;
    else { let acc = 0.0; for (let c = 0; c < nch; c++) acc += val(p + c * bps); x[f] = acc / nch; }
  }
  return { rate: sr, x };
}
