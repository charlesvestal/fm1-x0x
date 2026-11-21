#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Measure the 303 drive's makeup tables (firmware/src/dsp/bass303.c: makeup_soft, makeup_rat): for
each Drive value, the gain after the drive that keeps the 303's K-weighted (BS.1770) level at Drive 0's,
before the deliberate rise (DRV_RISE), which bass303.c applies on top. Run it after changing what the
drive does to the level: Soft's input gain, the RAT's taper or its tone filter, the patch measured on.

  tools/measure_drive_makeup.py                 print both tables, measured on the acid patch
  tools/measure_drive_makeup.py --type rat      only one
  tools/measure_drive_makeup.py --patch 3       on another of tools/drive_line.h's DRIVE_PATCHES
  tools/measure_drive_makeup.py --every-value   at every Drive value instead of the default points

The 303 plays tools/drive_line.h's acid line (tools/drive_render.c) at Volume 96 through this
firmware's bass303.c as it is, its makeup and rise included; both are gains after the drive, so
taking them out of the measured level leaves what the drive itself does, and the table needs no
special build. The level is measured at a set of Drive values (denser at the bottom of the knob, and
for RAT around the tone filter's closing point, Drive 33, where the level bends) and interpolated in
dB between them: these are the points the committed tables were measured at, so with the firmware
unchanged it prints them as they are. With --every-value it measures all 127 (within 0.05 dB of the
interpolated tables, but every entry moves, and so do the dsp_hash scenarios that run the drive).

After pasting new tables: tests/host/bass303_drive_test.c checks them (on the acid patch), the RAT
migration needs refitting (tools/fit_drive_migration.py; Soft's migration follows the table by
itself), and the driven dsp_hash scenarios need re-recording (dsp_hash_test GOLDEN --write).

Needs numpy and scipy (pip3 install numpy scipy) and a host C compiler ($CC, default cc)."""
import argparse
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

try:
    import numpy as np
    from fit_drive_migration import kweight, patch_names
except ImportError:
    sys.exit("measure_drive_makeup.py needs numpy and scipy: pip3 install numpy scipy")

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "firmware/src/dsp/bass303.c"
EXE = ROOT / "build/host/drive_render_makeup"
SOFT, RAT = 1, 2
POINTS = {
    SOFT: [0, 1, 2, 4, 6, 8, 10, 12, 16, 20, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96, 104, 112, 120, 127],
    RAT: [0, 1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24, 28, 32, 33, 36, 40, 48, 56, 64, 72, 80, 88, 96, 104,
          112, 120, 127],
}


def build():
    cc = os.environ.get("CC", "cc")
    EXE.parent.mkdir(parents=True, exist_ok=True)
    inc = [f"-I{ROOT}/firmware/src", f"-I{ROOT}/firmware/src/dsp", f"-I{ROOT}/build/gen"]
    subprocess.run([cc, "-O2", "-ffp-contract=off", "-w", "-DX0X_HOST"] + inc + ["-o", str(EXE),
                    str(ROOT / "tools/drive_render.c"), str(SRC), "-lm"], check=True)


def firmware_tables():
    """Return bass303.c's makeup tables (name -> 128 gains) and DRV_RISE (dB)."""
    src = SRC.read_text()
    tabs = {}
    for name in ("soft", "rat"):
        body = re.search(rf"makeup_{name}\[128\] = \{{(.*?)\}};", src, re.S).group(1)
        tabs[name] = np.array([float(x) for x in re.findall(r"(-?[\d.]+)f", body)])
        assert len(tabs[name]) == 128, name
    return tabs, float(re.search(r"#define DRV_RISE (-?[\d.]+)f", src).group(1))


def level(typ, drive, patch):
    """Return the K-weighted level (dB) of the 303 at a Drive value."""
    raw = subprocess.run([str(EXE), str(typ), "96", str(drive), str(patch)], capture_output=True,
                         check=True).stdout
    n = int(np.frombuffer(raw[:4], dtype=np.int32)[0])
    x = np.frombuffer(raw[4:4 + 4 * n], dtype=np.float32).astype(float)
    return 10 * np.log10(np.mean(kweight(x) ** 2) + 1e-20)


def measure(typ, cur, rise, drives, patch):
    """Return the makeup table (128 gains, rounded as bass303.c holds them) for drive type typ."""
    with ThreadPoolExecutor() as ex:
        lv = list(ex.map(lambda d: level(typ, d, patch), drives))
    # the level without the makeup and rise the firmware applied
    raw = [lv[i] - 20 * np.log10(cur[d]) - rise * d / 127 for i, d in enumerate(drives)]
    db = np.interp(np.arange(128), drives, [0.0] + [raw[0] - r for r in raw[1:]])
    return np.round(10 ** (db / 20), 4)


def c_table(name, tab):
    rows = "".join("    " + " ".join(f"{x:.4f}f," for x in tab[i:i + 8]) + "\n" for i in range(0, 128, 8))
    return f"static const float makeup_{name}[128] = {{\n{rows}}};"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--type", choices=("soft", "rat", "both"), default="both")
    ap.add_argument("--patch", type=int, default=0, help="an index into tools/drive_line.h's DRIVE_PATCHES (0)")
    ap.add_argument("--every-value", action="store_true",
                    help="measure every Drive value, not the default points")
    args = ap.parse_args()
    build()
    patches = patch_names(EXE)
    if not 0 <= args.patch < len(patches):
        ap.error(f"--patch 0..{len(patches) - 1}: {', '.join(patches)}")
    cur, rise = firmware_tables()
    for typ, name in ((SOFT, "soft"), (RAT, "rat")):
        if args.type not in (name, "both"):
            continue
        drives = list(range(128)) if args.every_value else POINTS[typ]
        tab = measure(typ, cur[name], rise, drives, args.patch)
        diff = np.abs(20 * np.log10(tab / cur[name]))
        print(f"/* {name}, on the {patches[args.patch]} patch: {int(np.sum(tab != cur[name]))} entries differ "
              f"from bass303.c's, by at most {diff.max():.3f} dB (Drive {int(diff.argmax())}) */")
        print(c_table(name, tab))


if __name__ == "__main__":
    main()
