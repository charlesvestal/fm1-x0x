#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Read and write the FM-1's registers over USB-MIDI, through a development build of X0X
(X0X_DEBUG=1 ./build.sh; the commands are in firmware/src/app/editor.c and refuse anything outside
a few register and RAM ranges). Needs mido with python-rtmidi.

  fm1_debug.py peek ADDR [N]          N words (<= 16) from ADDR
  fm1_debug.py poke ADDR VALUE        write a word, print it before and after
  fm1_debug.py clock [ITERATIONS]     time a fixed loop against the 24 MHz timer (IRQs off)
"""
import sys
import time

import mido

HDR = [0x7D, 0x46, 0x4C]
PEEK, POKE, CLOCK = 40, 41, 42


def w35(v):
    return [(v >> (7 * k)) & 0x7F for k in range(5)]


def r35(b, i):
    return sum(b[i + k] << (7 * k) for k in range(5)) & 0xFFFFFFFF


def ask(cmd, args, timeout=3.0):
    name = next((n for n in mido.get_output_names() if "FM-1" in n or "X0X" in n), None)
    if not name:
        sys.exit("fm1_debug: no FM-1 MIDI port")
    with mido.open_input(name) as i, mido.open_output(name) as o:
        while i.poll():
            pass
        o.send(mido.Message("sysex", data=HDR + [cmd] + args))
        end = time.time() + timeout
        while time.time() < end:
            m = i.poll()
            if m is None:
                time.sleep(0.002)
                continue
            if m.type == "sysex" and list(m.data[:4]) == HDR + [cmd]:
                return list(m.data[4:])
    sys.exit("fm1_debug: no reply (not a debug build, or an address outside the allowed ranges)")


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    if a[0] == "peek":
        addr, n = int(a[1], 0), int(a[2], 0) if len(a) > 2 else 1
        b = ask(PEEK, w35(addr) + w35(n))
        for k in range(n):
            print(f"{addr + 4 * k:08X}: {r35(b, 5 + 5 * k):08X}")
    elif a[0] == "poke":
        addr, v = int(a[1], 0), int(a[2], 0)
        b = ask(POKE, w35(addr) + w35(v))
        print(f"{addr:08X}: {r35(b, 5):08X} -> {r35(b, 10):08X}")
    elif a[0] == "clock":
        it = int(a[1], 0) if len(a) > 1 else 2000000
        b = ask(CLOCK, w35(it), timeout=10)
        ticks, c0, c1 = r35(b, 0), r35(b, 5), r35(b, 10)
        us = ticks / 24.0
        print(f"{it} iterations: {ticks} ticks = {us:.1f} us = {it / us:.2f} iterations per us;"
              f" C0_TL_CKCNT {c0:08X} -> {c1:08X} ({(c1 - c0) & 0xFFFFFFFF} counted"
              + (f", {((c1 - c0) & 0xFFFFFFFF) / us:.1f} MHz)" if c1 != c0 else ", not counting)"))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
