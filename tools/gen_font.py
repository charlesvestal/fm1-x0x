#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""Render the bitmap fonts to a C header (assets/fonts/):
  XS / S / B / M / L: Terminus 12, 16, 16 bold, 24 bold, 32 bold (BDF, SIL OFL 1.1); see SIZES. TTF fonts also work through render()
(anti-aliased, tabular figures via the OpenType `tnum` feature).

Glyph format: per glyph an advance width, a bitmap width and an offset; the
bitmap starts FONT_PAD pixels left of the pen position (room for side
bearings); rows top to bottom, 2 pixels per byte (high nibble first),
alpha 0..15.
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

FONTS = Path(__file__).resolve().parents[1] / "assets" / "fonts"
# (name, file, px, scale, pixel, first, last): pixel fonts are rendered without
# anti-aliasing at their design size and enlarged by an integer factor
# X0X: six roles, T (the footer: capitals, digits), XS (grid labels, units, hints), S (body), B (headers, names, the selected
# row), M (knob values) and L (the big readout: numbers only, space to '9', to keep its size down). Each set gives them a face and a pixel size,
# chosen so the line height stays near the layout's 12 / 16 / 24 / 32. X0X_FONTSET picks one.
# A TTF entry "file.ttf@wght=600" sets a variable font's axis.
FONTSETS = {
    "terminus": [("T", "ter-u12n.bdf", 12, 1, True, 32, 95),
                 ("XS", "ter-u12n.bdf", 12, 1, True, 32, 126),
                 ("S", "ter-u16n.bdf", 16, 1, True, 32, 126),
                 ("B", "ter-u16b.bdf", 16, 1, True, 32, 126),
                 ("M", "ter-u24b.bdf", 24, 1, True, 32, 95),
                 ("L", "ter-u32b.bdf", 32, 1, True, 32, 57)],
    "barlow": [("T", "BarlowSemiCondensed-SemiBold.ttf", 10, 1, False, 32, 95),
               ("XS", "BarlowSemiCondensed-SemiBold.ttf", 12, 1, False, 32, 126),
               ("S", "BarlowSemiCondensed-Medium.ttf", 15, 1, False, 32, 126),
               ("B", "BarlowSemiCondensed-Bold.ttf", 15, 1, False, 32, 126),
               ("M", "BarlowSemiCondensed-SemiBold.ttf", 23, 1, False, 32, 95),
               ("L", "BarlowSemiCondensed-Bold.ttf", 31, 1, False, 32, 57)],
    "inter": [("T", "Inter[opsz,wght].ttf@wght=600", 9, 1, False, 32, 95),
              ("XS", "Inter[opsz,wght].ttf@wght=600", 11, 1, False, 32, 126),
              ("S", "Inter[opsz,wght].ttf@wght=500", 14, 1, False, 32, 126),
              ("B", "Inter[opsz,wght].ttf@wght=700", 14, 1, False, 32, 126),
              ("M", "Inter[opsz,wght].ttf@wght=650", 21, 1, False, 32, 95),
              ("L", "Inter[opsz,wght].ttf@wght=700", 29, 1, False, 32, 57)],
    "rajdhani": [("XS", "Rajdhani-Bold.ttf", 14, 1, False, 32, 126),
                 ("S", "Rajdhani-SemiBold.ttf", 17, 1, False, 32, 126),
                 ("B", "Rajdhani-Bold.ttf", 17, 1, False, 32, 126),
                 ("M", "Rajdhani-Bold.ttf", 26, 1, False, 32, 95),
                 ("L", "Rajdhani-Bold.ttf", 35, 1, False, 32, 95)],
    "chakra": [("XS", "ChakraPetch-SemiBold.ttf", 11, 1, False, 32, 126),
               ("S", "ChakraPetch-Medium.ttf", 14, 1, False, 32, 126),
               ("B", "ChakraPetch-Bold.ttf", 14, 1, False, 32, 126),
               ("M", "ChakraPetch-SemiBold.ttf", 21, 1, False, 32, 95),
               ("L", "ChakraPetch-Bold.ttf", 29, 1, False, 32, 95)],
}
import os  # noqa: E402
FONTSET = os.environ.get("X0X_FONTSET", "barlow")    # the others need X0X_FONT_DIRS (not in assets/)
FONTDIRS = [Path(p) for p in os.environ.get("X0X_FONT_DIRS", "").split(":") if p]
SIZES = FONTSETS[FONTSET]
PAD = 2


def upscale(px_, w, h, scale):
    """integer nearest-neighbour enlargement of a w x h glyph"""
    if scale == 1:
        return px_
    big = []
    for y in range(h):
        row = []
        for x in range(w):
            row += [px_[y * w + x]] * scale
        big += row * scale
    return big


