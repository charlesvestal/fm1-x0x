#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Fit the ROOM reverb's knob mapping (firmware/src/dsp/fxbus.c, fx_room_coeffs) to CLASSIC, so
that the two types decay alike and are as loud at the same RvDec and RvTone. Both are rendered
by this firmware's own fxbus on the host (tools/fit_room_render.c) on a 20 ms noise burst at a
snare's send level.

  tools/fit_room.py            fit, then print the C for fx_room_coeffs and the fit's errors
  tools/fit_room.py --check    the firmware's mapping as it is, against CLASSIC

At each setting of a grid of RvDec and RvTone:
  - CLASSIC's decay time in 10 mel-spaced bands (100 Hz .. 10 kHz): the time from -3 to -23 dB of
    the band's backward-integrated energy, times 3. CLASSIC's decay depends on level (its 12-bit
    loop truncates small values), which is why the burst is at a send's level, not an impulse.
  - The decay times at DC and Nyquist (t_dc, t_ny) that make ROOM's predicted decay in each band
    (its lines' loop gain and damper at the band's centre, averaged over the lines) closest to
    CLASSIC's, in log time.
  - ROOM rendered at those times: the output gain that matches its K-weighted (BS.1770) level to
    CLASSIC's.
Then ln t_dc, ln t_ny and the gain are each fitted over the grid by least squares, with the
polynomial terms fx_room_coeffs uses.

Needs numpy and scipy (pip3 install numpy scipy) and a host C compiler ($CC, default cc). Takes
about 5 s."""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

try:
    import numpy as np
    from scipy.optimize import least_squares
    from scipy.signal import butter, lfilter, sosfilt
except ImportError:
    sys.exit("fit_room.py needs numpy and scipy: pip3 install numpy scipy")

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "build/host/fit_room_render"
FS = 44100
DECS = [0, 16, 32, 48, 64, 73, 80, 96, 112, 120, 127]
TONES = [0, 15, 32, 48, 57, 72, 88, 100, 115, 127]
BURST = 882                                 # the burst's length, as fit_room_render.c's

mel = lambda f: 2595 * np.log10(1 + f / 700)
imel = lambda m: 700 * (10 ** (m / 2595) - 1)
EDGES = imel(np.linspace(mel(100), mel(10000), 11))
BANDS = list(zip(EDGES[:-1], EDGES[1:]))
CENTRES = np.array([np.sqrt(a * b) for a, b in BANDS])
SOS = [butter(4, b, "band", fs=FS, output="sos") for b in BANDS]

# The terms of each fit, as fx_room_coeffs writes them (x = RvDec / 127, y = RvTone / 127).
TERMS = {
    "t_dc": ["1", "x", "x*x", "x*x*x", "y"],
    "t_ny": ["1", "x", "y", "x*y", "y*y", "x*x"],
    "gain": ["1", "x", "y"],
}


def lengths():
    """the lines' lengths, from fxbus.h"""
    h = (ROOT / "firmware/src/dsp/fxbus.h").read_text()
    return np.array([int(re.search(rf"#define FX_RM_L{j}\s+(\d+)", h).group(1)) for j in range(8)])


def build():
    cc = os.environ.get("CC", "cc")
    EXE.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([cc, "-O2", "-ffp-contract=off", "-w", f"-I{ROOT}/firmware/src", f"-I{ROOT}/firmware/src/dsp",
                    f"-I{ROOT}/build/gen", "-o", str(EXE), str(ROOT / "tools/fit_room_render.c"), "-lm"], check=True)


def render(*args):
    out = subprocess.run([str(EXE)] + [str(a) for a in args], capture_output=True, check=True).stdout
    n = int(np.frombuffer(out[:4], np.int32)[0])
    x = np.frombuffer(out[4:], np.float32).astype(float)
    return x[:n], x[n:2 * n]


def band_t60(x, start=0.01):
    """decay time per band, s: from -3 to -23 dB of the backward-integrated energy, times 3"""
    t = []
    for sos in SOS:
        h = sosfilt(sos, x)[BURST + int(start * FS):]
        e = np.cumsum(h[::-1] ** 2)[::-1]
        e = 10 * np.log10(e / e[0] + 1e-30)
        t.append(3 * (np.argmax(e < -23) - np.argmax(e < -3)) / FS)
    return np.array(t)


def t60_lr(l, r):
    return (band_t60(l) + band_t60(r)) / 2


def dampers(lens, t_dc, t_ny):
    """each line's loop gain and damper pole, as fx_room_times sets them: g from t_dc; the pole matching, at
    3 kHz, the decay of a mean-length reference line whose damper t_ny sets at Nyquist"""
    lref = np.mean(lens)
    sref = lref / FS
    gn = min(10 ** (-3 * sref / t_ny + 3 * sref / t_dc), 1.0)
    ar = (1 - gn) / (1 + gn)
    cm = np.cos(2 * np.pi * 3000 / FS)
    q2 = (1 - ar) ** 2 / (1 - 2 * ar * cm + ar * ar)
    out = []
    for L in lens:
        g = 10 ** (-3 * (L / FS) / t_dc)
        r = q2 ** (L / lref)
        qa, qb = 1 - r, 1 - r * cm
        out.append((g, qa / (qb + np.sqrt(qb * qb - qa * qa))))
    return out


