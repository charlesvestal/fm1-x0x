#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Fit the RAT part of the 303 drive's migration (firmware/src/dsp/bass303.c, bass303_pre_rescale):
the new Drive and Volume that sound most like an old RAT setting, for projects saved before the
drive was rescaled. Both 303s are rendered on the host by tools/drive_render.c, built against this
firmware's bass303.c and against the one from before the rescaling (bass303.c and the headers it
includes, all from git show OLD_REV, so that a later change to fastmath.h can't change "before"),
playing one acid line through each of its patches (tools/drive_line.h: dark and resonant, bright,
mellow, squelchy, a closed square), since how a drive setting sounds depends on what reaches it.

  tools/fit_drive_migration.py           fit, then print the C tables and the fit's errors
  tools/fit_drive_migration.py --check   the firmware's mapping as it is, for RAT and for Soft (whose
                                         mapping is computed, not fitted: bass303_pre_rescale)

For every patch and every old RAT setting (Volume v, Drive d > 0):
  - the new Drive whose sound has the closest spectral shape: third-octave band levels, 31.5 Hz ..
    16 kHz, their difference weighted by the old sound's K-weighted (BS.1770) band loudness, the
    overall level difference removed (Volume takes care of that);
  - the new Volume that matches its K-weighted loudness. The new 303 applies Volume after the drive,
    so each new Drive is rendered once, at Volume 96, and the Volume follows from the level
    difference (60/127 dB a pot step).
