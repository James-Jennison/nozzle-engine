#!/usr/bin/env python3
"""Checks the color a sliced G-code file's thumbnail draws its model in.

    thumbnail_color.py <file.gcode> <#RRGGBB>

The bridge's thumbnail renderer (nozzle/bridge/android/thumbnail_render.cpp) flat-shades every face as
base * shade + highlight, with shade in [0.35, 1] and highlight = 32 * (shade - 0.35) / 0.65. This decodes the largest
embedded thumbnail, takes its most common opaque pixel (one face of the model) and exits 0 when that pixel is the given
base color under some shade, 1 when it is not. Standard library only.
"""
import base64
import re
import struct
import sys
import zlib
from collections import Counter


def decode_png(png):
    """Rows of an 8-bit RGB or RGBA PNG."""
    if png[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, header = 8, b"", None
    while pos < len(png):
        length, kind = struct.unpack(">I4s", png[pos:pos + 8])
        body = png[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
    width, height, depth, color_type, _, _, interlace = header
    if depth != 8 or color_type not in (2, 6) or interlace != 0:
        raise ValueError(f"unsupported PNG (depth {depth}, color type {color_type}, interlace {interlace})")
    bpp = 4 if color_type == 6 else 3
    raw, stride = zlib.decompress(idat), width * bpp
    rows, prev = [], bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        filter_type, line = raw[start], bytearray(raw[start + 1:start + 1 + stride])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if filter_type == 1:
                line[i] = (line[i] + a) & 255
            elif filter_type == 2:
                line[i] = (line[i] + b) & 255
            elif filter_type == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif filter_type == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return width, bpp, rows


def most_common_opaque_pixel(gcode_path):
    text = open(gcode_path, errors="replace").read()
    blocks = re.findall(r"; thumbnail begin (\d+)x(\d+) \d+\n(.*?); thumbnail end", text, re.S)
    if not blocks:
        raise ValueError("no thumbnail in the G-code")
    _, _, encoded = max(blocks, key=lambda block: int(block[0]) * int(block[1]))
    png = base64.b64decode("".join(line.lstrip("; ").strip() for line in encoded.splitlines()))
    width, bpp, rows = decode_png(png)
    count = Counter()
    for line in rows:
        for x in range(width):
            pixel = tuple(line[x * bpp:x * bpp + bpp])
            if bpp == 3 or pixel[3] == 255:
                count[pixel[:3]] += 1
    if not count:
        raise ValueError("the thumbnail has no opaque pixel")
    return count.most_common(1)[0][0]


def shade_fit(pixel, base):
    """(shade, worst channel error) of the shade that best explains `pixel` as the renderer's shading of `base`."""
    k = 32 / 0.65
    shade = sum((p + 0.35 * k) * (b + k) for p, b in zip(pixel, base)) / sum((b + k) ** 2 for b in base)
    shade = min(1.0, max(0.35, shade))
    error = max(abs(p - min(255.0, b * shade + k * (shade - 0.35))) for p, b in zip(pixel, base))
    return shade, error


def main():
    if len(sys.argv) != 3 or not re.fullmatch(r"#[0-9a-fA-F]{6}", sys.argv[2]):
        print(__doc__, file=sys.stderr)
        return 2
    base = tuple(int(sys.argv[2][i:i + 2], 16) for i in (1, 3, 5))
    try:
        pixel = most_common_opaque_pixel(sys.argv[1])
    except ValueError as problem:
        print(f"thumbnail_color: {problem}", file=sys.stderr)
        return 2
    shade, error = shade_fit(pixel, base)
    print(f"thumbnail pixel {pixel} against {sys.argv[2]}: shade {shade:.3f}, worst channel error {error:.1f}")
    return 0 if error <= 2.0 else 1


if __name__ == "__main__":
    sys.exit(main())