def predicted_t60(lens, t_dc, t_ny):
    """ROOM's decay time per band from its lines' loop gain and damper (fx_room_times), s"""
    w = 2 * np.pi * CENTRES / FS
    rates = []
    for L, (g, a) in zip(lens, dampers(lens, t_dc, t_ny)):
        rates.append(-20 * np.log10(g * (1 - a) / np.sqrt(1 - 2 * a * np.cos(w) + a * a)) / (L / FS))   # dB / s
    return 60 / np.mean(rates, axis=0)


def k_weight(x):
    """ITU-R BS.1770 K-weighting at 44.1 kHz: the two biquads, from their analogue prototypes"""
    f0, G, Q = 1681.974450955533, 3.999843853973347, 0.7071752369554196
    K = np.tan(np.pi * f0 / FS)
    Vh = 10 ** (G / 20)
    Vb = Vh ** 0.4996667741545416
    a0 = 1 + K / Q + K * K
    b1 = [(Vh + Vb * K / Q + K * K) / a0, 2 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0]
    a1 = [1, 2 * (K * K - 1) / a0, (1 - K / Q + K * K) / a0]
    f0, Q = 38.13547087602444, 0.5003270373238773
    K = np.tan(np.pi * f0 / FS)
    a2 = [1, 2 * (K * K - 1) / (1 + K / Q + K * K), (1 - K / Q + K * K) / (1 + K / Q + K * K)]
    return lfilter([1, -2, 1], a2, lfilter(b1, a1, x))


def loudness(l, r):
    return 10 * np.log10(np.mean(k_weight(l) ** 2) + np.mean(k_weight(r) ** 2))


def design(terms, x, y):
    return np.stack([eval(t, {}, {"x": x, "y": y}) * np.ones_like(x) for t in terms], 1)


def report(name, t60, ref, db):
    err = (t60 / ref - 1) * 100
    print(f"{name}: decay per band against CLASSIC {np.sqrt(np.mean(err ** 2)):.1f} % rms, "
          f"max {np.max(np.abs(err)):.1f} %; level {np.sqrt(np.mean(db ** 2)):.2f} dB rms, max {np.max(np.abs(db)):.2f} dB")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--check", action="store_true", help="the firmware's mapping against CLASSIC, no fit")
    a = ap.parse_args()
    build()
    lens = lengths()
    print("lines", " ".join(map(str, lens)), "; bands (Hz)", " ".join(f"{f:.0f}" for f in CENTRES))
    pts, ref, fdn, db = [], [], [], []
    for dec in DECS:
        for tone in TONES:
            cl, cr = render("classic", dec, tone)
            target = t60_lr(cl, cr)
            if a.check:
                rl, rr = render("room", dec, tone)
                fdn.append(t60_lr(rl, rr))
                db.append(loudness(rl, rr) - loudness(cl, cr))
            else:
                fit = least_squares(lambda p: np.log(predicted_t60(lens, np.exp(p[0]), np.exp(p[0] + p[1]))) - np.log(target),
                                    [np.log(np.median(target)), np.log(0.5)], bounds=([-5, -6], [3, 0]))
                t_dc, t_ny = np.exp(fit.x[0]), np.exp(fit.x[0] + fit.x[1])
                rl, rr = render("room", dec, tone, t_dc, t_ny, 1.0)
                gain = 10 ** ((loudness(cl, cr) - loudness(rl, rr)) / 20)
                pts.append((dec / 127, tone / 127, t_dc, t_ny, gain))
            ref.append(target)
            print(".", end="", flush=True, file=sys.stderr)
    print(file=sys.stderr)
    ref = np.array(ref)
    if a.check:
        report("firmware", np.array(fdn), ref, np.array(db))
        return
    x, y, t_dc, t_ny, gain = (np.array(c) for c in zip(*pts))
    coef = {}
    for name, target, log in (("t_dc", t_dc, True), ("t_ny", t_ny, True), ("gain", gain, False)):
        A = design(TERMS[name], x, y)
        c, *_ = np.linalg.lstsq(A, np.log(target) if log else target, rcond=None)
        pred = np.exp(A @ c) if log else A @ c
        err = (pred / target - 1) * 100
        print(f"{name}: polynomial against the grid's fits {np.sqrt(np.mean(err ** 2)):.1f} % rms, max {np.max(np.abs(err)):.1f} %")
        coef[name] = c
    d, n, g = coef["t_dc"], coef["t_ny"], coef["gain"]
    s = lambda v: f" {'-' if v < 0 else '+'} {abs(v):.4f}f"      # a term's sign as C writes it
    print("\n/* fx_room_coeffs (tools/fit_room.py) */")
    print(f"    const float t_dc = fm_expf({d[0]:.4f}f + x * ({d[1]:.4f}f + x * ({d[2]:.4f}f + x * {d[3]:.4f}f)){s(d[4])} * y);")
    print(f"    const float t_ny = fm_minf(t_dc, fm_expf({n[0]:.4f}f{s(n[1])} * x{s(n[2])} * y{s(n[3])} * x * y")
    print(f"                                            {s(n[4])} * y * y{s(n[5])} * x * x));")
    print(f"    f->rm_out = {g[0]:.4f}f{s(g[1])} * x{s(g[2])} * y;")

if __name__ == "__main__":
    main()