The tables hold the new Drive and Volume at a grid of nodes (Volume every 8, Drive denser at its
low end, where the old RAT's gain rises fastest), interpolated bilinearly between them. The Drive
table is monotone (no lower for more old Drive or Volume), so that a lane sweeping either pot moves
the new Drive one way, and smooth: it is fitted a row (an old Volume) at a time, loudest first, the
whole row at once, as the nondecreasing Drives, none above the row before's, with the least shape
error (the mean over the patches and the settings around each node) plus SMOOTH times the change in
sound the migration adds between neighbouring nodes: how far apart the two new settings sound
(shape distance), less how far apart the two old ones did. So where the old knob hardly changed
the sound, nor does the new mapping; choosing each node's best Drive on its own instead makes
cliffs where several new Drives fit about as well (old Volume ~24 to 40), the new Drive jumping
from mild to full distortion between old settings that sounded alike. A node's Volume is the
midpoint of the ones the patches need to match the level (the least worst error). Below old Volume
~32 the old RAT hardly clips and no new setting matches it closely.

Needs numpy and scipy (pip3 install numpy scipy), git, and a host C compiler ($CC, default cc).
Takes about a minute a patch and type (it renders every old setting: 128 x 127); the reports are
per patch."""
import argparse
import os
import re
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

try:
    import numpy as np
    from scipy.signal import lfilter, welch
except ImportError:
    sys.exit("fit_drive_migration.py needs numpy and scipy: pip3 install numpy scipy")

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "build/host"
EXE_NEW, EXE_OLD = OUT / "drive_render", OUT / "drive_render_old"
OLD_REV = "54194c5"                         # upstream main before the drive was rescaled
FS = 44100
SOFT, RAT = 1, 2
STEP_DB = 60.0 / 127.0                       # a Volume pot step (amp_scaler: -60 + 60 n dB)
VNODES = [0, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96, 104, 112, 120, 127]
DNODES = [1, 2, 4, 8, 12, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96, 104, 112, 120, 127]
NPS = 16384                                  # Welch segments: 2.7 Hz bins, so every band has some
SMOOTH = 2.0                                 # the fit's weight on the change in sound it adds


def kweight(x):
    """Filter x with the ITU-R BS.1770 K-weighting: a high shelf (+4 dB, 1682 Hz), then a 38 Hz
    highpass."""
    a_ = 10 ** (4 / 40); w = 2 * np.pi * 1681.97 / FS; c = np.cos(w)
    al = np.sin(w) / 2 * np.sqrt((a_ + 1 / a_) * (1 / 0.7072 - 1) + 2)
    b = [a_ * ((a_ + 1) + (a_ - 1) * c + 2 * np.sqrt(a_) * al), -2 * a_ * ((a_ - 1) + (a_ + 1) * c),
         a_ * ((a_ + 1) + (a_ - 1) * c - 2 * np.sqrt(a_) * al)]
    a = [(a_ + 1) - (a_ - 1) * c + 2 * np.sqrt(a_) * al, 2 * ((a_ - 1) - (a_ + 1) * c),
         (a_ + 1) - (a_ - 1) * c - 2 * np.sqrt(a_) * al]
    y = lfilter(b, a, x)
    w = 2 * np.pi * 38.13 / FS; c = np.cos(w); al = np.sin(w) / (2 * 0.5003)
    return lfilter([(1 + c) / 2, -(1 + c), (1 + c) / 2], [1 + al, -2 * c, 1 - al], y)


F, _ = welch(np.zeros(NPS * 2), FS, nperseg=NPS)
CENTRES = [c for c in 1000.0 * 2.0 ** (np.arange(-15, 15) / 3.0)
           if np.any((F >= c * 2 ** (-1 / 6)) & (F < c * 2 ** (1 / 6)))]
BAND = [(F >= c * 2 ** (-1 / 6)) & (F < c * 2 ** (1 / 6)) for c in CENTRES]
_, KP = welch(kweight(np.random.default_rng(0).standard_normal(FS * 20)), FS, nperseg=NPS)
KBAND = np.array([KP[m].mean() for m in BAND]) * (FS / 2)      # K-weighting's power gain per band


def build():
    cc = os.environ.get("CC", "cc")
    old = OUT / "drive_old"
    old.mkdir(parents=True, exist_ok=True)
    for f in ("bass303.c", "bass303.h", "fastmath.h", "x0x_param.h"):
        src = subprocess.run(["git", "-C", str(ROOT), "show", f"{OLD_REV}:firmware/src/dsp/{f}"],
                             capture_output=True, check=True).stdout
        (old / f).write_bytes(src)
    flags = ["-O2", "-ffp-contract=off", "-w", "-DX0X_HOST"]
    inc = [f"-I{ROOT}/firmware/src", f"-I{ROOT}/firmware/src/dsp", f"-I{ROOT}/build/gen"]
    subprocess.run([cc] + flags + ["-DDRIVE_MAP"] + inc + ["-o", str(EXE_NEW), str(ROOT / "tools/drive_render.c"),
                    str(ROOT / "firmware/src/dsp/bass303.c"), "-lm"], check=True)
    subprocess.run([cc] + flags + [f"-I{old}"] + inc + ["-o", str(EXE_OLD), str(ROOT / "tools/drive_render.c"),
                    str(old / "bass303.c"), "-lm"], check=True)


def features(exe, typ, vol, drive, patch):
    """Return the third-octave band levels (dB) and the K-weighted level (dB) of a render (from the
    line's step 16, as drive_render writes it)."""
    raw = subprocess.run([str(exe), str(typ), str(vol), str(drive), str(patch)], capture_output=True,
                         check=True).stdout
    n = int(np.frombuffer(raw[:4], dtype=np.int32)[0])
    x = np.frombuffer(raw[4:4 + 4 * n], dtype=np.float32).astype(float)
    _, p = welch(x, FS, nperseg=NPS)
    bands = np.array([p[m].sum() for m in BAND]) + 1e-20
    return 10 * np.log10(bands), 10 * np.log10(np.mean(kweight(x) ** 2) + 1e-20)


def old_row(tvp):
    typ, v, patch = tvp
    return [features(EXE_OLD, typ, v, d, patch) for d in range(1, 128)]


def measure(typ, patch):
    """Render everything of drive type typ on a patch; return the shape errors of every old setting
    against every new Drive (dist[v, d, k]), the old K-weighted levels (old_ld[v, d]), the new ones at
    Volume 96 (new_ld[k]), and the band levels of the new Drives and of the old settings (new_db[k],
    old_db[v, d])."""
    new = [features(EXE_NEW, typ, 96, k, patch) for k in range(128)]
    new_db = np.array([f[0] for f in new]); new_ld = np.array([f[1] for f in new])
    dist = np.zeros((128, 128, 128)); old_ld = np.zeros((128, 128)); old_db = np.zeros((128, 128, len(BAND)))
    with ProcessPoolExecutor() as ex:
        for v, row in enumerate(ex.map(old_row, [(typ, v, patch) for v in range(128)])):
            for d, (o_db, o_ld) in enumerate(row, start=1):
                w = 10 ** (o_db / 10) * KBAND; w /= w.sum()   # what is heard: loudness-weighted bands
                diff = o_db - new_db
                diff -= (diff @ w)[:, None]                    # the level difference, Volume's job
                dist[v, d] = np.sqrt((diff * diff) @ w)
                old_ld[v, d] = o_ld
                old_db[v, d] = o_db
    return dist, old_ld, new_ld, new_db, old_db


def volume_for(old_ld, new_ld, v, d, k):
    """Return the new Volume that gives new Drive k old setting (v, d)'s level."""
    return int(np.clip(np.round(96 + (old_ld[v, d] - new_ld[k]) / STEP_DB), 0, 127))


def shape_dist(x, y):
    """Return the shape distance (dB) between band levels x and y (along their last axis): their
    difference weighted by the K-weighted loudness of both, the level difference removed."""
    w = (10 ** (x / 10) + 10 ** (y / 10)) * KBAND
    w = w / w.sum(axis=-1, keepdims=True)
    e = x - y
    e = e - (e * w).sum(axis=-1, keepdims=True)
    return np.sqrt((e * e * w).sum(axis=-1))


def fit(ms):
    """Return the Drive and Volume tables at the nodes, from every patch's measure()."""
    nv, nd = len(VNODES), len(DNODES)
    cost = np.zeros((nv, nd, 128))                  # a node's shape error for each new Drive
    for a, v in enumerate(VNODES):
        for b, d in enumerate(DNODES):
            cost[a, b] = np.mean([m[0][max(0, v - 4):v + 5, max(1, d - 4):d + 5].reshape(-1, 128).mean(axis=0)
                                  for m in ms], axis=0)
    new_apart = [shape_dist(m[3][:, None], m[3][None, :]) for m in ms]   # [j, k]: new Drives j and k

    def added(node1, node2):
        """The change in sound the migration adds between two nodes, [new Drive at 1, at 2], the
        mean over the patches."""
        old_apart = [shape_dist(m[4][node1], m[4][node2]) for m in ms]
        return np.mean([np.maximum(new_apart[i] - old_apart[i], 0) for i in range(len(ms))], axis=0)

    ks = np.arange(128)
    drv = np.zeros((nv, nd), dtype=int)
    for a in range(nv - 1, -1, -1):
        above = drv[a + 1] if a < nv - 1 else np.full(nd, 127)
        unary = cost[a].copy()
        for b in range(nd):
            if a < nv - 1:
                unary[b] += SMOOTH * added((VNODES[a], DNODES[b]), (VNODES[a + 1], DNODES[b]))[:, above[b]]
            unary[b, ks > above[b]] = np.inf
        total, back = unary[0], []
        for b in range(1, nd):
            step = SMOOTH * added((VNODES[a], DNODES[b - 1]), (VNODES[a], DNODES[b]))
            step[ks[:, None] > ks[None, :]] = np.inf          # nondecreasing along the row
            t = total[:, None] + step
            back.append(np.argmin(t, axis=0))
            total = t[back[-1], ks] + unary[b]
        k = int(np.argmin(total))
        for b in range(nd - 1, -1, -1):
            drv[a, b] = k
            if b:
                k = int(back[b - 1][k])
    vol = np.zeros((nv, nd), dtype=int)
    for a, v in enumerate(VNODES):
        for b, d in enumerate(DNODES):
            need = np.array([m[1][v, d] - m[2][drv[a, b]] for m in ms])
            vol[a, b] = int(np.clip(np.round(96 + (need.max() + need.min()) / 2 / STEP_DB), 0, 127))
    return drv, vol


def interp(tab, v, d):
    """bass303_pre_rescale's interpolation of a node table at old (v, d), d > 0."""
    a = min(np.searchsorted(VNODES, v, side="right") - 1, len(VNODES) - 2)
    b = min(np.searchsorted(DNODES, d, side="right") - 1, len(DNODES) - 2)
    fv, wv = v - VNODES[a], VNODES[a + 1] - VNODES[a]
    fd, wd = d - DNODES[b], DNODES[b + 1] - DNODES[b]
    x = (int(tab[a, b]) * (wd - fd) + int(tab[a, b + 1]) * fd) * (wv - fv) \
        + (int(tab[a + 1, b]) * (wd - fd) + int(tab[a + 1, b + 1]) * fd) * fv
    return (2 * x + wv * wd) // (2 * wv * wd)


def report(name, nd, nv, dist, old_ld, new_ld):
    v, d = np.meshgrid(np.arange(128), np.arange(1, 128), indexing="ij")
    shape = dist[v, d, nd[v, d]]
    level = np.abs(old_ld[v, d] - (new_ld[nd[v, d]] + (nv[v, d] - 96) * STEP_DB))
    loud = v >= 48
    print(f"{name}: shape error (dB) median {np.median(shape):.2f}, 90% {np.percentile(shape, 90):.2f}, "
          f"at old Volume >= 48 median {np.median(shape[loud]):.2f}, 90% {np.percentile(shape[loud], 90):.2f}; "
          f"level error (dB) median {np.median(level):.2f}, max {level.max():.2f}")


def patch_names(exe=EXE_NEW):
    """Return the names of drive_render's patches (tools/drive_line.h's DRIVE_PATCHES), in order."""
    return subprocess.run([str(exe), "patches"], capture_output=True, check=True, text=True).stdout.split()


def firmware_map(typ):
    out = subprocess.run([str(EXE_NEW), "map", str(typ)], capture_output=True, check=True, text=True).stdout.split()
    m = np.array(out, dtype=int).reshape(128, 128, 2)
    return m[:, :, 0], m[:, :, 1]


def c_table(name, tab):
    rows = ",\n".join("    {" + ", ".join(f"{x:3d}" for x in row) + "}" for row in tab)
    return f"static const uint8_t {name}[{len(VNODES)}][{len(DNODES)}] = {{\n{rows},\n}};"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--check", action="store_true", help="measure the firmware's mapping as it is")
    args = ap.parse_args()
    build()
    patches = patch_names()
    for typ, name in ((RAT, "RAT"), (SOFT, "Soft")) if args.check else ((RAT, "RAT"),):
        ms = [measure(typ, p) for p in range(len(patches))]
        for (dist, old_ld, new_ld, *_), pname in zip(ms, patches):
            best = np.argmin(dist, axis=2)
            bestv = np.array([[volume_for(old_ld, new_ld, v, d, best[v, d]) if d else v for d in range(128)]
                              for v in range(128)])
            report(f"{name} {pname}, per-setting optimum", best, bestv, dist, old_ld, new_ld)
            same = np.tile(np.arange(128), (128, 1))
            report(f"{name} {pname}, Drive and Volume kept as they were", same,
                   np.tile(np.arange(128)[:, None], (1, 128)), dist, old_ld, new_ld)
            if args.check:
                nd, nv = firmware_map(typ)
                report(f"{name} {pname}, the firmware's mapping", nd, nv, dist, old_ld, new_ld)
    if args.check:
        return
    drv, vol = fit(ms)
    nd = np.array([[interp(drv, v, d) if d else 0 for d in range(128)] for v in range(128)])
    nv = np.array([[interp(vol, v, d) if d else v for d in range(128)] for v in range(128)])
    for (dist, old_ld, new_ld, *_), pname in zip(ms, patches):
        report(f"RAT {pname}, the fitted tables", nd, nv, dist, old_ld, new_ld)
    print(f"\n/* old Volume nodes {VNODES}; old Drive nodes {DNODES} */")
    print(c_table("rat_mig_drive", drv))
    print(c_table("rat_mig_volume", vol))


if __name__ == "__main__":
    main()
