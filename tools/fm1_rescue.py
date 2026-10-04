#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""fm1_rescue: put stock firmware back on an FM-1 through the chip's own update mode (the
mask-ROM "UBOOT"), from a Mac, with no extra hardware.

For an FM-1 whose firmware crashes at start-up. X0X 0.2-beta crashes at once, and after two failed
boots enters UBOOT, which shows up on USB as "WL80UBOOT1.00" (4C4A:8057). macOS then takes it for a
disk, upsets it, and it reboots into the crash a few seconds later; this script waits for it,
takes it away from macOS (hence sudo) and keeps it.

  python3 -m venv ~/fm1-rescue && ~/fm1-rescue/bin/pip install pyusb libusb-package
  sudo ~/fm1-rescue/bin/python fm1_rescue.py FM-1.fwsc            # check and back up, write nothing
  sudo ~/fm1-rescue/bin/python fm1_rescue.py FM-1.fwsc --write    # then write stock

FM-1.fwsc is M-VAVE's FM-1 V15 (m-vave.com/download, PC Firmware); only that exact file is
accepted. The protocol and the write policy are FM-1-transporter's (github.com/kurogedelic/
FM-1-transporter, docs/PROTOCOL.md), which were verified on hardware:

  1. JieLi's flash loader (wl82loader.bin, from github.com/kagaimiq/jl-uboot-tool, downloaded
     and checked against its hash) is loaded into RAM and started;
  2. the chip key must be 980F and the flash 856014 (1 MiB), or nothing is done;
  3. the whole flash is read and saved as a backup before anything else;
  4. only 4 KiB sectors in [0x4000, 0x93000) that differ from V15 are written (the bootloader below
     0x4000 and the device data above 0x93000 are never touched; a difference below 0x4000 stops
     it), each erased, written and read back;
  5. a final full read must equal the expected image; then the FM-1 restarts into V15.
"""
import argparse
import hashlib
import os
import struct
import sys
import time
import urllib.request

VID, PID = 0x4C4A, 0x8057
STOCK_SHA256 = "db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a"
LOADER_URL = ("https://raw.githubusercontent.com/kagaimiq/jl-uboot-tool/"
              "c32b28e7a04f873f526ba929cb1bae9f05442fe9/data/loaderblobs/usb/wl82loader.bin")
LOADER_SHA256 = "d41da6126760c9d66660bcc0cac8d27d221806c5e369a8036921efe68dca5376"
LOADER_ADDR, LOADER_ARG = 0x1C02000, 0x0001

FLASH_SIZE = 0x100000
APP_END = 0x93000          # the .fwsc flash entry is raw flash [0, APP_END)
WRITE_MIN = 0x4000         # flash header, SPL, isd_config: never written
SECTOR = 0x1000
IO = 512

# ROM UBOOT1.00
CMD_WRITE_MEMORY, CMD_READ_MEMORY, CMD_JUMP = 0xFB06, 0xFD07, 0xFB08
# loader
CMD_ERASE_SECTOR, CMD_WRITE_FLASH, CMD_READ_FLASH = 0xFB01, 0xFB04, 0xFD05
CMD_READ_KEY, CMD_GET_ONLINE_DEVICE, CMD_RUN_APP = 0xFC09, 0xFC0A, 0xFC0C


class Fail(Exception):
    pass


# ------------------------------------------------------------------ maths ---
def crc16(data, crc=0):
    """CRC-16/XMODEM (poly 0x1021, no reflection)"""
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


MAGIC = bytes([0xC3, 0xCF, 0xC0, 0xE8, 0xCE, 0xD2, 0xB0, 0xAE,
               0xC4, 0xE3, 0xA3, 0xAC, 0xD3, 0xF1, 0xC1, 0xD6])


def crc_cipher(buf):
    """JieLi's CrcDecode cipher, key 0xFFFFFFFF (in place)"""
    crc = crc16(b"\xFF\xFF", 0xFFFF)
    for i in range(len(buf)):
        crc = crc16(MAGIC[i % 16:i % 16 + 1], crc)
        buf[i] ^= crc & 0xFF


def enc(buf, key=0xFFFF):
    """JieLi ENC stream cipher (in place)"""
    for i in range(len(buf)):
        buf[i] ^= key & 0xFF
        key = ((key << 1) ^ (0x1021 if key & 0x8000 else 0)) & 0xFFFF


