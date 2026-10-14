#!/usr/bin/env python3
"""Convert resources/logo/logo.png -> src/cyd/logo.h (RGB565, scaled to fit the CYD).

The CYD is a 240x320 portrait TFT driven by TFT_eSPI; it has no PNG decoder, so we
bake the logo into a PROGMEM uint16_t array and blit it with tft.pushImage().
Transparent pixels are composited over black (the splash background)."""
import sys
from PIL import Image

SRC = sys.argv[1]
DST = sys.argv[2]
TARGET_W = int(sys.argv[3]) if len(sys.argv) > 3 else 240

im = Image.open(SRC).convert("RGBA")
w0, h0 = im.size
target_h = round(h0 * TARGET_W / w0)
im = im.resize((TARGET_W, target_h), Image.LANCZOS)
W, H = im.size
px = im.load()

def to565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

words = []
for y in range(H):
    for x in range(W):
        r, g, b, a = px[x, y]
        # composite over black
        r = r * a // 255
        g = g * a // 255
        b = b * a // 255
        words.append(to565(r, g, b))

with open(DST, "w") as f:
    f.write("// Auto-generated from resources/logo/logo.png by scratchpad/gen_logo.py.\n")
    f.write("// COINTEL-FLO boot splash wordmark, RGB565, composited over black.\n")
    f.write("// Blit with: tft.pushImage((240-LOGO_W)/2, y, LOGO_W, LOGO_H, COINTEL_LOGO);\n")
    f.write("#pragma once\n#include <pgmspace.h>\n#include <stdint.h>\n\n")
    f.write(f"static constexpr int LOGO_W = {W};\n")
    f.write(f"static constexpr int LOGO_H = {H};\n\n")
    f.write("static const uint16_t COINTEL_LOGO[] PROGMEM = {\n")
    for i in range(0, len(words), 12):
        row = ", ".join(f"0x{v:04X}" for v in words[i:i+12])
        f.write("  " + row + ",\n")
    f.write("};\n")

print(f"wrote {DST}: {W}x{H} = {len(words)} px = {len(words)*2} bytes")
