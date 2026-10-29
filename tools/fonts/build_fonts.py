#!/usr/bin/env python3
"""Rebuild the embedded UI fonts (src/fonts/*.h) for LVGL tiny_ttf.

  pip install fonttools
  python3 tools/fonts/build_fonts.py

Sources (both SIL Open Font License), kept next to this script:
  Orbitron.ttf  (variable)  -> static wght 500, dotted zero  -> hero numerals
  Rajdhani.ttf  (SemiBold)                                    -> UI text
Both are subset to Latin + German umlauts + typographic marks.
"""
import os
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.pens.transformPen import TransformPen
from fontTools.pens.recordingPen import RecordingPen
from fontTools import subset

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "src", "fonts")
GLYPHS = ("0123456789:.,;-+%/ °·•–—…~ABCDEFGHIJKLMNOPQRSTUVWXYZ"
          "abcdefghijklmnopqrstuvwxyzäöüÄÖÜß€!?#()[]'\"&@*=<>_|")


def orbitron_static():
    st = instancer.instantiateVariableFont(TTFont(os.path.join(HERE, "Orbitron.ttf")), {"wght": 500})
    glyf = st["glyf"]
    # winding of the outer contour of "O"
    rec = RecordingPen()
    glyf["O"].draw(rec, glyf)
    pts = []
    for op, args in rec.value:
        if op in ("moveTo", "lineTo"):
            pts.append(args[0])
        elif op == "qCurveTo":
            pts.append(args[-1])
        if op in ("closePath", "endPath"):
            break
    area = sum(pts[i][0] * pts[(i + 1) % len(pts)][1] - pts[(i + 1) % len(pts)][0] * pts[i][1]
               for i in range(len(pts)))
    # Orbitron's slashed zero reads as "Ø" on a watch: rebuild it from "O"
    # (same metrics) plus a centre dot.
    pen = TTGlyphPen(None)
    glyf["O"].draw(TransformPen(pen, (1, 0, 0, 1, 3, 0)), glyf)
    cx, cy, r = 417, 360, 52
    rect = [(cx - r, cy - r), (cx + r, cy - r), (cx + r, cy + r), (cx - r, cy + r)]
    if area < 0:
        rect = rect[::-1]
    pen.moveTo(rect[0])
    for p in rect[1:]:
        pen.lineTo(p)
    pen.closePath()
    glyf["zero"] = pen.glyph()
    glyf["zero"].recalcBounds(glyf)
    st["hmtx"]["zero"] = (834, glyf["zero"].xMin)
    return st


def do_subset(font):
    opts = subset.Options()
    opts.hinting = False
    opts.drop_tables += ["GSUB", "GPOS"]
    sub = subset.Subsetter(opts)
    sub.populate(text=GLYPHS)
    sub.subset(font)
    return font


def emit(font, name, path, note):
    tmp = path + ".ttf.tmp"
    font.save(tmp)
    data = open(tmp, "rb").read()
    os.remove(tmp)
    with open(path, "w") as f:
        f.write("#pragma once\n#include <stdint.h>\n")
        f.write(f"// {note} ({len(data)} bytes)\n// Rebuild: see tools/fonts/build_fonts.py\n")
        f.write(f"static const uint8_t {name}[] = {{\n")
        for i in range(0, len(data), 16):
            f.write("  " + ",".join(str(b) for b in data[i:i + 16]) + ",\n")
        f.write("};\n")
        f.write(f"static const unsigned int {name}_len = {len(data)};\n")
    print(f"{path}: {len(data)} bytes")


if __name__ == "__main__":
    emit(do_subset(orbitron_static()), "orbitron_ttf", os.path.join(OUT, "font_orbitron.h"),
         "Orbitron (OFL) static wght=500, dotted zero, Latin subset")
    emit(do_subset(TTFont(os.path.join(HERE, "Rajdhani.ttf"))), "rajdhani_ttf",
         os.path.join(OUT, "font_rajdhani.h"), "Rajdhani SemiBold (OFL), Latin subset")
