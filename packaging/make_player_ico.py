#!/usr/bin/env python3
"""
Writes resources/player.ico: the placeholder icon FlyPlayer.exe is built with. Export (src/Engine/Export/PeIcon.cpp)
overwrites its images in place with the game's icon, so every image must be an uncompressed 32-bit DIB (never PNG)
at a fixed size. Re-run only to change the sizes or the placeholder picture.

    packaging/make_player_ico.py [source.png] [out.ico]      (needs Pillow)
"""
import os, struct, sys
from PIL import Image

SIZES = (16, 32, 48, 256)
here = os.path.dirname(os.path.abspath(__file__))
src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "assets", "FlyengineLogo.png")
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, "..", "resources", "player.ico")

def dib(img, n):
    img = img.convert("RGBA").resize((n, n), Image.LANCZOS)
    px = img.load()
    d = struct.pack("<IiiHHIIiiII", 40, n, n * 2, 1, 32, 0, n * n * 4, 0, 0, 0, 0)
    for y in range(n - 1, -1, -1):
        for x in range(n):
            r, g, b, a = px[x, y]
            d += bytes((b, g, r, a))
    return d + bytes(((n + 31) // 32) * 4 * n)

img = Image.open(src)
images = [dib(img, n) for n in SIZES]
data = struct.pack("<HHH", 0, 1, len(SIZES))
offset = 6 + 16 * len(SIZES)
for n, d in zip(SIZES, images):
    data += struct.pack("<BBBBHHII", n % 256, n % 256, 0, 0, 1, 32, len(d), offset)
    offset += len(d)
open(out, "wb").write(data + b"".join(images))
print("wrote", out, [len(d) for d in images])
