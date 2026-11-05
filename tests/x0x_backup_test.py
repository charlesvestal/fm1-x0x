#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""tools/x0x_backup.py against a simulated X0X (no hardware, no mido; after Polly's test). Its stored project is the
image the device's code made in tests/backup_test.c (firmware/src/backup.c's format, read by the tool's parser); it
streams a backup in backup.c's frames (128 bytes, then the end frame) and pulls a restore as restore_rx() does (the
head, each record, its payload in 512s, the tail, then the question and the result). Checked: the backup written
byte for byte; a restore sent whole and taken only on SEL; playing (STOP FIRST), HOME, a damaged file, another
version's and Polly's (nothing sent), not X0X, an X0X that stops asking.
Run from the repo root:  python3 tests/x0x_backup_test.py IMAGE"""
import io
import os
import queue
import shutil
import struct
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import fm1_install as I  # noqa: E402
import x0x_backup as B  # noqa: E402

I.DELAY.update(open=0, hs=0.1, hs_full=0.1, retry=0.02)
B.DELAY.update(poll=0.02, idle=0.3, ask=0.5, first=0.3)
failed = 0


def ok(cond, what):
    global failed
    print(f"{what:<66} {'ok' if cond else 'FAIL'}")
    failed += not cond


class Link:
    def __init__(self, dev):
        self.dev, self.q, self.lost = dev, queue.Queue(), False

    def send(self, pkt):
        self.dev.rx(self, bytes(pkt))
        return True

    def read(self, timeout):
        try:
            return self.q.get(timeout=max(timeout, 0))
        except queue.Empty:
            return None

    def drain(self):
        while not self.q.empty():
            self.q.get_nowait()

    def close(self):
        pass


def dev_frame(cmd, body):
    """backup.c's bu_frame"""
    n = len(body)
    return b"\xF0" + I.pack7(bytes([0, 0x59, cmd, n & 0xFF, n >> 8, 0]) + body + bytes([~sum(body) & 0xFF])) + b"\xF7"


class FakeX0X:
    def __init__(self, image, identity="FM-1_9001008", playing=False, answer=True, stall_after=None, flags=0):
        self.image, self.identity, self.playing, self.answer = image, identity, playing, answer
        self.stall_after, self.flags = stall_after, flags
        self.req, self.asked, self.pull, self.wanted, self.reads = None, 0, None, None, 0

    def input_names(self):
        return ["X0X FM-1"]

    output_names = input_names

    def open(self, i, o):
        return Link(self)

    # ---- backup: the stored image in 128-byte frames, then the end frame (bu_step)
    def stream(self, link):
        img = bytearray(self.image)
        if self.flags:                            # (playing: flagged in the head)
            img[8] = self.flags
            img[12:16] = struct.pack("<I", zlib.crc32(img[:12]))
            img[-4:] = struct.pack("<I", zlib.crc32(img[:-4]))
        for off in range(0, len(img), 128):
            link.q.put(dev_frame(B.BU_REQ, struct.pack("<I", off) + bytes(img[off:off + 128])))
        link.q.put(dev_frame(B.BU_END, struct.pack("<IIBB", len(img), zlib.crc32(img), 0, self.flags)))

    # ---- restore: restore_rx()'s reads
    def puller(self, n, crc):
        if self.playing:
            yield (B.FAIL_ADDR | B.PLAYING, 8)
            return
        got = bytearray((yield (0, 16)))
        pos = 16
        for _ in range(B.NOBJ):
            r = yield (pos, 8)
            got += r
            ln = struct.unpack_from("<H", r, 2)[0]
            pos += 8
            for k in range(0, ln, 512):
                got += (yield (pos + k, min(512, ln - k)))
            pos += ln
        got += (yield (pos, 4))
        if len(got) != n or zlib.crc32(got) != crc:
            yield (B.FAIL_ADDR | 6, 8)
            return
        try:
            B.parse(bytes(got))
        except B.BackupError:
            yield (B.FAIL_ADDR | 5, 8)
            return
        yield (B.ASK_ADDR, 8)
        self.asked += 1
        if not self.answer:
            yield (B.FAIL_ADDR | B.DECLINED, 8)
            return
        self.image = bytes(got)
        yield (B.OK_ADDR, 8)

    def ask(self, link, addr, n):
        q = bytes([0]) + addr.to_bytes(4, "little") + bytes([n & 0xFF, n >> 8, 0])
        link.q.put(b"\xF0" + I.pack7(bytes([0, 0x59, 0x30, 8, 0, 0]) + q + bytes([~sum(q) & 0xFF])) + b"\xF7")
        self.wanted = (addr, n)

    def next(self, link, data):
        try:
            addr, n = self.pull.send(data)
        except StopIteration:
            self.pull = None
            return
        self.ask(link, addr, n)

    def rx(self, link, d):
        if d == I.HS_QUERY:
            t = self.identity.encode()
            link.q.put(b"\xF0" + I.pack7(bytes([0, 0x59, 0x11, 0, 0, 0]) + t + bytes(28 - len(t))) + b"\xF7")
            return
        f = B.parse_frame(d)
        if f and f[0] == B.BU_REQ and f[1] == B.MAGIC:
            self.stream(link)
            return
        if f and f[0] == B.RS_REQ and len(f[1]) == 8:   # ota_take()'s checks passed; the request
            self.req = struct.unpack("<II", f[1])
            self.pull = self.puller(*self.req)
            self.next(link, None)
            return
        u = I.unpack7(d[1:-1])
        if self.pull and self.wanted and len(u) >= 15 and u[2] == 0x30:
            addr, n = self.wanted
            if int.from_bytes(u[7:11], "little") != addr or u[11] | u[12] << 8 != n:
                return
            self.reads += 1
            if self.stall_after is not None and self.reads > self.stall_after:
                return                            # (stops asking)
            self.next(link, bytes(u[14:14 + n]))


