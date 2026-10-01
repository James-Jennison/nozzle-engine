#!/usr/bin/env python3
"""Checks that a sliced G-code file prints inside the printer's printable area.

    within_bed.py <file.gcode>

Reads printable_area from the G-code's own config block and follows every extruding G1 move, grouped by feature
(";TYPE:" lines, or Bambu's "; FEATURE: "). The printer's own start and end G-code ("Custom") is not checked: a purge
line may sit off the printable area on purpose. Prints the prime tower's extents and exits 0 when everything is inside,
1 when a feature reaches past the area (each one is listed) or the file has no prime tower. Standard library only.
"""
import re
import sys


def main():
    text = open(sys.argv[1], errors="replace").read()
    area = re.findall(r"(?m)^; printable_area = (.*)$", text)
    if not area:
        print("no printable_area in the G-code config")
        return 1
    points = [tuple(float(v) for v in p.split("x")) for p in area[-1].split(",")]
    bed = (min(p[0] for p in points), min(p[1] for p in points), max(p[0] for p in points), max(p[1] for p in points))
    feature, x, y, extents = None, 0.0, 0.0, {}
    for line in text.splitlines():
        if line.startswith(";TYPE:"):
            feature = line[6:]
        elif line.startswith("; FEATURE: "):
            feature = line[11:]
        if not line.startswith("G1"):
            continue
        mx, my, me = (re.search(r"%s(-?[0-9.]+)" % axis, line) for axis in "XYE")
        nx, ny = float(mx.group(1)) if mx else x, float(my.group(1)) if my else y
        if feature and me and float(me.group(1)) > 0 and (mx or my):
            box = extents.setdefault(feature, [nx, ny, nx, ny])
            for px, py in ((x, y), (nx, ny)):
                box[0], box[1], box[2], box[3] = min(box[0], px), min(box[1], py), max(box[2], px), max(box[3], py)
        x, y = nx, ny
    outside = [(f, b) for f, b in sorted(extents.items())
               if f != "Custom" and (b[0] < bed[0] or b[1] < bed[1] or b[2] > bed[2] or b[3] > bed[3])]
    for f, b in outside:
        print("%s reaches x %.2f..%.2f y %.2f..%.2f, outside the printable area x %g..%g y %g..%g"
              % (f, b[0], b[2], b[1], b[3], bed[0], bed[2], bed[1], bed[3]))
    tower = extents.get("Prime tower")
    if tower is None:
        print("no prime tower in the G-code")
        return 1
    if not outside:
        print("prime tower x %.2f..%.2f y %.2f..%.2f inside x %g..%g y %g..%g" % (tower[0], tower[2], tower[1], tower[3], bed[0], bed[2], bed[1], bed[3]))
    return 1 if outside else 0


if __name__ == "__main__":
    sys.exit(main())
