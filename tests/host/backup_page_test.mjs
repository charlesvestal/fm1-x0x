// SPDX-License-Identifier: GPL-3.0-only
// The web backup page (web/backup/index.html, the part between its "backup protocol" markers; after Polly's test):
//  1. against tools/x0x_backup.py's own conversation with a simulated X0X (tests/x0x_backup_record.py): X0X's side
//     replayed, every message the page sends checked byte for byte against the tool's (a backup, a restore, a restore
//     while playing);
//  2. against a simulated X0X in JS (backup.c's 128-byte frames and end frame, restore_rx()'s reads: the head, each
//     record, its payload in 512s, the tail, the question, the result) on the device's own image (tests/backup_test.c):
//     a backup byte for byte, a restore byte for byte, STOP FIRST, HOME, a damaged file, another version's and
//     Polly's (nothing sent), not X0X, an X0X that stops asking, a lost frame.
//   node tests/host/backup_page_test.mjs build/host/backup_transcript.json
import fs from "fs";
import path from "path";
import zlib from "zlib";
import { fileURLToPath } from "url";

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const html = fs.readFileSync(path.join(root, "web/backup/index.html"), "utf8");
const m = /\/\/ --- backup protocol: begin[^\n]*\n([\s\S]*?)\/\/ --- backup protocol: end/.exec(html);
if (!m) { console.log("FAIL no backup protocol markers in web/backup/index.html"); process.exit(1); }
const PB = new Function(`"use strict";\n${m[1]}\nreturn PB;`)();
Object.assign(PB.DELAY, { poll: 5, idle: 300, ask: 500, first: 300, hs: 100, hsFull: 100 });

let fails = 0;
const check = (ok, msg) => { console.log(`${ok ? "ok  " : "FAIL"} ${msg}`); if (!ok) fails++; };
const hex = (a) => Buffer.from(a).toString("hex");
const same = (a, b) => a.length === b.length && Buffer.compare(Buffer.from(a), Buffer.from(b)) === 0;
const crc = (b) => zlib.crc32 ? zlib.crc32(Buffer.from(b)) >>> 0 : null;

const tr = JSON.parse(fs.readFileSync(process.argv[2] || path.join(root, "build/host/backup_transcript.json"), "utf8"));
const img = Uint8Array.from(Buffer.from(tr.image, "hex")), other = Uint8Array.from(Buffer.from(tr.other, "hex"));

// the page's CRC-32 is zlib's
if (crc(img) !== null) check(PB.crc32(img) === crc(img) && PB.crc32(other) === crc(other), "CRC-32 as zlib's");

// ---------------------------------------------------------------- 1. the tool's conversation, replayed
class ReplayLink {
  constructor(log) { this.log = log; this.i = 0; this.q = []; this.lost = false; this.bad = null; this.deliver(); }
  deliver() { while (this.i < this.log.length && this.log[this.i][0] === "dev") this.q.push([...Buffer.from(this.log[this.i++][1], "hex")]); }
  send(pkt) {
    const want = this.log[this.i];
    if (!this.bad && (!want || want[0] !== "host" || want[1] !== hex(pkt))) this.bad = `message ${this.i}: sent ${hex(pkt).slice(0, 40)}..., the tool sent ${want ? want[1].slice(0, 40) : "nothing"}...`;
    this.i++;
    this.deliver();
    return true;
  }
  drain() { this.q.length = 0; }
  async read() { await null; return this.q.shift() || null; }
  close() {}
  get done() { return this.i === this.log.length; }
}
const access1 = { inputs: new Map([["i", { name: "X0X FM-1", state: "connected", open: async () => {} }]]),
                  outputs: new Map([["o", { name: "X0X FM-1", state: "connected", open: async () => {} }]]) };
{
  const link = new ReplayLink(tr.runs.backup);
  const dev = await PB.find(access1, () => link);
  const r = await PB.backup(dev.link);
  check(!link.bad && link.done, `backup: every message as x0x_backup.py's (${tr.runs.backup.length})` + (link.bad ? ": " + link.bad : ""));
  check(same(r.image, img) && r.flags === 0, "backup: the file byte for byte as the tool's");
}
{
  const link = new ReplayLink(tr.runs.restore);
  let asked = 0;
  const dev = await PB.find(access1, () => link);
  const code = await PB.restore(dev.link, img, (w) => { if (w === "ask") asked++; });
  check(!link.bad && link.done, `restore: every message as x0x_backup.py's (${tr.runs.restore.length})` + (link.bad ? ": " + link.bad : ""));
  check(code === 0 && asked === 1, "restore: done, the FM-1's question passed on once");
}
{
  const link = new ReplayLink(tr.runs.restore_playing);
  const dev = await PB.find(access1, () => link);
  const code = await PB.restore(dev.link, img);
  check(!link.bad && link.done && code === PB.PLAYING, "restore while playing: STOP FIRST, as the tool");
}