# ---------------------------------------------------------------- package ---
def package_flash_image(raw):
    """The .fwsc's type-0 entry: raw flash [0, APP_END). Layout as tools/fm1pkg_make.py writes it:
    the logical image is the .fwsc without the marker byte after each of its first 20 0x2F-byte
    blocks; it starts with a 0x40-byte header and a list of 0x50-byte entries, each ENC'd."""
    logical = bytearray()
    for i in range(20):
        logical += raw[i * 0x30:i * 0x30 + 0x2F]
    logical += raw[20 * 0x30:]
    hdr = bytearray(logical[0:0x40])
    enc(hdr)
    n = struct.unpack_from("<H", hdr, 8)[0]
    if hdr[16:22] != b"AC791N" or not 0 < n < 16:
        raise Fail("not an FM-1 package (header)")
    for i in range(n):
        e = bytearray(logical[0x40 + i * 0x50:0x40 + (i + 1) * 0x50])
        enc(e)
        typ, _, dcrc, _, off, size = struct.unpack_from("<HHHHII", e, 0)
        if typ == 0:
            data = bytes(logical[off:off + size])
            if len(data) != size or crc16(data) != dcrc:
                raise Fail("package flash entry: bad CRC")
            return data
    raise Fail("package has no flash entry")


# -------------------------------------------------------------- transport ---
class Bot:
    """USB mass storage bulk-only transport: one SCSI-style command per call."""

    def __init__(self, dev, usb):
        self.dev, self.usb, self.tag = dev, usb, 1
        cfg = dev.get_active_configuration() if self._configured() else None
        if cfg is None:
            dev.set_configuration()
            cfg = dev.get_active_configuration()
        intf = cfg[(0, 0)]
        self.intf = intf.bInterfaceNumber
        util = usb.util
        self.ep_out = util.find_descriptor(intf, custom_match=lambda e: util.endpoint_direction(
            e.bEndpointAddress) == util.ENDPOINT_OUT and util.endpoint_type(e.bmAttributes) == util.ENDPOINT_TYPE_BULK)
        self.ep_in = util.find_descriptor(intf, custom_match=lambda e: util.endpoint_direction(
            e.bEndpointAddress) == util.ENDPOINT_IN and util.endpoint_type(e.bmAttributes) == util.ENDPOINT_TYPE_BULK)
        if self.ep_out is None or self.ep_in is None:
            raise Fail("no bulk endpoints")
        util.claim_interface(dev, self.intf)

    def _configured(self):
        try:
            self.dev.get_active_configuration()
            return True
        except self.usb.core.USBError:
            return False

    def reset_recovery(self):
        """Bulk-only mass storage reset, then clear both halts (what a host does after a stall)."""
        try:
            self.dev.ctrl_transfer(0x21, 0xFF, 0, self.intf, None, timeout=2000)
        except self.usb.core.USBError:
            pass
        for ep in (self.ep_in, self.ep_out):
            try:
                self.dev.clear_halt(ep)
            except self.usb.core.USBError:
                pass

    def xfer(self, cdb, data_out=None, n_in=0, timeout=5000):
        tag = self.tag
        self.tag = (self.tag + 1) & 0xFFFFFFFF
        length = len(data_out) if data_out is not None else n_in
        flags = 0x80 if n_in else 0x00
        cbw = struct.pack("<4sIIBBB", b"USBC", tag, length, flags, 0, len(cdb)) + bytes(cdb).ljust(16, b"\0")
        self.ep_out.write(cbw, timeout)
        got = b""
        if data_out is not None and len(data_out):
            self.ep_out.write(bytes(data_out), timeout)
        elif n_in:
            got = bytes(self.ep_in.read(n_in, timeout))
        csw = bytes(self.ep_in.read(13, timeout))
        sig, ctag, _, status = struct.unpack("<4sIIB", csw)
        if sig != b"USBS" or ctag != tag:
            raise Fail(f"bad status block ({csw.hex()})")
        if status != 0:
            raise Fail(f"command failed (status {status})")
        return got


def cdb_for(cmd, args=b""):
    return (struct.pack(">H", cmd) + bytes(args)).ljust(16, b"\xFF")