def render_bdf(path, scale, first, last):
    """BDF bitmap font (Terminus 8x16): exact pixels, cell = the font bounding box."""
    lines = path.read_text(errors="replace").splitlines()
    fbb = next(l for l in lines if l.startswith("FONTBOUNDINGBOX")).split()
    cw, ch, fx, fy = int(fbb[1]), int(fbb[2]), int(fbb[3]), int(fbb[4])
    base = ch + fy                                     # rows above the baseline
    glyphs, i = {}, 0
    while i < len(lines):
        if lines[i].startswith("ENCODING"):
            code = int(lines[i].split()[1])
            j = i
            while not lines[j].startswith("BBX"):
                j += 1
            bw, bh, bx, by = map(int, lines[j].split()[1:5])
            dw = cw
            k = i
            while not lines[k].startswith("BITMAP"):
                if lines[k].startswith("DWIDTH"):
                    dw = int(lines[k].split()[1])
                k += 1
            rows = [int(r, 16) for r in lines[k + 1:k + 1 + bh]]
            nbits = ((bw + 7) // 8) * 8
            cell = [0] * (cw * ch)
            top = base - (by + bh)
            for r, bits in enumerate(rows):
                for c in range(bw):
                    if bits >> (nbits - 1 - c) & 1:
                        x, y = bx + c, top + r
                        if 0 <= x < cw and 0 <= y < ch:
                            cell[y * cw + x] = 15
            glyphs[code] = (dw, cell)
            i = k + bh
        i += 1
    if last >= 0xDC and 0xDC not in glyphs and ord("U") in glyphs:      # Ü for "HÜGELTON": U with dots on its top row
        dw, cell = glyphs[ord("U")]
        cell = cell[:]
        lit = [x for y in range(ch) for x in range(cw) if cell[y * cw + x]]
        if lit:
            top = min(y for y in range(ch) for x in range(cw) if cell[y * cw + x])
            for x in range(cw):
                cell[top * cw + x] = 15 if x in (min(lit), max(lit)) else 0
            for x in range(cw):
                cell[(top + 1) * cw + x] = 0
            glyphs[0xDC] = (dw, cell)
    missing = [chr(c) for c in range(first, min(last, 126) + 1) if c not in glyphs]
    if missing:
        print(f"font {path.name}: no glyph for {''.join(missing)!r} (drawn as '?')")
    out = []
    for c in range(first, last + 1):
        dw, cell = glyphs.get(c, glyphs[ord("?")])
        bw = cw + 2 * PAD                              # keep the PAD convention of the TTF path
        px_ = []
        for y in range(ch):
            px_ += [0] * PAD + cell[y * cw:(y + 1) * cw] + [0] * PAD
        out.append((dw * scale, bw * scale, upscale(px_, bw, ch, scale)))
    return ch * scale, out


def font_path(name):
    for d in [FONTS, *FONTDIRS]:
        if (d / name).exists():
            return d / name
    raise SystemExit(f"gen_font: {name} not in {FONTS} or X0X_FONT_DIRS")


def render(ttf, px, scale, pixel, first, last):
    if ttf.endswith(".bdf"):
        return render_bdf(font_path(ttf), scale, first, last)
    name, _, var = ttf.partition("@")
    font = ImageFont.truetype(str(font_path(name)), px)
    if var:                                            # a variable font: "wght=600"
        axes = dict(kv.split("=") for kv in var.split(","))
        names = [a["name"] if isinstance(a["name"], str) else a["name"].decode() for a in font.get_variation_axes()]
        font.set_variation_by_axes([float(axes.get(n.lower()[:4], axes.get(n, 0)) or a["default"])
                                    for n, a in zip(names, font.get_variation_axes())])
    feats = None if pixel else ["tnum"]
    asc, desc = font.getmetrics()
    top = max(0, font.getbbox("A8|(", features=feats)[1] - 1)
    h = asc + desc - top
    glyphs = []
    for c in range(first, last + 1):
        ch = chr(c)
        adv = int(round(font.getlength(ch, features=feats)))
        bw = adv + 2 * PAD
        img = Image.new("L", (bw, asc + desc), 0)
        ImageDraw.Draw(img).text((PAD, 0), ch, font=font, fill=255, features=feats)
        img = img.crop((0, top, bw, top + h))
        px_ = [(15 if v >= 128 else 0) if pixel else min(15, (v + 8) // 17) for v in img.tobytes()]
        glyphs.append((adv * scale, bw * scale, upscale(px_, bw, h, scale)))
    return h * scale, glyphs


def main(out):
    lines = [f"/* generated by tools/gen_font.py: font set {FONTSET} */",
             "#pragma once", "#include <stdint.h>", f"#define FONT_PAD {PAD}  /* x scale for L, see below */", ""]
    for name, ttf, px, scale, pixel, first, last in SIZES:
        h, glyphs = render(ttf, px, scale, pixel, first, last)
        data, offs = [], []
        for adv, bw, g in glyphs:
            offs.append(len(data))
            for y in range(h):
                row = g[y * bw:(y + 1) * bw] + [0]
                for x in range(0, bw, 2):
                    data.append((row[x] << 4) | row[x + 1])
        assert len(data) < 65536
        lines.append(f"static const uint8_t FONT_{name}_DATA[{len(data)}] = {{")
        for i in range(0, len(data), 24):
            lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 24]) + ",")
        lines.append("};")
        lines.append(f"static const uint16_t FONT_{name}_OFF[{len(offs)}] = {{" + ", ".join(map(str, offs)) + "};")
        lines.append(f"static const uint8_t FONT_{name}_ADV[{len(glyphs)}] = {{" +
                     ", ".join(str(a) for a, _, _ in glyphs) + "};")
        lines.append(f"static const uint8_t FONT_{name}_BW[{len(glyphs)}] = {{" +
                     ", ".join(str(b) for _, b, _ in glyphs) + "};")
        lines.append(f"static const felucca_font_t FONT_{name} = {{ {h}, {PAD * scale}, {first}, {last}, "
                     f"FONT_{name}_ADV, FONT_{name}_BW, FONT_{name}_OFF, FONT_{name}_DATA }};")
        lines.append("")
        print(f"font {name}: {ttf} {px}px h {h}, digit adv {glyphs[ord('0') - first][0]}, {len(data)} B")
    print(f"font set {FONTSET}: {sum(len(l) for l in lines if l.startswith('    0x')) // 6} B (approx)")
    Path(out).write_text("\n".join(lines))


if __name__ == "__main__":
    main(sys.argv[1])