// ---------------------------------------------------------------- 2. a simulated X0X
const u32 = (b, o) => (b[o] | b[o + 1] << 8 | b[o + 2] << 16 | b[o + 3] << 24) >>> 0;
const le32 = (v) => [v & 255, v >>> 8 & 255, v >>> 16 & 255, v >>> 24 & 255];
function devFrame(cmd, body) {                                  // backup.c's bu_frame
  const s = body.reduce((a, v) => a + v, 0);
  return [0xF0, ...PB.pack7([0, 0x59, cmd, body.length & 255, body.length >> 8, 0, ...body, ~s & 255]), 0xF7];
}
class FakeX0X {
  constructor(image, o = {}) {
    Object.assign(this, { image, identity: "FM-1_9001008", playing: false, answer: true, stallAfter: null, dropFrame: null }, o);
    this.req = null; this.asked = 0; this.reads = 0; this.q = []; this.lost = false;
  }
  // the Link the page gets
  send(pkt) { this.rx(pkt); return true; }
  drain() { this.q.length = 0; }
  async read(ms) { if (!this.q.length) await new Promise((r) => setTimeout(r, Math.min(ms, 2))); return this.q.shift() || null; }
  close() {}
  stream() {                                                   // bu_step: 128-byte frames, then the end frame
    let n = 0;
    for (let off = 0; off < this.image.length; off += 128, n++) {
      if (n === this.dropFrame) continue;
      this.q.push(devFrame(PB.BU_REQ, [...le32(off), ...this.image.subarray(off, off + 128)]));
    }
    this.q.push(devFrame(PB.BU_END, [...le32(this.image.length), ...le32(PB.crc32(this.image)), 0, 0]));
  }
  *puller(n, c) {                                              // restore_rx's reads
    if (this.playing) { yield [PB.FAIL_ADDR | PB.PLAYING, 8]; return; }
    const got = [...(yield [0, 16])];
    let pos = 16;
    for (let i = 0; i < PB.NOBJ; i++) {
      const r = yield [pos, 8];
      got.push(...r);
      const ln = r[2] | r[3] << 8;
      pos += 8;
      for (let k = 0; k < ln; k += 512) got.push(...(yield [pos + k, Math.min(512, ln - k)]));
      pos += ln;
    }
    got.push(...(yield [pos, 4]));
    const g = Uint8Array.from(got);
    if (g.length !== n || PB.crc32(g) !== c) { yield [PB.FAIL_ADDR | 6, 8]; return; }
    try { PB.parse(g); } catch (_) { yield [(PB.FAIL_ADDR | 5) >>> 0, 8]; return; }
    yield [PB.ASK_ADDR, 8];
    this.asked++;
    if (!this.answer) { yield [(PB.FAIL_ADDR | PB.DECLINED) >>> 0, 8]; return; }
    this.image = g;
    yield [PB.OK_ADDR, 8];
  }
  ask(addr, n) {
    const q = [0, ...le32(addr >>> 0), n & 255, n >> 8, 0];
    this.q.push([0xF0, ...PB.pack7([0, 0x59, 0x30, 8, 0, 0, ...q, ~q.reduce((a, v) => a + v, 0) & 255]), 0xF7]);
    this.wanted = [addr >>> 0, n];
  }
  next(data) {
    const r = this.pull.next(data);
    if (r.done) { this.pull = null; return; }
    this.ask(...r.value);
  }
  rx(d) {
    // newOta: ota.c from M-UPGRADE 3.11's fix on, NAME_ and three digits to HS_QUERY, the whole to HS_FULL
    const reply = (text) => {
      const t = [...Buffer.from(text), ...new Array(27 - text.length).fill(0)];
      t[24] = 0xB3, t[25] = 0x60, t[26] = 0x14;                  // felucca-bsp#29: the flash's JEDEC id after the text
      this.q.push([0xF0, ...PB.pack7([0, 0x59, 0x11, 27, 0, 0, ...t, ~t.reduce((a, v) => a + v, 0) & 255]), 0xF7]);
    };
    if (same(d, PB.HS_QUERY)) {
      reply(this.newOta ? this.identity.slice(0, this.identity.indexOf("_") + 4) : this.identity);
      return;
    }
    if (same(d, PB.HS_FULL)) {
      this.hsFull = (this.hsFull || 0) + 1;
      if (this.newOta) reply(this.identity);
      return;
    }
    const f = PB.parseFrame(d);
    if (f && f.cmd === PB.BU_REQ && same(f.body, PB.MAGIC)) { this.stream(); return; }
    if (f && f.cmd === PB.RS_REQ && f.body.length === 8) {
      this.req = [u32(f.body, 0), u32(f.body, 4)];
      this.pull = this.puller(...this.req);
      this.next(undefined);
      return;
    }
    const u = PB.unpack7(d.slice(1, -1));
    if (this.pull && this.wanted && u.length >= 15 && u[2] === 0x30) {
      const [addr, n] = this.wanted;
      if (u32(u, 7) !== addr || (u[11] | u[12] << 8) !== n) return;
      if (this.stallAfter !== null && ++this.reads > this.stallAfter) return;
      this.next(Uint8Array.from(u.slice(14, 14 + n)));
    }
  }
}
const accessOf = (dev) => ({ inputs: new Map([["i", { name: "X0X FM-1", state: "connected", open: async () => {} }]]),
                             outputs: new Map([["o", { name: "X0X FM-1", state: "connected", open: async () => {} }]]), dev });
