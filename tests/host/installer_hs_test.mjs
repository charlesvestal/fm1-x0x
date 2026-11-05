// SPDX-License-Identifier: MPL-2.0
// Copyright (C) 2026 Charles Vestal
// From fm1-base's tests/installer_hs_test.mjs (3c4a57e).
// The web installer's handshake (web/fm1ota.js) against fake FM-1s: what each firmware answers. A firmware on
// firmware/src/ota.c from M-UPGRADE 3.11's fix on answers the plain query with its name, '_' and three digits
// ("FM-1_901": M-UPGRADE 3.11 takes NAME_ and 1-3 digits only) and its whole identity to the query with the body 01
// (PERF-1's scheme, deamonSLINKY/PERF-1 775106b); the installer has to see the whole one, and every other firmware as
// before.
import { Updater, HS_FULL, pack7, unpack7 } from "../../web/fm1ota.js";

const HS_QUERY = [0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7];
const same = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
// an identity reply: the text, zeros, felucca-bsp#29's flash JEDEC id in bytes 24..26 (jedec), the checksum
const idPkt = (text, jedec) => {
  const t = [...text].map((c) => c.charCodeAt(0)).concat(new Array(27 - text.length).fill(0));
  if (jedec) t[24] = 0xB3, t[25] = 0x60, t[26] = 0x14;
  return [0xF0, ...pack7([0, 0x59, 0x11, 27, 0, 0, ...t, ~t.reduce((a, v) => a + v, 0) & 0xFF]), 0xF7];
};

// one port called FM-1: answer(query) -> the identity text to send back, or null
function access(answer, jedec) {
  const input = { name: "FM-1", state: "connected", onmidimessage: null, open: async () => {} };
  const output = {
    name: "FM-1", state: "connected", open: async () => {}, sent: [],
    send(b) {
      this.sent.push(Array.from(b));
      const t = answer(Array.from(b));
      if (t !== null) setTimeout(() => input.onmidimessage && input.onmidimessage({ data: idPkt(t, jedec) }), 5);
    },
  };
  return { inputs: new Map([[1, input]]), outputs: new Map([[1, output]]), output };
}

let bad = 0;
function check(what, cond) { console.log((cond ? "ok   " : "FAIL ") + what); if (!cond) bad++; }

const cut = (t) => t.slice(0, t.indexOf("_") + 4);
const fixed = (t) => (q) => same(q, HS_QUERY) ? cut(t) : same(q, HS_FULL) ? t : null;   // chip/ota.c with the fix
const before = (t) => (q) => same(q, HS_QUERY) ? t : null;                               // everything else
const cases = [
  ["a release with the fix: FM-1_901, then its whole identity", fixed("FM-1_9010200"), "FM-1_9010200", true, true],
  ["a release before it: its whole identity to the plain query", before("FM-1_9010200"), "FM-1_9010200", false, false],
  ["a dev build with the fix: FM-1_900", fixed("FM-1_900"), "FM-1_900", true, false],
  ["a dev build before it: FM-1_900, no answer to 01", before("FM-1_900"), "FM-1_900", true, false],
  ["M-VAVE's firmware: FM-1_015, no answer to 01", before("FM-1_015"), "FM-1_015", true, false],
  ["the update loader: ota-FM-1_900, never asked with 01", before("ota-FM-1_900"), "ota-FM-1_900", false, false],
];
for (const [what, answer, want, asksFull, jedec] of cases) {
  const a = access(answer, jedec);
  const dev = await new Updater(a).find();
  if (dev) dev.link.close();
  check(`${what}: ${want}`, dev && dev.id.text === want);
  check(`  ${asksFull ? "asked with 01" : "never asked with 01"}`, a.output.sent.some((q) => same(q, HS_FULL)) === asksFull);
}
{
  const a = access(fixed("FM-1_9010200"), true);
  const dev = await new Updater(a).find();
  if (dev) dev.link.close();
  check("felucca-bsp#29's bytes after the text: the text up to its first zero", dev && dev.id.text === "FM-1_9010200" &&
        dev.id.model === "FM-1" && dev.id.version === 9010200);
}
// the bytes of the query with 01, as chip/ota.c checks them (ota_take: header, length, checksum)
check("HS_FULL is 00 59 11, length 1, body 01, checksum FE",
      same(HS_FULL, [0xF0, ...pack7([0, 0x59, 0x11, 1, 0, 0, 1, 0xFE]), 0xF7]) &&
      same(unpack7(HS_FULL.slice(1, -1)).slice(0, 8), [0, 0x59, 0x11, 1, 0, 0, 1, 0xFE]));
if (bad) { console.log(`${bad} FAILED`); process.exit(1); }
console.log("installer handshake tests passed");