img = open(sys.argv[1], "rb").read()
flags, objs = B.parse(img)
ok(len(objs) == 6 and [o for o, _ in objs] == [0, 1, 2, 3, 5, 6], "the device's image (backup_test) parses: 6 parts")
tmp = tempfile.mkdtemp()
path = os.path.join(tmp, "x0x.x0b")

dev = FakeX0X(img)
out = io.StringIO()
rc = B.main(["backup", path], backend=dev, out=out)
ok(rc == 0 and open(path, "rb").read() == img and "done" in out.getvalue(), "backup: exit 0, the file byte for byte")
out = io.StringIO()
ok(B.main(["info", path], out=out) == 0 and "settings" in out.getvalue() and "motion" in out.getvalue(),
   "info: the file's parts")
out = io.StringIO()
dev = FakeX0X(img, flags=1)
rc = B.main(["backup", path + "2"], backend=dev, out=out)
ok(rc == 0 and "not saved" in out.getvalue(), "backup with changes not saved (playing): written, the note said")

other = B.build([(o, d[::-1] if d else b"x" * 10) for o, d in objs])   # another project on the device
dev = FakeX0X(other)
out = io.StringIO()
rc = B.main(["restore", path, "--yes"], backend=dev, out=out)
ok(rc == 0 and dev.image == img and "done" in out.getvalue(), "restore: exit 0, the device holds the backup")
ok(dev.req == (len(img), zlib.crc32(img)) and dev.asked == 1, "... the request: its size and CRC-32; asked once")
ok("SEL yes" in out.getvalue(), "... the host says to answer on the FM-1")

dev = FakeX0X(other, playing=True)
ok(B.main(["restore", path, "--yes"], backend=dev, out=io.StringIO()) == B.EXIT["playing"] and dev.image == other,
   "playing: STOP FIRST (exit 5), nothing changed")
dev = FakeX0X(other, answer=False)
ok(B.main(["restore", path, "--yes"], backend=dev, out=io.StringIO()) == 1 and dev.image == other and dev.asked == 1,
   "HOME on the FM-1: exit 1, nothing changed")
bad = bytearray(img)
bad[len(bad) // 2] ^= 1
open(path + ".bad", "wb").write(bad)
dev = FakeX0X(other)
ok(B.main(["restore", path + ".bad", "--yes"], backend=dev, out=io.StringIO()) == 2 and dev.req is None,
   "a damaged file: refused here (exit 2), nothing sent")
for what, magic, ver, want in (("another version (2)", b"X0XB", 2, "another version"),
                               ("Polly's backup", b"PLYB", 2, "Polly")):
    o = bytearray(img)                            # its CRCs right
    o[0:4] = magic
    struct.pack_into("<H", o, 4, ver)
    struct.pack_into("<I", o, 12, zlib.crc32(o[:12]))
    struct.pack_into("<I", o, len(o) - 4, zlib.crc32(o[:-4]))
    open(path + ".o", "wb").write(o)
    dev = FakeX0X(other)
    msg = io.StringIO()
    rc = B.main(["restore", path + ".o", "--yes"], backend=dev, out=msg)
    try:
        B.parse(bytes(o))
        said = ""
    except B.BackupError as e:
        said = str(e)
    ok(rc == 2 and dev.req is None and dev.image == other and want in said,
       f"{what}: refused here (exit 2), nothing sent: {said[:30]}")
ok(B.VERSION == 1 and struct.unpack_from("<H", img, 4)[0] == 1, "the device's backups: version 1")
dev = FakeX0X(other, identity="FM-1_3000401")
ok(B.main(["restore", path, "--yes"], backend=dev, out=io.StringIO()) == I.EXIT["model"] and dev.image == other,
   "not X0X (Polly's identity): refused")
dev = FakeX0X(other, stall_after=5)
ok(B.main(["restore", path, "--yes"], backend=dev, out=io.StringIO()) == I.EXIT["stopped"] and dev.image == other,
   "the FM-1 stops asking: exit 4, nothing changed")
dev = FakeX0X(other)
ok(B.main(["restore", path], backend=dev, out=io.StringIO(), ask=lambda p: False) == 1 and dev.req is None,
   "not confirmed here: cancelled, nothing sent")
shutil.rmtree(tmp)
print("x0x backup test %s" % ("FAILED" if failed else "passed"))
sys.exit(1 if failed else 0)