const connect = (dev) => PB.find(accessOf(dev), () => dev);
const err = async (p) => { try { await p; return null; } catch (e) { return e; } };

{
  const dev = new FakeX0X(img);
  const r = await PB.backup((await connect(dev)).link);
  check(same(r.image, img), "simulated X0X: backup byte for byte");
  const back = new FakeX0X(other);
  const code = await PB.restore((await connect(back)).link, r.image);
  check(code === 0 && same(back.image, img) && back.asked === 1, "simulated X0X: restore, the device holds the backup byte for byte");
  check(back.req[0] === img.length && back.req[1] === PB.crc32(img), "... the request: its size and CRC-32");
}
{
  const dev = new FakeX0X(other, { playing: true });
  const code = await PB.restore((await connect(dev)).link, img);
  check(code === PB.PLAYING && same(dev.image, other), "playing: STOP FIRST, nothing changed");
}
{
  const dev = new FakeX0X(other, { answer: false });
  const code = await PB.restore((await connect(dev)).link, img);
  check(code === PB.DECLINED && same(dev.image, other) && dev.asked === 1, "HOME on the FM-1: not restored, nothing changed");
}
{
  const bad = img.slice(); bad[bad.length >> 1] ^= 1;
  let e = null; try { PB.parse(bad); } catch (x) { e = x; }
  check(e && /damaged/.test(e.message), "a damaged file: refused (" + (e && e.message) + ")");
  const dev = new FakeX0X(other);
  const e2 = await err(PB.restore((await connect(dev)).link, bad));
  check(e2 && dev.req === null && same(dev.image, other), "... nothing sent");
}
for (const [what, magic, ver, want] of [["another version (2)", PB.MAGIC, 2, /another version of X0X/],
                                        ["Polly's backup", [0x50, 0x4C, 0x59, 0x42], 2, /Polly backup/]]) {
  const old = img.slice();                                     // its CRCs right
  old.set(magic, 0);
  old[4] = ver; old[5] = 0;
  old.set(le32(PB.crc32(old.subarray(0, 12))), 12);
  old.set(le32(PB.crc32(old.subarray(0, old.length - 4))), old.length - 4);
  let e = null; try { PB.parse(old); } catch (x) { e = x; }
  check(e && want.test(e.message), `${what}: refused, said so (${e && e.message})`);
  const dev = new FakeX0X(other);
  await err(PB.restore((await connect(dev)).link, old));
  check(dev.req === null && same(dev.image, other), "... nothing sent");
}
check(PB.VERSION === 1 && (img[4] | img[5] << 8) === 1, "the device's backups: version 1");
{
  let e = null; try { PB.parse(Uint8Array.from(Buffer.from("not a backup at all".repeat(20)))); } catch (x) { e = x; }
  check(e && /not an X0X backup/.test(e.message), "another file: not an X0X backup");
}
{
  const dev = new FakeX0X(other, { identity: "FM-1_3000401" });
  const e = await err(connect(dev));
  check(e && e.code === "model" && /not X0X/.test(e.message), "not X0X (Polly's identity): refused");
}
{
  const dev = new FakeX0X(img, { identity: "FM-1_9010200", newOta: true });
  const c = await connect(dev);
  check(c && c.id === "FM-1_9010200" && dev.hsFull === 1,
        "an X0X that answers FM-1_901 to the plain query (M-UPGRADE 3.11): its whole identity asked with 01");
  c.link.close();
}
{
  const dev = new FakeX0X(other, { stallAfter: 5 });
  const e = await err(PB.restore((await connect(dev)).link, img));
  check(e && e.code === "stopped" && same(dev.image, other), "X0X stops asking: stopped, nothing changed");
}
{
  const dev = new FakeX0X(img, { dropFrame: 3 });
  const e = await err(PB.backup((await connect(dev)).link));
  check(e && e.code === "lost", "a backup frame lost on the way: said, nothing saved");
}
{
  const empty = { inputs: new Map(), outputs: new Map() };
  const e = await err(PB.find(empty, () => null));
  check(e && e.code === "notfound", "no FM-1: not found");
}
console.log(fails ? `backup page test FAILED (${fails})` : "backup page test passed");
process.exit(fails ? 1 : 0);