# ----------------------------------------------------------------- device ---
class Uboot:
    def __init__(self, bot):
        self.bot = bot

    def inquiry(self):
        r = self.bot.xfer(bytes([0x12, 0, 0, 0, 36, 0]), n_in=36)
        return r[8:16].decode(errors="replace").strip(), r[16:32].decode(errors="replace").strip()

    def cmd(self, cmd, args=b""):
        r = self.bot.xfer(cdb_for(cmd, args), n_in=16)
        if struct.unpack(">H", r[0:2])[0] != cmd:
            raise Fail(f"command {cmd:04X} answered as {r[0:2].hex()}")
        return r[2:]

    def write_block(self, cmd, addr, data):
        args = struct.pack(">IH", addr, len(data)) + b"\0" + struct.pack("<H", crc16(data))
        self.bot.xfer(cdb_for(cmd, args), data_out=data)

    def start_loader(self, blob):
        for off in range(0, len(blob), IO):                  # shipped already ciphered: send raw
            self.write_block(CMD_WRITE_MEMORY, LOADER_ADDR + off, blob[off:off + IO])
        self.cmd(CMD_JUMP, struct.pack(">IH", LOADER_ADDR, LOADER_ARG))

    def chip(self):
        p = self.cmd(CMD_READ_KEY, struct.pack(">I", 0x00AC6900))
        k = bytearray([p[5], p[4]])
        crc_cipher(k)
        key = k[0] | k[1] << 8
        p = self.cmd(CMD_GET_ONLINE_DEVICE)
        return key, p[0], struct.unpack_from("<I", p, 2)[0]

    def read(self, addr, n):
        out = bytearray()
        while len(out) < n:
            k = min(IO, n - len(out))
            out += self.bot.xfer(cdb_for(CMD_READ_FLASH, struct.pack(">IH", addr + len(out), k)), n_in=k)
        return bytes(out)

    def write_sector(self, addr, data):
        if addr < WRITE_MIN or addr >= APP_END or addr % SECTOR or len(data) != SECTOR:
            raise Fail(f"refusing to write {addr:#x}")
        self.cmd(CMD_ERASE_SECTOR, struct.pack(">I", addr))
        for off in range(0, SECTOR, IO):
            self.write_block(CMD_WRITE_FLASH, addr + off, data[off:off + IO])
        if self.read(addr, SECTOR) != data:
            raise Fail(f"sector {addr:#07x} did not verify")

    def run_app(self):
        try:
            self.cmd(CMD_RUN_APP, struct.pack(">I", 1))
        except Exception:
            pass                                             # it may reset before it answers


# ------------------------------------------------------------------- flow ---
def rescue(dev, image, loader, write, backup_dir, say=print):
    """Everything in one session: the FM-1 leaves UBOOT (and crashes again) as soon as it is let go."""
    vendor, product = dev.inquiry()
    say(f"  device: {vendor} {product}")
    if vendor != "WL82":
        raise Fail(f"not a WL82 UBOOT ({vendor!r})")
    dev.start_loader(loader)
    key, typ, fid = dev.chip()
    say(f"  chip key {key:04X}, flash type {typ}, id {fid:06X}")
    if (key, typ, fid) != (0x980F, 3, 0x856014):
        raise Fail("this is not the FM-1's chip and flash (expected key 980F, type 3, id 856014); nothing written")
    say("  reading the whole flash ...")
    t0 = time.time()
    now = dev.read(0, FLASH_SIZE)
    path = os.path.join(backup_dir, time.strftime("fm1-backup-%Y%m%d-%H%M%S.bin"))
    with open(path, "wb") as f:
        f.write(now)
    say(f"  backup saved: {path} ({time.time() - t0:.1f} s)")
    diff = [a for a in range(0, APP_END, SECTOR) if image[a:a + SECTOR] != now[a:a + SECTOR]]
    low = [a for a in diff if a < WRITE_MIN]
    if low:
        raise Fail("the protected head of the flash differs from V15 (sectors "
                   + " ".join(f"{a:#06x}" for a in low) + "): not writing; send this output and the backup")
    say(f"  {len(diff)} of {APP_END // SECTOR} firmware sectors differ from V15")
    if not write:
        say("  check passed. Nothing written: run again with --write to put V15 back.")
        return False
    # high to low: X0X's start-up code (low in the app area: its watchdog and its crash counter,
    # which bring it back to update mode) is the last thing replaced if the write is cut short
    for i, a in enumerate(reversed(diff)):
        for attempt in range(3):
            try:
                dev.write_sector(a, image[a:a + SECTOR])
                break
            except Fail as e:
                if attempt == 2:
                    raise Fail(f"{e}; the FM-1 is still in update mode: run the script again")
        say(f"\r  writing {i + 1}/{len(diff)}", end="")
    say("")
    say("  final read ...")
    expect = image + now[APP_END:]
    final = dev.read(0, FLASH_SIZE)
    if final != expect:
        bad = [a for a in range(0, FLASH_SIZE, SECTOR) if final[a:a + SECTOR] != expect[a:a + SECTOR]]
        raise Fail("final read differs in sectors " + " ".join(f"{a:#07x}" for a in bad)
                   + "; run the script again with --write")
    say("  flash now holds V15. Restarting the FM-1 ...")
    dev.run_app()
    return True


