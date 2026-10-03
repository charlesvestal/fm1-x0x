#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Synthetic one-bar test loops (no recordings): 8 tone bursts each, a different pitch set per
loop, so a slice is identifiable. Used by the upload scenario.
  make_test_loops.py OUTDIR"""
import math
import struct
import sys
from pathlib import Path

out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
sr, bpm = 44100, 110
n = int(sr * 240 / bpm)                            # one bar
for k, base in enumerate((220, 330, 440, 550)):
    x = []
    for i in range(n):
        e, t = divmod(i, n // 8)
        f = base * (1 + e / 8)
        x.append(0.8 * math.sin(2 * math.pi * f * t / sr) * math.exp(-t / (sr * 0.04)))
    pcm = b"".join(struct.pack("<h", int(v * 32767)) for v in x)
    (out / f"loop{k + 1}.wav").write_bytes(b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " +
                                           struct.pack("<IHHIIHH", 16, 1, 1, sr, sr * 2, 2, 16) + b"data" +
                                           struct.pack("<I", len(pcm)) + pcm)
print(f"4 test loops in {out}")
