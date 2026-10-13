#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Measure the factory mix against a big-beat reference (Chemical Brothers, Fatboy Slim): a funk
break up front, a 909 kick under it, clap on 2 and 4, open hats on the off-beats, a 303 line.
Played through the simulator with the factory sound, rendered whole and part by part (the 909
voice by voice), then each render's loudness, peak, crest and tonal balance.

  tools/mix_report.py [OUTDIR]          (default build/mix; needs build/host/x0x_host)

The break is a real one, for measuring only: build/breaks/ (tools/import_breaks.py from BB Gen's
samples) loaded into user slot 1 as the simulator would after tools/upload_breaks.py. Without
those files the built-in 909 loop plays instead. The 808 plays the same groove for its own solo.
303 A plays its factory TB-3PO line (303 B its own, solo only). Default tempo.

Loudness is ITU-R BS.1770 (K-weighted, ungated over the loop): LU, comparable between the
renders. Peaks are against the codec's full scale (the output is limited at -6 dBFS, Felucca's
ceiling). Crest = peak - RMS: a squashed big-beat mix sits near 8-10 dB. Bands are the share of
the render's energy, in dB."""
import subprocess
import sys
import wave
from pathlib import Path

import numpy as np
from scipy import signal

SRC = Path(__file__).resolve().parents[1]
HOST = SRC / "build" / "host" / "x0x_host"
FS = 44100
PARTS = ["909", "808", "303A", "303B", "BREAK"]


def white(ws):
    return [f"tapkey w{w}" for w in ws]


def voice(b, steps):                  # select drum track b, toggle its steps
    return [f"tapkey b{b}"] + white(steps)


BREAKS = [SRC / "build" / "breaks" / f"{n}.wav" for n in ("THINK", "FUNKY")]


def groove_909(voices=None):
    v = {"BD": voice(0, [0, 4, 8, 12]), "CP": voice(6, [4, 12]), "OH": voice(8, [2, 6, 10, 14])}
    out = []
    for k in (voices or v):
        out += v[k]
    return out + ["press ENV"] + white([0, 8]) + ["release ENV"]


def groove_808():
    return (["turn ALGO 1"] + voice(0, [0, 4, 8, 12]) + voice(6, [4, 12]) + voice(10, [0, 4, 8, 12])
            + voice(9, [2, 6, 10, 14]) + voice(7, [3, 11]) + ["turn ALGO -1"])


def break_on():
    s = ["turn ALGO 4"] + white(range(16))
    if all(b.exists() for b in BREAKS):               # the real breaks: loops A and B
        s += ["turn SELECT 3", "turn K1 2", "turn K2 2", "turn SELECT -3"]
    return s + ["turn ALGO -4"]


def script(wav, voices=None, play=("909", "303A", "BREAK"), bars=8, raw=False):
    s = ["wait 300"]
    if raw:                           # the balance before the master: its compressor and PUMP off
        s += ["param 6 0 1 0", "param 6 0 6 0"]
    if all(b.exists() for b in BREAKS):
        s.append("slot 0 BRK " + " ".join(str(b) for b in BREAKS))
    s += groove_909(voices) + groove_808() + break_on() + ["tap HOME"]
    for i, p in enumerate(PARTS):     # every part not playing: muted on HOME
        if p not in play:
            s.append(f"tapkey b{i}")
    ms = int(60000 / 125 * 4)
    s += ["tap PLAY", f"wait {ms}", f"wav {wav}", f"wait {ms * bars}", "wavstop", "tap PLAY"]
    return "\n".join(s) + "\n"


def k_weight(x):
    """BS.1770 K-weighting at 44.1 kHz (the two biquads, from their analogue prototypes)."""
    import math
    f0, G, Q = 1681.974450955533, 3.999843853973347, 0.7071752369554196
    K = math.tan(math.pi * f0 / FS)
    Vh, Vb = 10 ** (G / 20), 10 ** (G / 20) ** 0.4996667741545416
    a0 = 1 + K / Q + K * K
    b1 = [(Vh + Vb * K / Q + K * K) / a0, 2 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0]
    a1 = [1, 2 * (K * K - 1) / a0, (1 - K / Q + K * K) / a0]
    f0, Q = 38.13547087602444, 0.5003270373238773
    K = math.tan(math.pi * f0 / FS)
    a2 = [1, 2 * (K * K - 1) / (1 + K / Q + K * K), (1 - K / Q + K * K) / (1 + K / Q + K * K)]
    b2 = [1, -2, 1]
    return signal.lfilter(b2, a2, signal.lfilter(b1, a1, x))


def analyse(path):
    with wave.open(str(path)) as w:
        x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64)
        x = np.asarray(x.reshape(-1, w.getnchannels())[:, 0]) / 32768.0
    if not len(x) or not np.any(x):
        return None
    lu = -0.691 + 10 * np.log10(np.mean(k_weight(x) ** 2) + 1e-20)
    peak = 20 * np.log10(np.max(np.abs(x)) + 1e-20)
    rms = 20 * np.log10(np.sqrt(np.mean(x ** 2)) + 1e-20)
    f, p = signal.welch(x, FS, nperseg=8192)
    tot = np.sum(p)
    bands = {}
    for name, lo, hi in (("sub", 20, 60), ("bass", 60, 200), ("lowmid", 200, 800), ("mid", 800, 3000),
                         ("high", 3000, 8000), ("air", 8000, 20000)):
        m = (f >= lo) & (f < hi)
        bands[name] = 10 * np.log10(np.sum(p[m]) / tot + 1e-20)
    return {"lu": lu, "peak": peak, "rms": rms, "crest": peak - rms, "bands": bands}


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else SRC / "build" / "mix"
    out.mkdir(parents=True, exist_ok=True)
    jobs = {"MIX": dict(), "MIX + 808, 303B": dict(play=PARTS), "MIX, raw": dict(raw=True),
            "BREAK": dict(play=("BREAK",), raw=True), "909": dict(play=("909",), raw=True),
            "303A": dict(play=("303A",), raw=True), "303B": dict(play=("303B",), raw=True),
            "808": dict(play=("808",), raw=True)}
    for v in ("BD", "CP", "OH"):
        jobs[f"909 {v}"] = dict(play=("909",), voices=[v], raw=True)
    res = {}
    for name, kw in jobs.items():
        tag = name.replace(" ", "_").replace("+", "").replace(",", "")
        wav = out / f"{tag}.wav"
        sc = out / f"{tag}.x0x"
        sc.write_text(script(wav.name, **kw))
        r = subprocess.run([str(HOST), str(sc), str(out)], capture_output=True, text=True)
        if r.returncode:
            print(r.stdout[-2000:])
            raise SystemExit(f"{name}: the simulator failed")
        res[name] = analyse(wav)
    ref = res["MIX, raw"]["lu"]
    print("the parts are measured before the master's compressor ('raw'): vs MIX is against the raw mix")
    print(f"{'render':16} {'LU':>7} {'vs MIX':>6} {'peak':>6} {'crest':>6}   bands: sub bass lowmid mid high air (dB of the total)")
    for name, a in res.items():
        if not a:
            print(f"{name:16} silent")
            continue
        b = " ".join(f"{a['bands'][k]:5.1f}" for k in ("sub", "bass", "lowmid", "mid", "high", "air"))
        print(f"{name:16} {a['lu']:7.1f} {a['lu'] - ref:+6.1f} {a['peak']:6.1f} {a['crest']:6.1f}   {b}")
    print(f"renders in {out}")


if __name__ == "__main__":
    main()