def get_loader(path):
    if path:
        blob = open(path, "rb").read()
    else:
        print("downloading JieLi's flash loader (wl82loader.bin) ...")
        with urllib.request.urlopen(LOADER_URL, timeout=30) as r:
            blob = r.read()
    if hashlib.sha256(blob).hexdigest() != LOADER_SHA256:
        raise Fail("wl82loader.bin: unexpected contents")
    return blob


def backend(usb):
    """libusb: pip's libusb-package (no Homebrew needed), else the system's or Homebrew's"""
    try:
        import libusb_package
        b = libusb_package.get_libusb1_backend()
        if b is not None:
            return b
    except ImportError:
        pass
    import usb.backend.libusb1 as lb
    for path in (None, "/opt/homebrew/lib/libusb-1.0.dylib", "/usr/local/lib/libusb-1.0.dylib"):
        b = lb.get_backend() if path is None else (lb.get_backend(find_library=lambda _: path)
                                                   if os.path.exists(path) else None)
        if b is not None:
            return b
    raise Fail("libusb not found: pip install libusb-package")


def open_device(usb, timeout):
    """Wait for 4C4A:8057 and take it from macOS's mass storage driver."""
    print("waiting for the FM-1 in update mode (switch it on, or wait for it to cycle) ...")
    be = backend(usb)
    t0 = time.time()
    while time.time() - t0 < timeout:
        dev = usb.core.find(idVendor=VID, idProduct=PID, backend=be)
        if dev is not None:
            print(f"found it after {time.time() - t0:.1f} s")
            try:
                try:
                    if dev.is_kernel_driver_active(0):
                        dev.detach_kernel_driver(0)
                except (NotImplementedError, usb.core.USBError) as e:
                    if getattr(e, "errno", None) not in (None, 2):
                        print(f"  (could not detach macOS's driver: {e})")
                bot = Bot(dev, usb)
                try:
                    Uboot(bot).inquiry()
                except Exception:
                    bot.reset_recovery()
                return bot
            except Exception as e:
                print(f"  could not open it ({e}); waiting for the next cycle")
                time.sleep(1.0)
                continue
        time.sleep(0.02)
    raise Fail("the FM-1 did not appear in update mode")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("package", help="M-VAVE's FM-1 V15 .fwsc")
    ap.add_argument("--write", action="store_true", help="write V15 (without it: check and back up only)")
    ap.add_argument("--loader", help="a local wl82loader.bin instead of downloading it")
    ap.add_argument("--wait", type=float, default=120, help="seconds to wait for the FM-1 (default 120)")
    ap.add_argument("--tries", type=int, default=5, help="attempts if it drops out mid-way (default 5)")
    args = ap.parse_args()
    try:
        raw = open(args.package, "rb").read()
        if hashlib.sha256(raw).hexdigest() != STOCK_SHA256:
            raise Fail(f"{args.package} is not M-VAVE's FM-1 V15 package (sha256 mismatch)")
        image = package_flash_image(raw)
        if len(image) != APP_END:
            raise Fail("unexpected flash entry size")
        loader = get_loader(args.loader)
        if os.geteuid() != 0:
            raise Fail("run it with sudo: macOS has to give the device up")
        try:
            import usb.core
            import usb.util
        except ImportError:
            raise Fail("pyusb is missing: ~/fm1-rescue/bin/pip install pyusb libusb-package")
        backup_dir = os.path.dirname(os.path.abspath(args.package))
        for attempt in range(1, args.tries + 1):
            bot = open_device(usb, args.wait)
            try:
                rescue(Uboot(bot), image, loader, args.write, backup_dir,
                       say=lambda *a, **k: print(*a, **k, flush=True))
                return 0
            except (Fail, usb.core.USBError) as e:
                print(f"\n  attempt {attempt}: {e}")
                if isinstance(e, Fail) and "not writing" in str(e) or "not the FM-1" in str(e):
                    return 1
            finally:
                try:
                    usb.util.dispose_resources(bot.dev)
                except Exception:
                    pass
        raise Fail("gave up; send this whole output")
    except Fail as e:
        print(f"fm1_rescue: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
