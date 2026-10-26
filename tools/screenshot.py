#!/usr/bin/env python3
"""Turn a WC_SCREENSHOT dump (768-byte 6-bit VGA palette + 320x200 pixels) into a PNG.
Usage: screenshot.py dump.raw out.png [scale]"""
import struct, sys, zlib

raw = open(sys.argv[1], "rb").read()
scale = int(sys.argv[3]) if len(sys.argv) > 3 else 2
pal = [tuple(min(255, c * 255 // 63) for c in raw[i * 3:i * 3 + 3]) for i in range(256)]
pix = raw[768:768 + 64000]
rows = []
for y in range(200):
    line = bytearray()
    for x in range(320):
        line += bytes(pal[pix[y * 320 + x]]) * scale
    rows += [b"\0" + bytes(line)] * scale
w, h = 320 * scale, 200 * scale

def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)

png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b"")
open(sys.argv[2], "wb").write(png)
